// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mapping.hpp"

#include <windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <xinput.h>

#include <g6shim/log.hpp>
#include <g6shim/settings.hpp>
#include <g6shim/window.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>

namespace g6shim::input {

namespace {

constexpr char kMappingFile[] = "g6shim_input.ini";
constexpr char kAutopressVariable[] = "G6SHIM_INPUT_AUTOPRESS";
constexpr wchar_t kXInputDll[] = L"xinput1_4.dll";
constexpr char kXInputGetState[] = "XInputGetState";
constexpr std::size_t kMaxSpec = 512;
constexpr std::size_t kIniSectionBytes = 32768;
constexpr std::size_t kControllerButtons = 32;
constexpr LONG kXInputStickRange = 32767;
constexpr float kXInputTriggerRange = 255.0f;
constexpr LONG kDirectInputRange = 1000;
constexpr DWORD kDirectInputRetryMs = 2000;
constexpr BYTE kDirectInputPressed = 0x80;
constexpr SHORT kKeyDown = static_cast<SHORT>(0x8000);
constexpr int kFunctionKeys = 12;

constexpr std::uint32_t kPovNorthEast = 4500;
constexpr std::uint32_t kPovSouthEast = 13500;
constexpr std::uint32_t kPovSouthWest = 22500;
constexpr std::uint32_t kPovNorthWest = 31500;

constexpr char kDefaultMapping[] =
    "; g6shim input mapping. Edit and restart the game.\r\n"
    "; Gamepad buttons: A B X Y Menu View Up Down Left Right LB RB LS RS LT RT\r\n"
    "; Several per line, separated by spaces, e.g.  Enter = A Menu\r\n"
    "; XInput controllers need no mapping.\r\n"
    "\r\n"
    "[keyboard]\r\n"
    "; Keys: Up Down Left Right Enter Space Backspace Escape Tab Shift Ctrl Alt,\r\n"
    "; letters A-Z, digits 0-9, F1-F12. The arrow keys also move the left stick.\r\n"
    "Up = Up\r\nDown = Down\r\nLeft = Left\r\nRight = Right\r\n"
    "Enter = A Menu\r\nBackspace = B\r\nA = X\r\nS = Y\r\nQ = LB\r\nW = RB\r\nTab = View\r\n"
    "\r\n"
    "[controller]\r\n"
    "; Other controllers (DirectInput): button number = gamepad buttons. g6shim.log\r\n"
    "; shows the number of every button you press. The D-pad (hat) always works.\r\n"
    "; Defaults: the usual layout of pads with four face buttons: 0 left, 1 bottom,\r\n"
    "; 2 right, 3 top, 4/5 shoulders, 6/7 triggers, 8/9 the two centre buttons,\r\n"
    "; 10/11 stick clicks, 12 home, 13 touch pad.\r\n"
    "0 = X\r\n1 = A\r\n2 = B\r\n3 = Y\r\n4 = LB\r\n5 = RB\r\n6 = LT\r\n7 = RT\r\n"
    "8 = View\r\n9 = Menu\r\n10 = LS\r\n11 = RS\r\n12 = Menu\r\n13 = View\r\n";

struct Name {
    const char* name;
    std::uint32_t bit;
};

constexpr Name kButtonNames[] = {{"A", kA},
                                 {"B", kB},
                                 {"X", kX},
                                 {"Y", kY},
                                 {"Menu", kMenu},
                                 {"View", kView},
                                 {"Up", kDPadUp},
                                 {"Down", kDPadDown},
                                 {"Left", kDPadLeft},
                                 {"Right", kDPadRight},
                                 {"LB", kLeftShoulder},
                                 {"RB", kRightShoulder},
                                 {"LS", kLeftThumbstick},
                                 {"RS", kRightThumbstick},
                                 {"LT", kLeftTriggerBit},
                                 {"RT", kRightTriggerBit}};

constexpr Name kAutopressNames[] = {{"Menu", kMenu}, {"View", kView},     {"A", kA},          {"B", kB},
                                    {"X", kX},       {"Y", kY},           {"Up", kDPadUp},    {"Down", kDPadDown},
                                    {"Left", kDPadLeft}, {"Right", kDPadRight}};

struct Key {
    const char* name;
    int vk;
};

constexpr Key kKeyNames[] = {{"Up", VK_UP},       {"Down", VK_DOWN},   {"Left", VK_LEFT},      {"Right", VK_RIGHT},
                             {"Enter", VK_RETURN}, {"Space", VK_SPACE}, {"Backspace", VK_BACK}, {"Escape", VK_ESCAPE},
                             {"Tab", VK_TAB},     {"Shift", VK_SHIFT}, {"Ctrl", VK_CONTROL},   {"Alt", VK_MENU}};

std::mutex g_mutex;

float larger(float a, float b) { return std::fabs(a) >= std::fabs(b) ? a : b; }

float stick(LONG value, LONG range) {
    return std::clamp(static_cast<float>(value) / static_cast<float>(range), -1.0f, 1.0f);
}

bool same_name(std::string_view a, const char* b) {
    const std::size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (std::size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

std::string_view trim(std::string_view s) {
    const std::size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string_view::npos ? std::string_view() : s.substr(a, b - a + 1);
}

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

bool read_xinput(Reading& r) {
    static const XInputGetStateFn get_state = [] {
        HMODULE dll = LoadLibraryW(kXInputDll);
        return dll ? reinterpret_cast<XInputGetStateFn>(GetProcAddress(dll, kXInputGetState)) : nullptr;
    }();
    if (!get_state) return false;
    for (DWORD pad = 0; pad < XUSER_MAX_COUNT; ++pad) {
        XINPUT_STATE s{};
        if (get_state(pad, &s) != ERROR_SUCCESS) continue;
        const WORD b = s.Gamepad.wButtons;
        const struct {
            WORD from;
            std::uint32_t to;
        } map[] = {{XINPUT_GAMEPAD_START, kMenu},         {XINPUT_GAMEPAD_BACK, kView},
                   {XINPUT_GAMEPAD_A, kA},                {XINPUT_GAMEPAD_B, kB},
                   {XINPUT_GAMEPAD_X, kX},                {XINPUT_GAMEPAD_Y, kY},
                   {XINPUT_GAMEPAD_DPAD_UP, kDPadUp},     {XINPUT_GAMEPAD_DPAD_DOWN, kDPadDown},
                   {XINPUT_GAMEPAD_DPAD_LEFT, kDPadLeft}, {XINPUT_GAMEPAD_DPAD_RIGHT, kDPadRight},
                   {XINPUT_GAMEPAD_LEFT_SHOULDER, kLeftShoulder},
                   {XINPUT_GAMEPAD_RIGHT_SHOULDER, kRightShoulder},
                   {XINPUT_GAMEPAD_LEFT_THUMB, kLeftThumbstick},
                   {XINPUT_GAMEPAD_RIGHT_THUMB, kRightThumbstick}};
        for (const auto& m : map)
            if (b & m.from) r.buttons |= m.to;
        r.left_trigger = std::max(r.left_trigger, s.Gamepad.bLeftTrigger / kXInputTriggerRange);
        r.right_trigger = std::max(r.right_trigger, s.Gamepad.bRightTrigger / kXInputTriggerRange);
        r.left_x = larger(r.left_x, stick(s.Gamepad.sThumbLX, kXInputStickRange));
        r.left_y = larger(r.left_y, stick(s.Gamepad.sThumbLY, kXInputStickRange));
        r.right_x = larger(r.right_x, stick(s.Gamepad.sThumbRX, kXInputStickRange));
        r.right_y = larger(r.right_y, stick(s.Gamepad.sThumbRY, kXInputStickRange));
        static bool logged = false;
        if (!logged) {
            logged = true;
            G6SHIM_INFO("input: XInput controller %lu", pad);
        }
        return true;
    }
    return false;
}

std::vector<std::pair<std::string, std::string>> ini_section(const std::string& path, const char* section) {
    std::vector<char> buf(kIniSectionBytes);
    const DWORD n = GetPrivateProfileSectionA(section, buf.data(), static_cast<DWORD>(buf.size()), path.c_str());
    std::vector<std::pair<std::string, std::string>> out;
    for (const char* line = buf.data(); line < buf.data() + n && *line; line += std::strlen(line) + 1) {
        const std::string_view text = line;
        const std::size_t eq = text.find('=');
        if (text.empty() || text[0] == ';' || eq == std::string_view::npos) continue;
        out.emplace_back(trim(text.substr(0, eq)), trim(text.substr(eq + 1)));
    }
    return out;
}

std::uint32_t mapped_buttons(const std::string& text) {
    std::vector<std::string> unknown;
    const std::uint32_t bits = parse_buttons(text, &unknown);
    for (const auto& word : unknown) G6SHIM_WARN("input: %s: unknown button \"%s\"", kMappingFile, word.c_str());
    return bits;
}

struct Mapping {
    std::vector<std::pair<int, std::uint32_t>> keys;
    std::uint32_t controller[kControllerButtons] = {};
};

const Mapping& mapping() {
    static const Mapping m = [] {
        Mapping out;
        char exe[MAX_PATH] = "";
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string path = exe;
        path = path.substr(0, path.find_last_of("\\/") + 1) + kMappingFile;
        if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            const HANDLE f =
                CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(f, kDefaultMapping, static_cast<DWORD>(std::strlen(kDefaultMapping)), &written, nullptr);
                CloseHandle(f);
                G6SHIM_INFO("input: wrote the default mapping to %s", path.c_str());
            }
        }
        for (const auto& [key, buttons] : ini_section(path, "keyboard")) {
            if (const int vk = parse_key(key)) out.keys.emplace_back(vk, mapped_buttons(buttons));
            else G6SHIM_WARN("input: %s: unknown key \"%s\"", kMappingFile, key.c_str());
        }
        for (const auto& [number, buttons] : ini_section(path, "controller")) {
            const int i = std::atoi(number.c_str());
            if (i >= 0 && i < static_cast<int>(kControllerButtons)) out.controller[i] = mapped_buttons(buttons);
        }
        G6SHIM_INFO("input: mapping from %s (%zu keys)", path.c_str(), out.keys.size());
        return out;
    }();
    return m;
}

void apply_triggers(Reading& r, std::uint32_t bits) {
    if (bits & kLeftTriggerBit) r.left_trigger = 1.0f;
    if (bits & kRightTriggerBit) r.right_trigger = 1.0f;
    r.buttons |= bits & ~(kLeftTriggerBit | kRightTriggerBit);
}

struct DirectInputPad {
    IDirectInput8W* api = nullptr;
    IDirectInputDevice8W* device = nullptr;
    DWORD next_attempt = 0;
    BYTE previous[kControllerButtons] = {};
} g_di;

BOOL CALLBACK first_device(const DIDEVICEINSTANCEW* instance, void* found) {
    *static_cast<DIDEVICEINSTANCEW*>(found) = *instance;
    return DIENUM_STOP;
}

void open_directinput() {
    const HWND window = static_cast<HWND>(main_window());
    if (!window || GetTickCount() < g_di.next_attempt) return;
    g_di.next_attempt = GetTickCount() + kDirectInputRetryMs;
    if (!g_di.api && FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                               reinterpret_cast<void**>(&g_di.api), nullptr)))
        return;
    DIDEVICEINSTANCEW found{};
    found.dwSize = sizeof found;
    g_di.api->EnumDevices(DI8DEVCLASS_GAMECTRL, first_device, &found, DIEDFL_ATTACHEDONLY);
    if (found.guidInstance == GUID{}) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            G6SHIM_INFO("input: no XInput or DirectInput game controller found (keyboard only)");
        }
        return;
    }
    IDirectInputDevice8W* device = nullptr;
    if (FAILED(g_di.api->CreateDevice(found.guidInstance, &device, nullptr))) return;
    device->SetDataFormat(&c_dfDIJoystick2);
    device->SetCooperativeLevel(window, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    DIPROPRANGE range{};
    range.diph.dwSize = sizeof range;
    range.diph.dwHeaderSize = sizeof range.diph;
    range.diph.dwHow = DIPH_DEVICE;
    range.lMin = -kDirectInputRange;
    range.lMax = kDirectInputRange;
    device->SetProperty(DIPROP_RANGE, &range.diph);
    device->Acquire();
    g_di.device = device;
    char name[MAX_PATH] = "?";
    WideCharToMultiByte(CP_UTF8, 0, found.tszProductName, -1, name, sizeof name, nullptr, nullptr);
    G6SHIM_INFO("input: DirectInput controller \"%s\" (buttons as in %s)", name, kMappingFile);
}

bool read_directinput(Reading& r) {
    if (!g_di.device) open_directinput();
    if (!g_di.device) return false;
    DIJOYSTATE2 s{};
    g_di.device->Poll();
    if (FAILED(g_di.device->GetDeviceState(sizeof s, &s))) {
        if (FAILED(g_di.device->Acquire())) {
            g_di.device->Release();
            g_di.device = nullptr;
        }
        return false;
    }
    for (std::size_t i = 0; i < kControllerButtons; ++i) {
        const bool down = (s.rgbButtons[i] & kDirectInputPressed) != 0;
        if (down && !g_di.previous[i]) G6SHIM_INFO("input: DirectInput button %zu pressed", i);
        g_di.previous[i] = down;
        if (down) apply_triggers(r, mapping().controller[i]);
    }
    r.buttons |= pov_buttons(s.rgdwPOV[0]);
    r.left_x = larger(r.left_x, stick(s.lX, kDirectInputRange));
    r.left_y = larger(r.left_y, -stick(s.lY, kDirectInputRange));
    r.right_x = larger(r.right_x, stick(s.lZ, kDirectInputRange));
    r.right_y = larger(r.right_y, -stick(s.lRz, kDirectInputRange));
    return true;
}

void read_keyboard(Reading& r) {
    const HWND window = static_cast<HWND>(main_window());
    if (!window || GetForegroundWindow() != window) return;
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & kKeyDown) != 0; };
    for (const auto& [vk, buttons] : mapping().keys)
        if (down(vk)) apply_triggers(r, buttons);
    if (down(VK_LEFT)) r.left_x = -1.0f;
    if (down(VK_RIGHT)) r.left_x = 1.0f;
    if (down(VK_UP)) r.left_y = 1.0f;
    if (down(VK_DOWN)) r.left_y = -1.0f;
}

const std::vector<Press>& autopresses() {
    static const std::vector<Press> presses = [] {
        const std::optional<std::string> spec = settings::get(kAutopressVariable);
        if (!spec || spec->size() >= kMaxSpec) return std::vector<Press>{};
        std::vector<Press> out = parse_autopress(*spec);
        G6SHIM_INFO("input: %zu automatic press(es) from %s", out.size(), kAutopressVariable);
        return out;
    }();
    return presses;
}

}  // namespace

std::uint32_t parse_buttons(std::string_view text, std::vector<std::string>* unknown) {
    std::uint32_t bits = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t begin = text.find_first_not_of(" \t", pos);
        if (begin == std::string_view::npos) break;
        const std::size_t end = text.find_first_of(" \t", begin);
        const std::string_view word = text.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
        bool known = false;
        for (const auto& n : kButtonNames)
            if (same_name(word, n.name)) {
                bits |= n.bit;
                known = true;
            }
        if (!known && unknown) unknown->emplace_back(word);
        pos = end == std::string_view::npos ? text.size() : end;
    }
    return bits;
}

int parse_key(std::string_view name) {
    for (const auto& k : kKeyNames)
        if (same_name(name, k.name)) return k.vk;
    if (name.size() == 1 && std::isalnum(static_cast<unsigned char>(name[0])))
        return std::toupper(static_cast<unsigned char>(name[0]));
    if (!name.empty() && (name[0] == 'F' || name[0] == 'f') && name.size() <= 3) {
        const int n = std::atoi(std::string(name.substr(1)).c_str());
        if (n >= 1 && n <= kFunctionKeys) return VK_F1 + n - 1;
    }
    return 0;
}

std::vector<Press> parse_autopress(std::string_view spec) {
    std::vector<Press> out;
    std::size_t pos = 0;
    while (pos <= spec.size() && out.size() < kMaxPresses) {
        std::size_t comma = spec.find(',', pos);
        if (comma == std::string_view::npos) comma = spec.size();
        const std::string_view item = spec.substr(pos, comma - pos);
        pos = comma + 1;
        const std::size_t colon = item.find(':');
        if (colon == std::string_view::npos) continue;
        const std::string_view name = item.substr(colon + 1);
        const double at = std::atof(std::string(item.substr(0, colon)).c_str());
        for (const auto& n : kAutopressNames)
            if (name == n.name) out.push_back({at, n.bit});
    }
    return out;
}

std::uint32_t autopress_buttons(const std::vector<Press>& presses, double seconds) {
    std::uint32_t buttons = 0;
    for (const auto& p : presses)
        if (seconds >= p.at && seconds < p.at + kPressSeconds) buttons |= p.buttons;
    return buttons;
}

std::uint32_t pov_buttons(std::uint32_t pov) {
    if ((pov & 0xFFFF) == kPovCentered) return 0;
    std::uint32_t buttons = 0;
    if (pov >= kPovNorthWest || pov <= kPovNorthEast) buttons |= kDPadUp;
    if (pov >= kPovNorthEast && pov <= kPovSouthEast) buttons |= kDPadRight;
    if (pov >= kPovSouthEast && pov <= kPovSouthWest) buttons |= kDPadDown;
    if (pov >= kPovSouthWest && pov <= kPovNorthWest) buttons |= kDPadLeft;
    return buttons;
}

namespace {

Reading read_sources(bool controllers) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Reading r{};
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    r.timestamp = static_cast<std::uint64_t>(now.QuadPart);
    if (controllers && !read_xinput(r)) read_directinput(r);
    read_keyboard(r);
    static const std::uint64_t first = r.timestamp;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const double seconds = static_cast<double>(r.timestamp - first) / static_cast<double>(frequency.QuadPart);
    r.buttons |= autopress_buttons(autopresses(), seconds);
    return r;
}

}  // namespace

Reading read() { return read_sources(true); }

Reading read_without_controllers() { return read_sources(false); }

}  // namespace g6shim::input
