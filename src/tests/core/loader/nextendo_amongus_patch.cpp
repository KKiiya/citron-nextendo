// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/loader/nextendo_amongus_patch.h"

namespace {

constexpr u64 AmongUs = 0x0100B0C013912000ULL;
constexpr std::size_t kNsoHeaderSize = 0x100;

// ldr x8,[x0,#0x30] ; cbz x8,+0x18 ; ldrb w0,[x8,#0x22] ; ret
constexpr std::array<u8, 16> kGetter{{0x08, 0x18, 0x40, 0xF9, 0x68, 0x00, 0x00, 0xB4, 0x00, 0x89,
                                      0x40, 0x39, 0xC0, 0x03, 0x5F, 0xD6}};
constexpr std::array<u8, 8> kPatched{{0x00, 0x00, 0x80, 0x52, 0xC0, 0x03, 0x5F, 0xD6}};

// [NSOHeader][program image] filled with nops, with the given getters placed at program offsets.
std::vector<u8> Image(std::initializer_list<std::pair<std::size_t, std::array<u8, 16>>> getters) {
    std::vector<u8> image(kNsoHeaderSize + 0x400);
    for (std::size_t i = kNsoHeaderSize; i < image.size(); i += 4) {
        image[i] = 0x1F;
        image[i + 1] = 0x20;
        image[i + 2] = 0x03;
        image[i + 3] = 0xD5; // nop
    }
    for (const auto& [offset, code] : getters) {
        std::copy(code.begin(), code.end(), image.begin() + kNsoHeaderSize + offset);
    }
    return image;
}

} // Anonymous namespace

TEST_CASE("Nextendo::AmongUsPatch patches the only getter", "[core][nextendo]") {
    const auto before = Image({{0x100, kGetter}});
    const auto after = Loader::NextendoAmongUsPatch::ApplyIfMatch(AmongUs, before);

    REQUIRE(after.size() == before.size());
    REQUIRE(std::equal(kPatched.begin(), kPatched.end(), after.begin() + kNsoHeaderSize + 0x100));
    // Only those 8 bytes changed.
    std::size_t changed = 0;
    for (std::size_t i = 0; i < before.size(); ++i) {
        changed += before[i] != after[i];
    }
    REQUIRE(changed == 8); // the first 8 bytes of the getter all differ; the other 8 are untouched
}

TEST_CASE("Nextendo::AmongUsPatch ignores other titles and unknown code", "[core][nextendo]") {
    const auto with_getter = Image({{0x100, kGetter}});
    REQUIRE(Loader::NextendoAmongUsPatch::ApplyIfMatch(0x0100F43008C44000ULL, with_getter) ==
            with_getter);

    const auto none = Image({});
    REQUIRE(Loader::NextendoAmongUsPatch::ApplyIfMatch(AmongUs, none) == none);

    // Two matches are ambiguous: leave the image alone rather than guess.
    const auto two = Image({{0x100, kGetter}, {0x200, kGetter}});
    REQUIRE(Loader::NextendoAmongUsPatch::ApplyIfMatch(AmongUs, two) == two);
}

TEST_CASE("Nextendo::AmongUsPatch tolerates a different cbz distance but not other code",
          "[core][nextendo]") {
    auto other_jump = kGetter;
    other_jump[4] = 0xA8;
    other_jump[5] = 0x01;
    const auto patched = Loader::NextendoAmongUsPatch::ApplyIfMatch(AmongUs, Image({{0x80, other_jump}}));
    REQUIRE(std::equal(kPatched.begin(), kPatched.end(), patched.begin() + kNsoHeaderSize + 0x80));

    auto other_register = kGetter;
    other_register[4] = 0x69; // cbz x9
    const auto untouched = Image({{0x100, other_register}});
    REQUIRE(Loader::NextendoAmongUsPatch::ApplyIfMatch(AmongUs, untouched) == untouched);

    auto other_load = kGetter;
    other_load[8] = 0x20; // ldrb with another offset
    const auto untouched2 = Image({{0x100, other_load}});
    REQUIRE(Loader::NextendoAmongUsPatch::ApplyIfMatch(AmongUs, untouched2) == untouched2);
}
