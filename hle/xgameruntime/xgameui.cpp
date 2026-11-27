// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "xgameui.hpp"

#include "xasync.hpp"

#include <g6shim/log.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace g6shim::xgameruntime::xgameui {

namespace {

using This = Object*;

constexpr int kEditControl = 100;
constexpr std::uint16_t kDefaultControl = 0x0080;
constexpr std::uint16_t kEditClass = 0x0081;
constexpr std::uint16_t kLabelClass = 0x0082;
constexpr std::uint16_t kNoId = 0xffff;

const std::uint8_t kProfile = 0;
std::uint8_t kText = 0;

std::wstring wide(std::string_view text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring out(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count);
    return out;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int count =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string out(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count, nullptr, nullptr);
    return out;
}

struct Entry {
    std::uint32_t max;
    std::optional<std::string> typed;
};

INT_PTR CALLBACK procedure(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    const HWND edit = GetDlgItem(dialog, kEditControl);
    switch (message) {
        case WM_INITDIALOG: {
            SetWindowLongPtrW(dialog, GWLP_USERDATA, static_cast<LONG_PTR>(lparam));
            auto* entry = reinterpret_cast<Entry*>(lparam);
            SendMessageW(edit, 0x00c5, static_cast<WPARAM>(entry->max), 0);
            SendMessageW(edit, 0x00b1, 0, -1);
            SetFocus(edit);
            return 0;
        }
        case WM_COMMAND: {
            auto* entry = reinterpret_cast<Entry*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
            switch (LOWORD(wparam)) {
                case IDOK: {
                    if (entry == nullptr) return 1;
                    const int length = GetWindowTextLengthW(edit);
                    std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
                    const int written =
                        GetWindowTextW(edit, buffer.data(), static_cast<int>(buffer.size()));
                    buffer.resize(written > 0 ? static_cast<std::size_t>(written) : 0);
                    entry->typed = narrow(buffer);
                    EndDialog(dialog, 1);
                    return 1;
                }
                case IDCANCEL:
                    EndDialog(dialog, 0);
                    return 1;
                default:
                    return 0;
            }
        }
        default:
            return 0;
    }
}

void add_word(std::vector<wchar_t>& words, std::uint16_t value) {
    words.push_back(static_cast<wchar_t>(value));
}

void add_dword(std::vector<wchar_t>& words, std::uint32_t value) {
    add_word(words, static_cast<std::uint16_t>(value & 0xffff));
    add_word(words, static_cast<std::uint16_t>(value >> 16));
}

void add_text(std::vector<wchar_t>& words, std::string_view text) {
    for (const wchar_t character : wide(text)) {
        if (character != L'\0') words.push_back(character);
    }
    words.push_back(L'\0');
}

struct Item {
    std::uint32_t style;
    std::array<std::uint16_t, 4> rect;
    std::uint16_t id;
    std::uint16_t kind;
    std::string_view caption;
};

void add_item(std::vector<wchar_t>& words, const Item& item) {
    if (words.size() % 2 == 1) words.push_back(L'\0');
    add_dword(words, static_cast<std::uint32_t>(WS_CHILD) | static_cast<std::uint32_t>(WS_VISIBLE) | item.style);
    add_dword(words, 0);
    for (const std::uint16_t value : item.rect) add_word(words, value);
    add_word(words, item.id);
    add_word(words, 0xffff);
    add_word(words, item.kind);
    add_text(words, item.caption);
    add_word(words, 0);
}

std::optional<std::string> dialog_ask(const std::string& title, const std::string& description,
                                      const std::string& initial, std::uint32_t max_length) {
    std::vector<wchar_t> words;
    const std::uint32_t style = static_cast<std::uint32_t>(WS_POPUP) | static_cast<std::uint32_t>(WS_CAPTION) |
                                static_cast<std::uint32_t>(WS_SYSMENU) |
                                static_cast<std::uint32_t>(DS_MODALFRAME) |
                                static_cast<std::uint32_t>(DS_CENTER) |
                                static_cast<std::uint32_t>(DS_SETFOREGROUND);
    add_dword(words, style);
    add_dword(words, static_cast<std::uint32_t>(WS_EX_TOPMOST));
    add_word(words, 4);
    add_word(words, 0);
    add_word(words, 0);
    add_word(words, 240);
    add_word(words, 62);
    add_word(words, 0);
    add_word(words, 0);
    add_text(words, title);
    const std::uint32_t tab = static_cast<std::uint32_t>(WS_TABSTOP);
    add_item(words, {0, {7, 7, 226, 10}, kNoId, kLabelClass, description});
    add_item(words, {static_cast<std::uint32_t>(WS_BORDER) | tab | static_cast<std::uint32_t>(ES_AUTOHSCROLL),
                     {7, 20, 226, 12}, static_cast<std::uint16_t>(kEditControl), kEditClass, initial});
    add_item(words, {static_cast<std::uint32_t>(BS_DEFPUSHBUTTON) | tab, {129, 40, 50, 14},
                     static_cast<std::uint16_t>(IDOK), kDefaultControl, "OK"});
    add_item(words, {static_cast<std::uint32_t>(BS_PUSHBUTTON) | tab, {183, 40, 50, 14},
                     static_cast<std::uint16_t>(IDCANCEL), kDefaultControl, "Cancel"});
    std::vector<std::uint32_t> template_words((words.size() + 1) / 2, 0);
    for (std::size_t index = 0; index < words.size(); ++index)
        template_words[index / 2] |= static_cast<std::uint32_t>(static_cast<std::uint16_t>(words[index]))
                                     << (16 * (index % 2));
    Entry entry{max_length, std::nullopt};
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<const DLGTEMPLATE*>(template_words.data()),
                            nullptr, &procedure, reinterpret_cast<LPARAM>(&entry));
    return entry.typed;
}

Ask g_ask = &dialog_ask;

HResult profile_card_async(This, xasync::AsyncBlock* block, const void*, std::uint64_t xuid) {
    G6SHIM_INFO("xgameruntime: profile card for %llu: nothing to show",
                static_cast<unsigned long long>(xuid));
    return xasync::finished(block, &kProfile, kOk);
}

HResult profile_card_result(This, xasync::AsyncBlock* block) {
    return xasync::XAsyncGetResult(block, &kProfile, 0, nullptr, nullptr);
}

HResult text_entry_async(This, xasync::AsyncBlock* block, const char* title, const char* description,
                         const char* initial, std::uint32_t, std::uint32_t max_length) {
    const std::string title_value = text(title).value_or("");
    const std::string description_value = text(description).value_or("");
    const std::string initial_value = text(initial).value_or("");
    G6SHIM_INFO("xgameruntime: text entry title=%s description=%s max=%u", title_value.c_str(),
                description_value.c_str(), max_length);
    return xasync::pending_with(block, &kText, [=]() -> xasync::PendingResult {
        const Ask ask_dialog = ask();
        const std::optional<std::string> typed =
            ask_dialog ? ask_dialog(title_value, description_value, initial_value, max_length) : std::nullopt;
        if (!typed) {
            G6SHIM_INFO("xgameruntime: text entry cancelled");
            return {kAbort, {}};
        }
        G6SHIM_INFO("xgameruntime: text entry done");
        std::string bytes = limit_text(*typed, max_length);
        bytes.push_back('\0');
        return {kOk, std::vector<std::uint8_t>(bytes.begin(), bytes.end())};
    });
}

HResult text_entry_size(This, xasync::AsyncBlock* block, std::uint32_t* out) {
    std::size_t size = 0;
    const HResult result = xasync::XAsyncGetResultSize(block, &size);
    if (result < 0) return result;
    return put(out, static_cast<std::uint32_t>(size));
}

HResult text_entry_result(This, xasync::AsyncBlock* block, std::uint32_t size, char* buffer,
                          std::uint32_t* used) {
    std::size_t written = 0;
    const HResult result = xasync::XAsyncGetResult(block, &kText, size, buffer, &written);
    if (result < 0 || used == nullptr) return result;
    return put(used, static_cast<std::uint32_t>(written));
}

const std::array<const void*, 20> kVtable = {
    slot(&missing<0>),         slot(&add_ref),           slot(&add_ref),           slot(&missing<3>),
    slot(&missing<4>),         slot(&missing<5>),        slot(&missing<6>),        slot(&profile_card_async),
    slot(&profile_card_result), slot(&missing<9>),        slot(&missing<10>),       slot(&missing<11>),
    slot(&missing<12>),        slot(&missing<13>),       slot(&missing<14>),       slot(&missing<15>),
    slot(&missing<16>),        slot(&text_entry_async),  slot(&text_entry_size),   slot(&text_entry_result)};

}  // namespace

const Object kObject{kVtable.data(), "game ui"};

Ask& ask() { return g_ask; }

std::string limit_text(std::string_view typed, std::uint32_t max_length) {
    if (max_length == 0) return std::string(typed);
    std::size_t at = 0;
    std::uint32_t count = 0;
    while (at < typed.size() && count < max_length) {
        const unsigned char lead = static_cast<unsigned char>(typed[at]);
        std::size_t length = 1;
        if ((lead & 0xe0) == 0xc0) length = 2;
        else if ((lead & 0xf0) == 0xe0) length = 3;
        else if ((lead & 0xf8) == 0xf0) length = 4;
        at = std::min(at + length, typed.size());
        ++count;
    }
    return std::string(typed.substr(0, at));
}

std::span<const Interface> interfaces() {
    static const Interface entries[] = {{kProvider, kIid2026, &kObject}};
    return entries;
}

}  // namespace g6shim::xgameruntime::xgameui

std::span<const g6shim::xgameruntime::Interface> g6shim::xgameruntime::xgameui_interfaces() {
    return xgameui::interfaces();
}
