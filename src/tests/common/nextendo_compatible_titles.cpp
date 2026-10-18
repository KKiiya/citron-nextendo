// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/nextendo_compatible_titles.h"

namespace {

constexpr u64 MarioKart8Deluxe = 0x0100152000022000;
constexpr u64 SuperSmashBrosUltimate = 0x01006a800016e000;
constexpr u64 DragonBallFighterZ = 0x0100a250097f0000;
constexpr u64 Diablo3 = 0x01001b300b9be000;
constexpr u64 NotTracked = 0x0100000000ffffff;

} // Anonymous namespace

TEST_CASE("Nextendo::CompatibleTitles::CompareVersions", "[common][nextendo]") {
    using Nextendo::CompatibleTitles::CompareVersions;

    REQUIRE(CompareVersions("4.0.1", "4.0.1") == 0);
    REQUIRE(CompareVersions("4.0.0", "4.0.1") < 0);
    REQUIRE(CompareVersions("4.0.2", "4.0.1") > 0);
    REQUIRE(CompareVersions("4.1.0", "4.0.9") > 0);
    REQUIRE(CompareVersions("10.0.0", "9.9.9") > 0); // Numeric, not lexicographic.
    REQUIRE(CompareVersions("3.10.0", "3.9.0") > 0);
    REQUIRE(CompareVersions("1.1.1.0", "1.1.1") == 0); // Missing parts count as 0.
    REQUIRE(CompareVersions("1.1.1", "1.1.1.1") < 0);
    REQUIRE(CompareVersions("01.50", "1.50") == 0); // Leading zeros.
    REQUIRE(CompareVersions("1.08.00", "1.8") == 0);
    REQUIRE(CompareVersions(" 4.0.1 ", "4.0.1") == 0);
    REQUIRE(CompareVersions("2.7.7.92381", "2.7.7.92380") > 0);

    REQUIRE_FALSE(CompareVersions("", "4.0.1").has_value());
    REQUIRE_FALSE(CompareVersions("v4.0.1", "4.0.1").has_value());
    REQUIRE_FALSE(CompareVersions("4.0.1b", "4.0.1").has_value());
    REQUIRE_FALSE(CompareVersions("4..1", "4.0.1").has_value());
    REQUIRE_FALSE(CompareVersions("4.0.", "4.0.1").has_value());
    REQUIRE_FALSE(CompareVersions("-4.0.1", "4.0.1").has_value());
    REQUIRE_FALSE(CompareVersions("99999999999999999999.0", "4.0.1").has_value());
}

TEST_CASE("Nextendo::CompatibleTitles::IsVersionOk accepts only the exact required version",
          "[common][nextendo]") {
    using Nextendo::CompatibleTitles::IsVersionOk;
    using Nextendo::CompatibleTitles::Table;

    REQUIRE(Table().at(MarioKart8Deluxe) == "4.0.1");
    REQUIRE(IsVersionOk(MarioKart8Deluxe, "4.0.1"));
    REQUIRE(IsVersionOk(MarioKart8Deluxe, "4.0.1.0"));
    REQUIRE(IsVersionOk(MarioKart8Deluxe, " 4.0.1 "));
    // A newer update that Nextendo hasn't prepared yet is refused, like an older one.
    REQUIRE_FALSE(IsVersionOk(MarioKart8Deluxe, "4.0.2"));
    REQUIRE_FALSE(IsVersionOk(MarioKart8Deluxe, "4.1.0"));
    REQUIRE_FALSE(IsVersionOk(MarioKart8Deluxe, "5.0.0"));
    REQUIRE_FALSE(IsVersionOk(MarioKart8Deluxe, "4.0.0"));
    REQUIRE_FALSE(IsVersionOk(MarioKart8Deluxe, "3.0.5"));

    REQUIRE(IsVersionOk(SuperSmashBrosUltimate, "13.0.5"));
    REQUIRE_FALSE(IsVersionOk(SuperSmashBrosUltimate, "13.0.6"));
    REQUIRE_FALSE(IsVersionOk(SuperSmashBrosUltimate, "13.0.4"));
    REQUIRE_FALSE(IsVersionOk(SuperSmashBrosUltimate, "9.0.0"));

    REQUIRE(IsVersionOk(DragonBallFighterZ, "01.50"));
    REQUIRE(IsVersionOk(DragonBallFighterZ, "1.50"));
    REQUIRE_FALSE(IsVersionOk(DragonBallFighterZ, "01.51"));
    REQUIRE_FALSE(IsVersionOk(DragonBallFighterZ, "01.49"));

    REQUIRE(IsVersionOk(Diablo3, "2.7.7.92380"));
    REQUIRE_FALSE(IsVersionOk(Diablo3, "2.7.8"));
    REQUIRE_FALSE(IsVersionOk(Diablo3, "2.7.7"));
}

TEST_CASE("Nextendo::CompatibleTitles::IsVersionOk edge cases", "[common][nextendo]") {
    using Nextendo::CompatibleTitles::IsVersionOk;

    // Unknown installed version and untracked titles are let through, as before.
    REQUIRE(IsVersionOk(MarioKart8Deluxe, ""));
    REQUIRE(IsVersionOk(NotTracked, "0.0.1"));
    REQUIRE(IsVersionOk(NotTracked, "anything"));

    // A version that can't be compared numerically never passes unless it matches exactly.
    REQUIRE_FALSE(IsVersionOk(MarioKart8Deluxe, "v4.0.2"));
    REQUIRE_FALSE(IsVersionOk(MarioKart8Deluxe, "4.0.1-beta"));
}

TEST_CASE("Nextendo::CompatibleTitles table versions are all numeric", "[common][nextendo]") {
    for (const auto& [program_id, version] : Nextendo::CompatibleTitles::Table()) {
        INFO("program_id=" << program_id << " version=" << version);
        REQUIRE(Nextendo::CompatibleTitles::ParseVersion(version).has_value());
        REQUIRE(Nextendo::CompatibleTitles::IsVersionOk(program_id, version));
    }
}
