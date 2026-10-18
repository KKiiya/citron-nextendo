// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

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

// SDK CachedNintendoAccountInfo::GetCountry reads a NUL-terminated string of at most 4 bytes at
// 0x20 of the user base (GetGender reads 0x10, GetRegion 0x24). Mario Kart 8 Deluxe copies it into
// its online profile, which is where the other players' flag comes from.
inline constexpr std::size_t NextendoCachedAccountCountryOffset = 0x20;

// Two ASCII letters, upper-cased; anything else becomes "" so the field stays empty rather than
// carrying something the game would misread.
inline std::string NormalizeNextendoCountry(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    if (value.size() != 2) {
        return {};
    }

    std::string out(2, '\0');
    for (std::size_t i = 0; i < 2; ++i) {
        char c = value[i];
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        if (c < 'A' || c > 'Z') {
            return {};
        }
        out[i] = c;
    }
    return out;
}

// The user base with the account's country at 0x20. An empty or invalid country leaves the field
// zeroed, which is what the game always got before (no flag).
inline std::array<std::uint8_t, 0x68> MakeNextendoCachedAccountInfo(std::string_view country) {
    auto data = NextendoCachedAccountInfo;
    const std::string code = NormalizeNextendoCountry(country);
    for (std::size_t i = 0; i < code.size(); ++i) {
        data[NextendoCachedAccountCountryOffset + i] = static_cast<std::uint8_t>(code[i]);
    }
    return data;
}
} // namespace Service::Account
