// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "settings_parse.hpp"
#include "title.hpp"
#include "xasync.hpp"
#include "xgamesave.hpp"

#include <g6shim/settings.hpp>

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace xg = g6shim::xgameruntime::xgamesave;
namespace xgr = g6shim::xgameruntime;
using g6shim::xgameruntime::HResult;
using g6shim::xgameruntime::Object;
using g6shim::xgameruntime::xasync::AsyncBlock;

constexpr wchar_t kChildVariable[] = L"G6SHIM_SAVE_CHILD";
constexpr wchar_t kOperationVariable[] = L"G6SHIM_SAVE_OP";
constexpr wchar_t kStopVariable[] = L"G6SHIM_SAVE_STOP";
constexpr wchar_t kMarkerVariable[] = L"G6SHIM_SAVE_MARKER";
constexpr char kChildTest[] = "xgameruntime_xgamesave_crash_child";
constexpr char kXuidFolder[] = "0009000000000001";

using Create = HResult (*)(Object*, const xg::Provider*, const char*, xg::Container**);
using Init = HResult (*)(Object*, const void*, const char*, bool, AsyncBlock*);
using InitResult = HResult (*)(Object*, AsyncBlock*, xg::Provider**);
using CloseProvider = void (*)(Object*, xg::Provider*);
using CreateUpdate = HResult (*)(Object*, const xg::Container*, const char*, xg::Update**);
using Write = HResult (*)(Object*, xg::Update*, const char*, const std::uint8_t*, std::size_t);
using DeleteBlob = HResult (*)(Object*, xg::Update*, const char*);
using Submit = HResult (*)(Object*, const xg::Update*, AsyncBlock*);
using CloseContainer = void (*)(Object*, xg::Container*);
using CloseUpdate = void (*)(Object*, xg::Update*);
using Enumerate = HResult (*)(Object*, const xg::Provider*, void*, bool (*)(const xg::ContainerInfo*, void*));
using ReadAsync = HResult (*)(Object*, const xg::Container*, const char* const*, std::uint32_t, AsyncBlock*);
using ReadResult = HResult (*)(Object*, AsyncBlock*, std::size_t, std::uint8_t*, std::uint32_t*);
using ReadNow = HResult (*)(Object*, const xg::Container*, const char* const*, std::uint32_t*, std::size_t,
                            std::uint8_t*);
using Quota = HResult (*)(Object*, const xg::Provider*, AsyncBlock*);
using QuotaResult = HResult (*)(Object*, AsyncBlock*, std::int64_t*);
using Delete = HResult (*)(Object*, const xg::Provider*, const char*, AsyncBlock*);
using DeleteResult = HResult (*)(Object*, AsyncBlock*);

template <class Function>
Function at(std::size_t offset) {
    return xgr::method<Function>(xg::kObject, offset);
}

Object* self() { return const_cast<Object*>(&xg::kObject); }

class TempDir {
public:
    TempDir() {
        static unsigned counter = 0;
        std::error_code error;
        path_ = std::filesystem::temp_directory_path(error);
        path_ /= "g6shim_saves_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(counter++);
        std::filesystem::create_directories(path_, error);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view data) {
    std::ofstream out(path, std::ios::binary);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

using Change = std::pair<std::string, std::optional<std::string>>;

HResult apply_update(const std::filesystem::path& root, const std::string& display,
                     const std::vector<Change>& changes) {
    xg::Update update;
    update.name = "Slot";
    update.dir = xg::container_dir(root, "Slot");
    update.display = display;
    for (const auto& [name, data] : changes) {
        if (data)
            update.changes.emplace_back(name, std::vector<std::uint8_t>(data->begin(), data->end()));
        else
            update.changes.emplace_back(name, std::nullopt);
    }
    return xg::submit(update);
}

struct State {
    std::string display;
    std::vector<std::pair<std::string, std::string>> blobs;

    friend bool operator==(const State&, const State&) = default;
};

std::optional<State> state_of(const std::filesystem::path& root) {
    xg::Container container{"Slot", xg::container_dir(root, "Slot")};
    std::error_code error;
    const std::optional<xg::Version> version = xg::current_named(container.dir, "Slot", error);
    if (!version) return std::nullopt;
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> blobs;
    if (xg::read_container(container, std::nullopt, blobs) != xgr::kOk) return std::nullopt;
    State state;
    state.display = version->display;
    for (const auto& [name, data] : blobs) state.blobs.emplace_back(name, std::string(data.begin(), data.end()));
    return state;
}

const State kOldState{"one", {{"a", "old a"}, {"b", "old b"}}};
const State kNewState{"two", {{"a", "new a"}, {"c", "new c"}}};

unsigned g_stop = 0;
unsigned g_steps = 0;
std::filesystem::path g_marker;

void child_step() {
    ++g_steps;
    if (g_steps != g_stop) return;
    const HANDLE marker = CreateFileW(g_marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (marker != INVALID_HANDLE_VALUE) CloseHandle(marker);
    for (;;) Sleep(1000);
}

std::optional<std::wstring> environment(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) return std::nullopt;
    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), size);
    if (written == 0 || written >= size) return std::nullopt;
    value.resize(written);
    return value;
}

struct ChildRun {
    bool started = false;
    bool finished = false;
};

ChildRun run_child(const std::filesystem::path& root, const char* operation, unsigned stop,
                   const std::filesystem::path& marker) {
    std::error_code ignored;
    std::filesystem::remove(marker, ignored);
    SetEnvironmentVariableW(kChildVariable, root.c_str());
    SetEnvironmentVariableW(kOperationVariable, std::wstring(operation, operation + std::strlen(operation)).c_str());
    const std::wstring stop_text = std::to_wstring(stop);
    SetEnvironmentVariableW(kStopVariable, stop_text.c_str());
    SetEnvironmentVariableW(kMarkerVariable, marker.c_str());
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(exe) + L"\" " + std::wstring(kChildTest, kChildTest + std::strlen(kChildTest));
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                        nullptr, &startup, &process);
    SetEnvironmentVariableW(kChildVariable, nullptr);
    SetEnvironmentVariableW(kOperationVariable, nullptr);
    SetEnvironmentVariableW(kStopVariable, nullptr);
    SetEnvironmentVariableW(kMarkerVariable, nullptr);
    ChildRun run;
    run.started = started != 0;
    if (!started) return run;
    for (int attempt = 0; attempt < 20000; ++attempt) {
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
            run.finished = true;
            break;
        }
        if (GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES) break;
        Sleep(1);
    }
    if (!run.finished) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return run;
}

struct Found {
    std::string name;
    std::string display;
    std::uint32_t blobs;
    std::uint64_t size;

    friend bool operator==(const Found&, const Found&) = default;
};

bool collect_container(const xg::ContainerInfo* info, void* context) {
    auto* out = static_cast<std::vector<Found>*>(context);
    out->push_back({info->name, info->display_name, info->blob_count, info->total_size});
    return true;
}

}  // namespace

TEST(xgameruntime_xgamesave_the_data_folder_comes_from_the_settings) {
    g6shim::settings::reload_file_for_test();
    const char file[] = "[saves]\ndir = C:/g6shim-saves-from-file\n";
    g6shim::settings::install_bytes_for_test(reinterpret_cast<const std::uint8_t*>(file), sizeof file - 1);
    xgr::Title title;
    title.id = 0x1A2B3C4D;
    xgr::set_title(title);
    CHECK(xgr::data_folder() == std::filesystem::path(L"C:/g6shim-saves-from-file"));

    const std::optional<std::wstring> saved = environment(L"G6SHIM_DATA_DIR");
    SetEnvironmentVariableW(L"G6SHIM_DATA_DIR", L"C:/g6shim-saves-from-environment");
    CHECK(xgr::data_folder() == std::filesystem::path(L"C:/g6shim-saves-from-environment"));
    SetEnvironmentVariableW(L"G6SHIM_DATA_DIR", saved ? saved->c_str() : nullptr);
    CHECK(xgr::data_folder() == std::filesystem::path(L"C:/g6shim-saves-from-file"));

    xgr::clear_title();
    g6shim::settings::reload_file_for_test();
}

TEST(xgameruntime_xgamesave_a_full_disk_is_not_the_quota) {
    const std::uint64_t mb = std::uint64_t{1} << 20;
    CHECK(xg::no_room(2 * mb, 0, 0, mb) == xg::kOutOfLocalStorage);
    CHECK(xg::no_room(2 * mb, 0, xg::kQuota - mb, 100 * mb) == xg::kQuotaExceeded);
    CHECK(!xg::no_room(mb, mb, xg::kQuota, 100 * mb).has_value());
    CHECK(!xg::no_room(mb, 0, 0, mb).has_value());
}

TEST(xgameruntime_xgamesave_names_are_exact) {
    for (const std::string& name : std::vector<std::string>{"Save_1", "slot/2", "a.b c%", "\xc3\xa9t\xc3\xa9"}) {
        const std::string file = xg::file_name(name);
        CHECK(file.find('.') == std::string::npos);
        CHECK(file.find('/') == std::string::npos);
        CHECK(file.find(' ') == std::string::npos);
        CHECK(xg::decode_name(file) == name);
    }
    CHECK(!xg::decode_name("x.new").has_value());
    for (const std::string& good :
         std::vector<std::string>{"Profile/Slot_1", "a/b/Save-2.sav", "x", std::string(256, 'a')}) {
        CHECK(xg::valid_container(good));
    }
    for (const std::string& bad : std::vector<std::string>{"a b", "", "a/", "a.", "a..b", "a.b/c", "a-b/c",
                                                           std::string(257, 'a')}) {
        CHECK(!xg::valid_container(bad));
    }

    TempDir dir;
    const xg::Provider provider{dir.path()};
    const std::string long_name(300, 'n');
    const std::vector<std::string> blobs = {"A", "a", "CON", "nul.txt", "x/y:z*?\"<>|", long_name, "end. "};
    for (const std::string& container : std::vector<std::string>{"Save", "save", "SAVE/CON.x"}) {
        xg::Update update;
        update.name = container;
        update.dir = xg::container_dir(dir.path(), container);
        for (const std::string& blob : blobs) {
            const std::string data = container + ":" + blob;
            update.changes.emplace_back(blob, std::vector<std::uint8_t>(data.begin(), data.end()));
        }
        const HResult result = xg::submit(update);
        CHECK(result == xgr::kOk);
    }
    std::vector<std::string> found;
    for (const auto& [name, path] : xg::containers(dir.path())) found.push_back(name);
    CHECK(found == std::vector<std::string>({"SAVE/CON.x", "Save", "save"}));
    for (const std::string& container : std::vector<std::string>{"Save", "save", "SAVE/CON.x"}) {
        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> read;
        CHECK(xg::read_container(xg::Container{container, xg::container_dir(dir.path(), container)}, std::nullopt,
                                 read) == xgr::kOk);
        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> expected;
        for (const std::string& blob : blobs) {
            const std::string data = container + ":" + blob;
            expected.emplace_back(blob, std::vector<std::uint8_t>(data.begin(), data.end()));
        }
        std::sort(expected.begin(), expected.end());
        CHECK(read == expected);
    }
}

TEST(xgameruntime_xgamesave_the_old_layout_moves_over) {
    TempDir saves;
    const std::filesystem::path root = saves.path() / kXuidFolder / "scid";
    const std::filesystem::path old = root / "Player";
    std::error_code error;
    std::filesystem::create_directories(old, error);
    write_file(old / "Save%20Game%20Data", "data");
    write_file(old / "Save%20Game%20Header", "header");
    write_file(old / "Save%20Game%20Data.new", "half");
    write_file(old / ".display", "Chapter 1");
    HResult status = xgr::kOk;
    const std::optional<xg::Provider> provider = xg::open_provider(saves.path(), std::string("scid"), status);
    CHECK(provider.has_value());
    if (!provider) return;
    CHECK(!std::filesystem::exists(old));
    const xg::Container container{"Player", xg::container_dir(provider->root, "Player")};
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> blobs;
    CHECK(xg::read_container(container, std::nullopt, blobs) == xgr::kOk);
    CHECK(blobs.size() == 2);
    if (blobs.size() == 2) {
        CHECK(blobs[0].first == "Save Game Data");
        CHECK(std::string(blobs[0].second.begin(), blobs[0].second.end()) == "data");
        CHECK(blobs[1].first == "Save Game Header");
        CHECK(std::string(blobs[1].second.begin(), blobs[1].second.end()) == "header");
    }
    std::error_code code;
    const std::optional<xg::Version> version = xg::current_named(container.dir, "Player", code);
    CHECK(version.has_value());
    if (version) {
        CHECK(version->number == 1);
        CHECK(version->display == "Chapter 1");
    }
}

TEST(xgameruntime_xgamesave_an_empty_configuration_id_is_the_default_one) {
    TempDir saves;
    HResult status = xgr::kOk;
    const std::optional<xg::Provider> scid = xg::open_provider(saves.path(), std::string("scid"), status);
    CHECK(scid.has_value());
    if (!scid) return;
    CHECK(apply_update(scid->root, "one", {{"a", "kept"}}) == xgr::kOk);
    const State kept{"one", {{"a", "kept"}}};
    CHECK(state_of(scid->root) == kept);

    const std::optional<xg::Provider> empty = xg::open_provider(saves.path(), std::string(""), status);
    CHECK(empty.has_value());
    CHECK(std::filesystem::is_directory(scid->root));
    CHECK(state_of(scid->root) == kept);
    const std::optional<xg::Provider> fallback = xg::open_provider(saves.path(), std::nullopt, status);
    CHECK(fallback.has_value());
    if (!empty || !fallback) return;
    CHECK(empty->root == fallback->root);
    CHECK(empty->root != scid->root);
    CHECK(!state_of(empty->root).has_value());
    const std::optional<xg::Provider> other = xg::open_provider(saves.path(), std::string("other"), status);
    CHECK(other.has_value());
    if (other) CHECK(!state_of(other->root).has_value());
    CHECK(state_of(scid->root) == kept);
}

TEST(xgameruntime_xgamesave_saves_persist_across_providers) {
    TempDir dir;
    const std::filesystem::path saves = dir.path() / "saves";
    void* served = nullptr;
    CHECK(xgr::query_api(&xg::kProvider, &xg::kProvider, &served) == xgr::kOk);
    CHECK(served == const_cast<Object*>(&xg::kObject));

    xgr::Title title;
    title.saves = saves;
    xgr::set_title(title);

    AsyncBlock init{};
    CHECK(at<Init>(0x20)(self(), nullptr, "scid", false, &init) == xgr::kOk);
    CHECK(xgr::xasync::XAsyncGetStatus(&init, true) == xgr::kOk);
    xg::Provider* opened = nullptr;
    CHECK(at<InitResult>(0x28)(self(), &init, &opened) == xgr::kOk);
    CHECK(opened != nullptr);
    if (opened != nullptr) at<CloseProvider>(0x30)(self(), opened);
    xgr::clear_title();

    HResult status = xgr::kOk;
    const std::optional<xg::Provider> provider_value = xg::open_provider(saves, std::string("scid"), status);
    CHECK(provider_value.has_value());
    if (!provider_value) return;
    xg::Provider* provider = new xg::Provider(*provider_value);

    xg::Container* container = nullptr;
    const Create create = at<Create>(0x80);
    CHECK(create(self(), provider, "bad name", &container) == xg::kInvalidContainerName);
    CHECK(create(self(), provider, "Slot_1", &container) == xgr::kOk);
    xg::Update* update = nullptr;
    CHECK(at<CreateUpdate>(0xb8)(self(), container, "Chapter 1", &update) == xgr::kOk);
    if (update == nullptr) return;
    const Write write = at<Write>(0xc8);
    CHECK(write(self(), update, "world", reinterpret_cast<const std::uint8_t*>("hello"), 5) == xgr::kOk);
    CHECK(write(self(), update, "player.dat", reinterpret_cast<const std::uint8_t*>("xy"), 2) == xgr::kOk);
    CHECK(write(self(), update, "gone", reinterpret_cast<const std::uint8_t*>("z"), 1) == xgr::kOk);
    CHECK(at<DeleteBlob>(0xd0)(self(), update, "gone") == xgr::kOk);
    AsyncBlock submit{};
    CHECK(at<Submit>(0xe0)(self(), update, &submit) == xgr::kOk);
    CHECK(xgr::xasync::XAsyncGetStatus(&submit, true) == xgr::kOk);
    at<CloseUpdate>(0xc0)(self(), update);
    at<CloseContainer>(0x88)(self(), container);
    delete provider;

    const std::optional<xg::Provider> reopened = xg::open_provider(saves, std::string("scid"), status);
    CHECK(reopened.has_value());
    if (!reopened) return;
    provider = new xg::Provider(*reopened);
    std::vector<Found> found;
    CHECK(at<Enumerate>(0x70)(self(), provider, &found, &collect_container) == xgr::kOk);
    CHECK((found == std::vector<Found>{Found{"Slot_1", "Chapter 1", 2, 7}}));

    container = nullptr;
    CHECK(create(self(), provider, "Slot_1", &container) == xgr::kOk);
    if (container == nullptr) return;
    const char* wanted[] = {"world", "player.dat"};
    AsyncBlock read{};
    CHECK(at<ReadAsync>(0xa8)(self(), container, wanted, 2, &read) == xgr::kOk);
    CHECK(xgr::xasync::XAsyncGetStatus(&read, true) == xgr::kOk);
    std::size_t size = 0;
    CHECK(xgr::xasync::XAsyncGetResultSize(&read, &size) == xgr::kOk);
    CHECK(size == 2 * xg::kBlobEntry + 11 + 13);
    std::vector<std::uint64_t> buffer((size + 7) / 8, 0);
    std::uint32_t count = 0;
    const ReadResult result = at<ReadResult>(0xb0);
    CHECK(result(self(), &read, 8, reinterpret_cast<std::uint8_t*>(buffer.data()), &count) ==
          xg::kBufferTooSmall);
    CHECK(result(self(), &read, size, reinterpret_cast<std::uint8_t*>(buffer.data()), &count) == xgr::kOk);
    CHECK(count == 2);
    const auto blob = [&buffer](std::size_t index) {
        const std::uint8_t* entry = reinterpret_cast<const std::uint8_t*>(buffer.data()) + index * xg::kBlobEntry;
        const char* name = nullptr;
        std::memcpy(&name, entry, sizeof name);
        std::uint32_t length = 0;
        std::memcpy(&length, entry + 8, sizeof length);
        const std::uint8_t* data = nullptr;
        std::memcpy(&data, entry + 16, sizeof data);
        return std::pair<std::string, std::string>(name, std::string(reinterpret_cast<const char*>(data), length));
    };
    CHECK((blob(0) == std::pair<std::string, std::string>("world", "hello")));
    CHECK((blob(1) == std::pair<std::string, std::string>("player.dat", "xy")));

    const char* missing[] = {"nothing"};
    std::uint32_t one = 1;
    std::uint8_t small[8] = {};
    const ReadNow read_now = at<ReadNow>(0xa0);
    CHECK(read_now(self(), container, missing, &one, 8, small) == xg::kBlobNotFound);
    std::uint32_t two = 2;
    CHECK(read_now(self(), container, wanted, &two, 8, small) == xg::kBufferTooSmall);

    AsyncBlock quota{};
    CHECK(at<Quota>(0x40)(self(), provider, &quota) == xgr::kOk);
    std::int64_t left = 0;
    CHECK(at<QuotaResult>(0x48)(self(), &quota, &left) == xgr::kOk);
    CHECK(static_cast<std::uint64_t>(left) <= xg::kQuota - 7);

    AsyncBlock remove{};
    CHECK(at<Delete>(0x58)(self(), provider, "Slot_1", &remove) == xgr::kOk);
    CHECK(at<DeleteResult>(0x60)(self(), &remove) == xgr::kOk);
    found.clear();
    CHECK(at<Enumerate>(0x70)(self(), provider, &found, &collect_container) == xgr::kOk);
    CHECK(found.empty());
    at<CloseContainer>(0x88)(self(), container);
    delete provider;
}

TEST(xgameruntime_xgamesave_crash_child) {
    if (GetEnvironmentVariableW(kChildVariable, nullptr, 0) == 0) return;
    const std::optional<std::wstring> root = environment(kChildVariable);
    const std::optional<std::wstring> operation = environment(kOperationVariable);
    const std::optional<std::wstring> stop = environment(kStopVariable);
    const std::optional<std::wstring> marker = environment(kMarkerVariable);
    if (!root || !operation || !stop || !marker) {
        CHECK(false);
        return;
    }
    g_steps = 0;
    g_stop = static_cast<unsigned>(std::stoul(*stop));
    g_marker = *marker;
    xg::crash_hook() = &child_step;
    HResult result = xgr::kOk;
    if (*operation == L"update")
        result = apply_update(*root, "two", {{"a", "new a"}, {"b", std::nullopt}, {"c", "new c"}});
    else
        result = xg::delete_container(*root, "Slot");
    xg::crash_hook() = nullptr;
    CHECK(result == xgr::kOk);
}

TEST(xgameruntime_xgamesave_a_kill_at_any_step_leaves_old_or_new) {
    struct Case {
        const char* operation;
        std::optional<State> after;
    };
    for (const Case& current : {Case{"update", kNewState}, Case{"delete", std::nullopt}}) {
        int old_seen = 0;
        int new_seen = 0;
        for (unsigned stop = 1; stop <= 64; ++stop) {
            TempDir dir;
            const std::filesystem::path root = dir.path() / "root";
            CHECK(apply_update(root, "one", {{"a", "old a"}, {"b", "old b"}}) == xgr::kOk);
            CHECK(state_of(root) == kOldState);
            const ChildRun child = run_child(root, current.operation, stop, dir.path() / "stopped");
            CHECK(child.started);
            if (!child.started) break;
            const std::optional<State> left = state_of(root);
            if (left == kOldState) {
                ++old_seen;
            } else {
                CHECK(left == current.after);
                ++new_seen;
            }
            if (child.finished) break;
        }
        CHECK(old_seen > 0);
        CHECK(new_seen > 1);
    }
}
