// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>

namespace Service::Account {
// Sets birthday if they dont have one set
inline constexpr auto NextendoCachedAccountInfo = [] {
    std::array<std::uint8_t, 0x68> data{};
    constexpr char birthday[] = "2000-01-01";
    for (std::size_t i = 0; i < sizeof(birthday); ++i) {
        data[i] = static_cast<std::uint8_t>(birthday[i]);
    }
    return data;
}();
} // namespace Service::Account
