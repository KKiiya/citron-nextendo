// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/acc/nextendo_account_country.h"
#include "core/hle/service/acc/nextendo_cached_account_info.h"

namespace {

using namespace std::chrono_literals;
using Service::Account::NextendoAccountCountryCache;

// Calls Get until it returns expected or two seconds pass (lookups run on their own thread).
bool EventuallyGets(NextendoAccountCountryCache& cache, u64 pid, const std::string& expected) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cache.Get(pid) == expected) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

NextendoAccountCountryCache::Timing FastTiming() {
    NextendoAccountCountryCache::Timing timing;
    timing.first_wait = 1000ms;
    return timing;
}

} // Anonymous namespace

TEST_CASE("NextendoAccountCountry::UserBaseCountryAt0x20", "[core]") {
    using Service::Account::MakeNextendoCachedAccountInfo;
    using Service::Account::NextendoCachedAccountCountryOffset;
    using Service::Account::NextendoCachedAccountInfo;

    REQUIRE(NextendoCachedAccountCountryOffset == 0x20);

    const auto info = MakeNextendoCachedAccountInfo(" fr ");
    REQUIRE(info[0x20] == 'F');
    REQUIRE(info[0x21] == 'R');
    REQUIRE(info[0x22] == 0);
    REQUIRE(info[0x23] == 0);
    for (std::size_t i = 0; i < info.size(); ++i) {
        if (i != 0x20 && i != 0x21) {
            REQUIRE(info[i] == NextendoCachedAccountInfo[i]);
        }
    }

    // Anything but two ASCII letters leaves the user base exactly as before.
    for (const char* bad : {"", "F", "FRA", "F1", "--", "\xC3\x89"}) {
        REQUIRE(MakeNextendoCachedAccountInfo(bad) == NextendoCachedAccountInfo);
    }
}

TEST_CASE("NextendoAccountCountry::CachedAfterFirstAnswer", "[core]") {
    auto calls = std::make_shared<std::atomic<int>>(0);
    NextendoAccountCountryCache cache{[calls](u64) -> std::optional<std::string> {
                                          ++*calls;
                                          return "de";
                                      },
                                      FastTiming()};

    REQUIRE(cache.Get(1) == "DE");
    REQUIRE(cache.Get(1) == "DE");
    cache.Prepare(1);
    REQUIRE(calls->load() == 1);
}

TEST_CASE("NextendoAccountCountry::FirstWaitIsBounded", "[core]") {
    auto calls = std::make_shared<std::atomic<int>>(0);
    NextendoAccountCountryCache::Timing timing;
    timing.first_wait = 20ms;
    NextendoAccountCountryCache cache{[calls](u64) -> std::optional<std::string> {
                                          ++*calls;
                                          std::this_thread::sleep_for(300ms);
                                          return "JP";
                                      },
                                      timing};

    const auto start = std::chrono::steady_clock::now();
    REQUIRE(cache.Get(1).empty());
    REQUIRE(std::chrono::steady_clock::now() - start < 250ms);

    // The slow answer still lands in the cache, and no second lookup was started meanwhile.
    REQUIRE(EventuallyGets(cache, 1, "JP"));
    REQUIRE(calls->load() == 1);
}

TEST_CASE("NextendoAccountCountry::PrepareFillsInBackground", "[core]") {
    auto calls = std::make_shared<std::atomic<int>>(0);
    NextendoAccountCountryCache::Timing timing;
    timing.first_wait = 0ms;
    NextendoAccountCountryCache cache{[calls](u64) -> std::optional<std::string> {
                                          ++*calls;
                                          return "CA";
                                      },
                                      timing};

    cache.Prepare(1);
    REQUIRE(EventuallyGets(cache, 1, "CA"));
    REQUIRE(calls->load() == 1);
}

TEST_CASE("NextendoAccountCountry::FailureKeepsLastKnownCountry", "[core]") {
    auto calls = std::make_shared<std::atomic<int>>(0);
    NextendoAccountCountryCache::Timing timing = FastTiming();
    timing.known_ttl = 0ms; // every Get refreshes in the background
    NextendoAccountCountryCache cache{[calls](u64) -> std::optional<std::string> {
                                          return ++*calls == 1 ? std::optional<std::string>{"IT"}
                                                               : std::nullopt;
                                      },
                                      timing};

    REQUIRE(cache.Get(1) == "IT");
    REQUIRE(cache.Get(1) == "IT"); // stale: answered at once, refresh started
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (calls->load() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(calls->load() >= 2);
    std::this_thread::sleep_for(50ms);
    REQUIRE(cache.Get(1) == "IT");
}

TEST_CASE("NextendoAccountCountry::NeverAnotherAccountsCountry", "[core]") {
    NextendoAccountCountryCache cache{[](u64 pid) -> std::optional<std::string> {
                                          if (pid == 1) {
                                              return "FR";
                                          }
                                          if (pid == 2) {
                                              return ""; // account without a country
                                          }
                                          return "FRA"; // malformed answer
                                      },
                                      FastTiming()};

    REQUIRE(cache.Get(1) == "FR");
    REQUIRE(cache.Get(2).empty());
    REQUIRE(cache.Get(3).empty());
    REQUIRE(cache.Get(1) == "FR");
}
