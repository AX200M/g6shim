// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>
#include <psapi.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace g6shim::xmem {

inline constexpr std::size_t kPageBytes = 64 << 10;
inline constexpr std::size_t kPageFrames = kPageBytes >> 12;
inline constexpr std::size_t kFirstPageNumber = 0x10'0000;

struct PageRun {
    HANDLE section;
    std::size_t index;
    std::size_t pages;
};

struct PhysicalPage {
    HANDLE section;
    std::size_t index;
};

struct PhysicalMapping {
    std::size_t address;
    std::size_t bytes;
    PhysicalPage page;
};

bool find_physical_page(std::size_t number, PhysicalPage* page);
bool find_physical_mapping(std::size_t address, PhysicalMapping* mapping);
bool find_stretch(std::size_t address, std::size_t bytes, HANDLE* section, std::uint64_t* offset);

class PageWindow {
public:
    PageWindow() noexcept = default;
    ~PageWindow();
    PageWindow(PageWindow&& other) noexcept;
    PageWindow& operator=(PageWindow&& other) noexcept;
    PageWindow(const PageWindow&) = delete;
    PageWindow& operator=(const PageWindow&) = delete;

    std::span<unsigned char> bytes() noexcept;

private:
    friend bool open_page_window(const std::size_t*, std::size_t, std::size_t, std::size_t, PageWindow*);
    void close() noexcept;

    void* mapping_ = nullptr;
    std::size_t within_ = 0;
    std::size_t length_ = 0;
};

bool page_runs(const std::size_t* pages, std::size_t count, std::size_t offset, std::size_t bytes,
               std::vector<PageRun>* runs);
bool open_page_window(const std::size_t* pages, std::size_t count, std::size_t offset, std::size_t bytes,
                      PageWindow* window);
bool write_physical(const std::size_t* pages, std::size_t count, std::size_t offset, const void* data,
                    std::size_t bytes);
bool map_alias(std::uintptr_t to, std::uintptr_t from, std::size_t bytes);

std::string commit_limit_message(std::size_t bytes, std::uint32_t error, const PERFORMANCE_INFORMATION* info);

}  // namespace g6shim::xmem
