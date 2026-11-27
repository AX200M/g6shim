// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "storage.hpp"
#include "title.hpp"
#include "xasync.hpp"

#include <windows.h>

#include <g6shim/drives.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace {

namespace st = g6shim::xgameruntime::storage;
namespace xgr = g6shim::xgameruntime;
using g6shim::xgameruntime::HResult;
using g6shim::xgameruntime::Object;
using g6shim::xgameruntime::xasync::AsyncBlock;

using Size = HResult (*)(Object*, std::size_t*);
using Get = HResult (*)(Object*, std::size_t, std::uint8_t*, std::size_t*);
using Space = HResult (*)(Object*, st::SpaceInfo*);
using Prompt = HResult (*)(Object*, std::uint64_t, AsyncBlock*);
using Result = HResult (*)(Object*, AsyncBlock*);

template <class Function>
Function at(std::size_t offset) {
    return xgr::method<Function>(st::kObject, offset);
}

Object* self() { return const_cast<Object*>(&st::kObject); }

class TempDir {
public:
    TempDir() {
        static unsigned counter = 0;
        std::error_code error;
        path_ = std::filesystem::temp_directory_path(error);
        path_ /= "g6shim_storage_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(counter++);
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

void write_file(const std::filesystem::path& path, std::size_t size) {
    std::ofstream out(path, std::ios::binary);
    const std::string bytes(size, '\0');
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

TEST(xgameruntime_storage_serves_a_drive_over_the_title_folder_and_its_quota) {
    TempDir dir;
    write_file(dir.path() / "save.dat", 100);
    xgr::Title title;
    title.id = 1;
    title.storage = dir.path();
    title.storage_bytes = 1000;
    xgr::set_title(title);

    void* served = nullptr;
    CHECK(xgr::query_api(&st::kProvider, &st::kIid, &served) == xgr::kOk);
    CHECK(served == const_cast<Object*>(&st::kObject));

    std::size_t size = 0;
    CHECK(at<Size>(0x18)(self(), &size) == xgr::kOk);
    CHECK(size == 4);

    std::uint8_t buffer[4] = {};
    std::size_t used = 0;
    const Get get = at<Get>(0x20);
    CHECK(get(self(), 4, buffer, &used) == xgr::kOk);
    CHECK(get(self(), 3, buffer, &used) == st::kFail);

    const std::string drive(reinterpret_cast<const char*>(buffer), 3);
    CHECK(drive.size() == std::size_t{3} && drive[1] == ':' && drive[2] == '\\');
    const std::string path = drive + "save.dat";
    const std::optional<std::wstring> host = g6shim::drives::translate(path.c_str());
    CHECK(host.has_value());
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (host) CHECK(GetFileAttributesExW(host->c_str(), GetFileExInfoStandard, &attributes) != 0);

    st::SpaceInfo info{};
    CHECK(at<Space>(0x28)(self(), &info) == xgr::kOk);
    CHECK(info.used == 100);
    CHECK(info.total_free == 900);
    CHECK(info.total == 1000);
    CHECK(info.available_free <= info.total_free);

    const Prompt prompt = at<Prompt>(0x30);
    const Result result = at<Result>(0x38);
    for (const auto& [bytes, expected] : {std::pair<std::uint64_t, HResult>{900, xgr::kOk},
                                          std::pair<std::uint64_t, HResult>{901, xgr::kAbort}}) {
        AsyncBlock block{};
        CHECK(prompt(self(), bytes, &block) == xgr::kOk);
        CHECK(xgr::xasync::XAsyncGetStatus(&block, true) == expected);
        CHECK(result(self(), &block) == expected);
    }
    xgr::clear_title();
}
