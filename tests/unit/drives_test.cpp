// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include <g6shim/drives.hpp>

#include "drives_remove.hpp"

#include <windows.h>

#include <io.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using g6shim::drives::mount_drive;
using g6shim::drives::mount_drive_at;
using g6shim::drives::on_drive;
using g6shim::drives::translate;
using g6shim::drives::persistent_storage_drive;
using g6shim::drives::temporary_storage_drive;
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

constexpr wchar_t kTestLetter = L'Y';
constexpr char kPayload[] = "8 bytes!";
constexpr DWORD kPayloadSize = sizeof kPayload - 1;

struct Module {
    explicit Module(const wchar_t* name) : handle(LoadLibraryW((exe_folder() + L"\\" + name).c_str())) {}
    ~Module() {
        if (handle != nullptr) FreeLibrary(handle);
    }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    template <class Function>
    Function get(const char* name) const {
        return handle != nullptr ? reinterpret_cast<Function>(GetProcAddress(handle, name)) : nullptr;
    }
    HMODULE handle;
};

struct MountedFolder {
    MountedFolder() { mount_drive_at(kTestLetter, folder.path.c_str()); }
    ~MountedFolder() {
        for (const std::wstring& name : created) {
            const std::wstring path = folder.path + L"\\" + name;
            if (!DeleteFileW(path.c_str())) RemoveDirectoryW(path.c_str());
        }
        mount_drive_at(kTestLetter, exe_folder().c_str());
    }
    MountedFolder(const MountedFolder&) = delete;
    MountedFolder& operator=(const MountedFolder&) = delete;
    std::wstring host(const wchar_t* name) {
        created.emplace_back(name);
        return folder.path + L"\\" + name;
    }
    void write(const wchar_t* name) {
        const HANDLE file = CreateFileW(host(name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD written = 0;
        WriteFile(file, kPayload, kPayloadSize, &written, nullptr);
        CloseHandle(file);
    }
    TempFolder folder;
    std::vector<std::wstring> created;
};

bool is_directory(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::uint64_t file_size(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (std::uint64_t{data.nFileSizeHigh} << 32) | data.nFileSizeLow;
}

using CreateDirectoryAFunction = BOOL(WINAPI*)(LPCSTR, LPSECURITY_ATTRIBUTES);
using FindFirstFileExAFunction = HANDLE(WINAPI*)(LPCSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
using GetFullPathNameWFunction = DWORD(WINAPI*)(LPCWSTR, DWORD, LPWSTR, LPWSTR*);
using CopyFileWFunction = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, BOOL);
using CopyFile2Function = HRESULT(WINAPI*)(PCWSTR, PCWSTR, COPYFILE2_EXTENDED_PARAMETERS*);
using DirectoryAFunction = int(__cdecl*)(const char*);
using DirectoryWFunction = int(__cdecl*)(const wchar_t*);
using FindCloseFunction = int(__cdecl*)(std::intptr_t);

template <class Data>
using FindFirstA = std::intptr_t(__cdecl*)(const char*, Data*);
template <class Data>
using FindFirstW = std::intptr_t(__cdecl*)(const wchar_t*, Data*);

template <class Data>
bool crt_finds_narrow(const Module& crt, const char* name) {
    const auto find = crt.get<FindFirstA<Data>>(name);
    const auto close = crt.get<FindCloseFunction>("_findclose");
    if (find == nullptr || close == nullptr) return false;
    Data data{};
    const std::intptr_t handle = find("Y:\\*.txt", &data);
    if (handle == -1) return false;
    close(handle);
    return std::strcmp(data.name, "a.txt") == 0 && data.size == kPayloadSize && (data.attrib & _A_SUBDIR) == 0;
}

template <class Data>
bool crt_finds_wide(const Module& crt, const char* name) {
    const auto find = crt.get<FindFirstW<Data>>(name);
    const auto close = crt.get<FindCloseFunction>("_findclose");
    if (find == nullptr || close == nullptr) return false;
    Data data{};
    const std::intptr_t handle = find(L"Y:\\*.txt", &data);
    if (handle == -1) return false;
    close(handle);
    return std::wcscmp(data.name, L"a.txt") == 0 && data.size == kPayloadSize;
}

std::wstring full_path(GetFullPathNameWFunction function, const wchar_t* path) {
    wchar_t buffer[MAX_PATH] = L"";
    LPWSTR file_part = nullptr;
    const DWORD length = function(path, MAX_PATH, buffer, &file_part);
    return length > 0 && length < MAX_PATH ? std::wstring(buffer, length) : std::wstring{};
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

TEST(drives_forwarders_create_and_remove_directories) {
    MountedFolder mounted;
    const struct {
        const wchar_t* dll;
        const char* drive_path;
        const wchar_t* host_name;
    } cases[] = {{L"g6shim_file.dll", "Y:\\made_file", L"made_file"},
                 {L"g6shim_kernel32.dll", "Y:/made_kernel32", L"made_kernel32"}};
    for (const auto& item : cases) {
        const Module module(item.dll);
        const auto create = module.get<CreateDirectoryAFunction>("CreateDirectoryA");
        CHECK(create != nullptr);
        if (create == nullptr) continue;
        CHECK(create(item.drive_path, nullptr));
        CHECK(is_directory(mounted.host(item.host_name)));
        CHECK(!create(item.drive_path, nullptr));
        CHECK(GetLastError() == ERROR_ALREADY_EXISTS);
    }
    const Module crt(L"g6shim_crtfs.dll");
    const auto mkdir_narrow = crt.get<DirectoryAFunction>("_mkdir");
    const auto mkdir_wide = crt.get<DirectoryWFunction>("_wmkdir");
    const auto rmdir_wide = crt.get<DirectoryWFunction>("_wrmdir");
    CHECK(mkdir_narrow != nullptr && mkdir_wide != nullptr && rmdir_wide != nullptr);
    if (mkdir_narrow == nullptr || mkdir_wide == nullptr || rmdir_wide == nullptr) return;
    CHECK(mkdir_narrow("Y:\\crt_narrow") == 0);
    CHECK(is_directory(mounted.host(L"crt_narrow")));
    CHECK(mkdir_wide(L"Y:/crt_wide") == 0);
    CHECK(is_directory(mounted.host(L"crt_wide")));
    CHECK(rmdir_wide(L"Y:\\crt_wide") == 0);
    CHECK(!is_directory(mounted.folder.path + L"\\crt_wide"));
    CHECK(rmdir_wide(L"Y:\\crt_wide") == -1);
}

TEST(drives_forwarders_find_files_on_a_mounted_drive) {
    MountedFolder mounted;
    mounted.write(L"a.txt");
    for (const wchar_t* dll : {L"g6shim_file.dll", L"g6shim_kernel32.dll"}) {
        const Module module(dll);
        const auto find = module.get<FindFirstFileExAFunction>("FindFirstFileExA");
        CHECK(find != nullptr);
        if (find == nullptr) continue;
        WIN32_FIND_DATAA data{};
        const HANDLE handle = find("Y:\\*.txt", FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
        CHECK(handle != INVALID_HANDLE_VALUE);
        if (handle == INVALID_HANDLE_VALUE) continue;
        CHECK(std::strcmp(data.cFileName, "a.txt") == 0);
        CHECK(data.nFileSizeLow == kPayloadSize);
        WIN32_FIND_DATAA next{};
        CHECK(!FindNextFileA(handle, &next));
        FindClose(handle);
        CHECK(find("Y:\\*.none", FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0) == INVALID_HANDLE_VALUE);
        CHECK(GetLastError() == ERROR_FILE_NOT_FOUND);
    }
    const Module crt(L"g6shim_crtfs.dll");
    CHECK(crt_finds_narrow<_finddata32_t>(crt, "_findfirst32"));
    CHECK(crt_finds_narrow<_finddata32i64_t>(crt, "_findfirst32i64"));
    CHECK(crt_finds_narrow<__finddata64_t>(crt, "_findfirst64"));
    CHECK(crt_finds_narrow<_finddata64i32_t>(crt, "_findfirst64i32"));
    CHECK(crt_finds_wide<_wfinddata32_t>(crt, "_wfindfirst32"));
    CHECK(crt_finds_wide<_wfinddata32i64_t>(crt, "_wfindfirst32i64"));
    CHECK(crt_finds_wide<_wfinddata64_t>(crt, "_wfindfirst64"));
    CHECK(crt_finds_wide<_wfinddata64i32_t>(crt, "_wfindfirst64i32"));
}

TEST(drives_forwarders_keep_full_paths_on_the_drive) {
    MountedFolder mounted;
    for (const wchar_t* dll : {L"g6shim_file.dll", L"g6shim_kernel32.dll"}) {
        const Module module(dll);
        const auto function = module.get<GetFullPathNameWFunction>("GetFullPathNameW");
        CHECK(function != nullptr);
        if (function == nullptr) continue;
        CHECK(full_path(function, L"Y:\\a\\..\\b.txt") == L"Y:\\b.txt");
        CHECK(full_path(function, L"Y:/a/./b.txt") == L"Y:\\a\\b.txt");
        CHECK(full_path(function, L"Y:b.txt") == L"Y:\\b.txt");
        CHECK(full_path(function, L"Y:") == L"Y:\\");
        CHECK(full_path(function, L"relative.txt") == full_path(&GetFullPathNameW, L"relative.txt"));
        wchar_t small[4] = L"";
        CHECK(function(L"Y:\\a\\b.txt", 4, small, nullptr) == sizeof "Y:\\a\\b.txt");
    }
}

TEST(drives_forwarders_copy_files_between_drive_paths) {
    MountedFolder mounted;
    mounted.write(L"a.txt");
    const Module kernel32(L"g6shim_kernel32.dll");
    const auto copy = kernel32.get<CopyFileWFunction>("CopyFileW");
    CHECK(copy != nullptr);
    if (copy != nullptr) {
        CHECK(copy(L"Y:\\a.txt", L"Y:/b.txt", TRUE));
        CHECK(file_size(mounted.host(L"b.txt")) == kPayloadSize);
        CHECK(!copy(L"Y:\\a.txt", L"Y:\\b.txt", TRUE));
        CHECK(GetLastError() == ERROR_FILE_EXISTS);
    }
    for (const wchar_t* dll : {L"g6shim_file2.dll", L"g6shim_kernel32.dll"}) {
        const Module module(dll);
        const auto copy2 = module.get<CopyFile2Function>("CopyFile2");
        CHECK(copy2 != nullptr);
        if (copy2 == nullptr) continue;
        const std::wstring target = std::wstring(L"c_") + dll + L".txt";
        CHECK(copy2(L"Y:\\a.txt", (L"Y:\\" + target).c_str(), nullptr) == S_OK);
        CHECK(file_size(mounted.host(target.c_str())) == kPayloadSize);
        CHECK(copy2(L"Y:\\missing.txt", L"Y:\\d.txt", nullptr) == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    }
}

TEST(drives_mount_persistent_storage_on_a_free_letter) {
    const std::wstring root = exe_folder() + L"\\g6shim_persistent";
    const int letter = persistent_storage_drive();
    CHECK(letter >= L'D');
    CHECK(letter <= L'Z');
    CHECK(letter != L'G');
    CHECK(((GetLogicalDrives() >> (letter - L'A')) & 1u) == 0);
    const DWORD attributes = GetFileAttributesW(root.c_str());
    CHECK(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    std::wstring drive(1, static_cast<wchar_t>(letter));
    drive += L":";
    CHECK(is(translate((drive + L"\\saves\\slot0.bin").c_str()), root + L"\\saves\\slot0.bin"));
    CHECK(is(translate(drive.c_str()), root + L"\\"));
    CHECK(is(translate(L"G:\\data"), exe_folder() + L"\\data"));
    CHECK(persistent_storage_drive() == letter);

    TempFolder other;
    const int other_letter = mount_drive(other.path.c_str());
    CHECK(other_letter != letter);
    CHECK(persistent_storage_drive() == letter);

    mount_drive_at(static_cast<wchar_t>(letter), other.path.c_str());
    const int moved = persistent_storage_drive();
    CHECK(moved != 0);
    CHECK(moved != letter);
    std::wstring moved_drive(1, static_cast<wchar_t>(moved));
    moved_drive += L":\\a";
    CHECK(is(translate(moved_drive.c_str()), root + L"\\a"));
    RemoveDirectoryW(root.c_str());
}

namespace {

constexpr wchar_t kTemporaryChildVariable[] = L"G6SHIM_TEST_TEMPORARY_CHILD";
constexpr char kTemporaryChildTest[] = "drives_temporary_storage_child";
constexpr char kTemporaryRootLine[] = "temporary root: ";
constexpr DWORD kChildTimeoutMs = 30000;

std::wstring drive_root(int letter) {
    std::wstring spec(1, static_cast<wchar_t>(letter));
    spec += L":";
    const auto root = translate(spec.c_str());
    return root ? *root : std::wstring{};
}

std::wstring widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

bool folder_exists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::string run_child(DWORD* exit_code) {
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &inherit, 0)) return {};
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(exe) + L"\" " + widen(kTemporaryChildTest);
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    SetEnvironmentVariableW(kTemporaryChildVariable, L"1");
    const BOOL started =
        CreateProcessW(exe, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    SetEnvironmentVariableW(kTemporaryChildVariable, nullptr);
    CloseHandle(write_end);
    std::string output;
    if (started) {
        char buffer[512];
        DWORD read = 0;
        while (ReadFile(read_end, buffer, sizeof buffer, &read, nullptr) && read > 0) output.append(buffer, read);
        WaitForSingleObject(process.hProcess, kChildTimeoutMs);
        GetExitCodeProcess(process.hProcess, exit_code);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    CloseHandle(read_end);
    return output;
}

}  // namespace

TEST(drives_temporary_storage_child) {
    if (GetEnvironmentVariableW(kTemporaryChildVariable, nullptr, 0) == 0) return;
    const int letter = temporary_storage_drive();
    CHECK(letter != 0);
    const std::wstring root = drive_root(letter);
    std::wstring probe(1, static_cast<wchar_t>(letter));
    probe += L":\\nested\\probe.bin";
    CHECK(CreateDirectoryW((root + L"nested").c_str(), nullptr));
    const auto host = translate(probe.c_str());
    CHECK(host.has_value());
    const HANDLE file = host ? CreateFileW(host->c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr)
                             : INVALID_HANDLE_VALUE;
    CHECK(file != INVALID_HANDLE_VALUE);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    std::printf("%s%ls\n", kTemporaryRootLine, root.c_str());
}

TEST(drives_mount_temporary_storage_and_remove_it_on_exit) {
    const bool t_taken = ((GetLogicalDrives() >> (L'T' - L'A')) & 1u) != 0 || translate(L"T:").has_value();
    const int letter = temporary_storage_drive();
    CHECK(letter != 0);
    CHECK(letter != L'G');
    if (!t_taken) CHECK(letter == L'T');
    const std::wstring root = drive_root(letter);
    CHECK(folder_exists(root));
    CHECK(root.find(L"g6shim_temp_") != std::wstring::npos);
    std::wstring file(1, static_cast<wchar_t>(letter));
    file += L":\\a\\b.bin";
    CHECK(is(translate(file.c_str()), root + L"a\\b.bin"));
    CHECK(temporary_storage_drive() == letter);
    CHECK(drive_root(letter) == root);

    DWORD exit_code = 1;
    const std::string output = run_child(&exit_code);
    CHECK(exit_code == 0);
    const std::size_t at = output.find(kTemporaryRootLine);
    CHECK(at != std::string::npos);
    if (at == std::string::npos) return;
    const std::size_t start = at + std::strlen(kTemporaryRootLine);
    const std::size_t end = output.find_first_of("\r\n", start);
    const std::wstring child_root = widen(output.substr(start, end - start));
    CHECK(!child_root.empty());
    CHECK(child_root != root);
    CHECK(!folder_exists(child_root));
}

TEST(drives_remove_a_folder_held_without_delete_sharing) {
    wchar_t base[MAX_PATH] = L"";
    CHECK(GetTempPathW(MAX_PATH, base) != 0);
    const std::wstring root = std::wstring(base) + L"g6shim_remove_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                              std::to_wstring(GetTickCount64());
    CHECK(CreateDirectoryW(root.c_str(), nullptr));
    const std::wstring file = root + L"\\held.bin";
    const HANDLE created =
        CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(created != INVALID_HANDLE_VALUE);
    if (created != INVALID_HANDLE_VALUE) CloseHandle(created);

    const HANDLE opened = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK(opened != nullptr);
    if (opened == nullptr) return;
    constexpr DWORD kHoldWithoutDeleteMs = 100;
    std::thread holder([&] {
        const HANDLE held =
            CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (held == INVALID_HANDLE_VALUE) return;
        SetEvent(opened);
        Sleep(kHoldWithoutDeleteMs);
        CloseHandle(held);
    });
    const DWORD wait = WaitForSingleObject(opened, 5000);
    g6shim::drives::detail::remove_folder_with_retries(root);
    holder.join();
    CloseHandle(opened);
    const bool gone = !folder_exists(root);
    if (!gone) {
        DeleteFileW(file.c_str());
        RemoveDirectoryW(root.c_str());
    }
    CHECK(wait == WAIT_OBJECT_0);
    CHECK(gone);
}
