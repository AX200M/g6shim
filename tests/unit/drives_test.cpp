// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include <g6shim/drives.hpp>

#include <windows.h>

#include <string>

using g6shim::drives::mount_drive;
using g6shim::drives::mount_drive_at;
using g6shim::drives::on_drive;
using g6shim::drives::translate;
using g6shim::drives::translate_package;

namespace {

struct TempFolder {
    TempFolder() {
        wchar_t base[MAX_PATH] = L"";
        GetTempPathW(MAX_PATH, base);
        static unsigned counter = 0;
        path = std::wstring(base) + L"g6shim_drives_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
               std::to_wstring(counter++);
        CreateDirectoryW(path.c_str(), nullptr);
    }
    ~TempFolder() { RemoveDirectoryW(path.c_str()); }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;
    std::wstring path;
};

std::wstring exe_folder() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return {};
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}

bool is(const std::optional<std::wstring>& value, const std::wstring& expected) {
    return value.has_value() && *value == expected;
}

}  // namespace

TEST(drives_recognise_the_path_forms_a_title_builds) {
    CHECK(on_drive(L"G:\\a\\b.bin"));
    CHECK(on_drive(L"g:/a"));
    CHECK(on_drive(L"G:"));
    CHECK(on_drive(L"G:relative"));
    CHECK(on_drive(L"\\\\?\\G:\\a"));
    CHECK(on_drive("G:\\a"));
    CHECK(on_drive("\\\\?\\g:/a"));
    CHECK(on_drive(L"C:\\a"));
    CHECK(!on_drive(L"1:\\a"));
    CHECK(!on_drive(L"relative/path"));
    CHECK(!on_drive(L""));
    CHECK(!on_drive(static_cast<const wchar_t*>(nullptr)));
    CHECK(!on_drive(static_cast<const char*>(nullptr)));
}

TEST(drives_mount_the_package_drive_on_the_executable_folder) {
    const std::wstring folder = exe_folder();
    CHECK(!folder.empty());
    CHECK(is(translate(L"G:\\common.rpf"), folder + L"\\common.rpf"));
    CHECK(is(translate(L"g:/common.rpf"), folder + L"/common.rpf"));
    CHECK(is(translate(L"G:"), folder + L"\\"));
    CHECK(is(translate("G:\\common.rpf"), folder + L"\\common.rpf"));
}

TEST(drives_rewrite_only_paths_on_a_mounted_drive) {
    TempFolder folder;
    mount_drive_at(L'Z', folder.path.c_str());
    CHECK(is(translate(L"Z:/a/b.txt"), folder.path + L"/a/b.txt"));
    CHECK(is(translate(L"z:\\a"), folder.path + L"\\a"));
    CHECK(is(translate(L"Z:"), folder.path + L"\\"));
    CHECK(is(translate(L"Z:relative"), folder.path + L"\\relative"));
    CHECK(is(translate(L"\\\\?\\Z:/a/b"), L"\\\\?\\" + folder.path + L"\\a\\b"));
    CHECK(is(translate(L"Z:\\a"), folder.path + L"\\a"));
    CHECK(is(translate("Z:/a/b.txt"), folder.path + L"/a/b.txt"));
    CHECK(is(translate("\\\\?\\z:\\a"), L"\\\\?\\" + folder.path + L"\\a"));
    CHECK(!translate(L"C:\\Windows").has_value());
    CHECK(!translate(L"relative/path").has_value());
    CHECK(!translate(L"").has_value());
    CHECK(!translate(static_cast<const wchar_t*>(nullptr)).has_value());
    CHECK(!translate("C:\\Windows").has_value());
    CHECK(!translate("relative/path").has_value());
    CHECK(!translate("").has_value());
    CHECK(!translate(static_cast<const char*>(nullptr)).has_value());
    mount_drive_at(L'Z', exe_folder().c_str());
}

TEST(drives_clean_up_long_paths_for_the_verbatim_form) {
    TempFolder folder;
    mount_drive_at(L'Y', folder.path.c_str());
    const std::wstring name(250, L'd');
    const std::wstring path = L"Y:/a/./b/../" + name + L"//f";
    CHECK(is(translate(path.c_str()), L"\\\\?\\" + folder.path + L"\\a\\" + name + L"\\f"));
    mount_drive_at(L'Y', exe_folder().c_str());
}

TEST(drives_put_a_unc_mount_in_the_verbatim_unc_form) {
    mount_drive_at(L'Y', L"\\\\server\\share\\");
    const std::wstring name(250, L'd');
    CHECK(is(translate((L"Y:/x/" + name).c_str()), L"\\\\?\\UNC\\server\\share\\x\\" + name));
    mount_drive_at(L'Y', exe_folder().c_str());
}

TEST(drives_give_a_folder_the_highest_free_letter) {
    TempFolder first;
    TempFolder second;
    const int letter = mount_drive(first.path.c_str());
    CHECK(letter >= L'D');
    CHECK(letter <= L'Z');
    CHECK(letter != L'G');
    CHECK(mount_drive(first.path.c_str()) == letter);
    const int other = mount_drive(second.path.c_str());
    CHECK(other >= L'D');
    CHECK(other <= L'Z');
    CHECK(other != letter);
    CHECK(other != L'G');
    std::wstring spec(1, static_cast<wchar_t>(letter));
    spec += L":";
    CHECK(is(translate(spec.c_str()), first.path + L"\\"));
}

TEST(drives_replace_a_fixed_letter) {
    TempFolder first;
    TempFolder second;
    const std::wstring folder = exe_folder();
    mount_drive_at(L'g', first.path.c_str());
    CHECK(is(translate(L"G:\\a"), first.path + L"\\a"));
    mount_drive_at(L'G', second.path.c_str());
    CHECK(is(translate(L"G:\\a"), second.path + L"\\a"));
    CHECK(is(translate(L"g:\\a"), second.path + L"\\a"));
    mount_drive_at(L'1', second.path.c_str());
    CHECK(is(translate(L"G:\\a"), second.path + L"\\a"));
    mount_drive_at(L'G', folder.c_str());
}

TEST(drives_translate_against_a_given_root) {
    CHECK(translate_package(L"G:\\a\\b.bin", L"D:\\games\\copy\\") == L"D:\\games\\copy\\a\\b.bin");
    CHECK(translate_package(L"g:/a/b", L"D:\\games\\copy\\") == L"D:\\games\\copy\\a/b");
    CHECK(translate_package(L"G:", L"D:\\games\\copy") == L"G:");
    CHECK(translate_package(L"C:\\other", L"D:\\games\\copy") == L"C:\\other");
    CHECK(translate_package(L"G:\\x", L"") == L"G:\\x");
    CHECK(translate_package(nullptr, L"D:\\games\\copy").empty());
}
