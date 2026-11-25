// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "runtime.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace g6shim::xgameruntime::xstore {

inline constexpr Guid kProvider = guid(0x0dd112ac, 0x7c24, 0x448c, 0xb9, 0x2b, 0x39, 0x60, 0xfb, 0x5b, 0xd3,
                                       0x0c);
inline constexpr Guid kIid2026 = guid(0xb09d803c, 0x2414, 0x4a05, 0x82, 0xc6, 0x66, 0xdf, 0xdc, 0x9e, 0x9a,
                                      0x44);
inline constexpr Guid kDurablesIid = guid(0x2d42fea5, 0xe71d, 0x4b76, 0x97, 0xcd, 0xc5, 0x0a, 0xfb, 0xb3,
                                          0xae, 0x5d);

inline constexpr HResult kNetworkError = static_cast<HResult>(0x89245301u);

inline constexpr std::size_t kStoreIdBytes = 18;
inline constexpr std::size_t kTrialIdBytes = 64;
inline constexpr char kSkuSuffix[] = "/0010";

struct GameLicense {
    std::array<char, kStoreIdBytes> sku_store_id;
    bool is_active;
    bool is_trial_owned_by_this_user;
    bool is_disc_license;
    bool is_trial;
    std::uint32_t trial_seconds_remaining;
    std::array<char, kTrialIdBytes> trial_unique_id;
    std::int64_t expiration;
};

static_assert(sizeof(GameLicense) == 104);

extern const Object kObject;

std::span<const Interface> interfaces();

}  // namespace g6shim::xgameruntime::xstore
