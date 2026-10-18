// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "common/common_types.h"

namespace Service::Account {

// Country of the linked Nextendo account, as two upper-case ASCII letters, handed to games in the
// Nintendo Account user base (see nextendo_cached_account_info.h). The source is the account's own
// country on the Nextendo site (/api/me, account.country): the player picks it at sign-up and can
// change it later, and it is the same field the account server puts in the Nintendo Account
// profile of real consoles, so an account shows the same flag on both. An account without a
// country gets an empty field (no flag), never a guessed one.
class NextendoAccountCountryCache {
public:
    // Returns the account's country ("" when it has none), or nullopt when the lookup failed.
    using Fetcher = std::function<std::optional<std::string>(u64 pid)>;

    struct Timing {
        // Longest a caller waits for the very first answer for an account.
        std::chrono::milliseconds first_wait{3000};
        // Age after which a known country is looked up again (in the background).
        std::chrono::milliseconds known_ttl{std::chrono::minutes{10}};
        // Same, when the account has no country or the site could not be reached.
        std::chrono::milliseconds unknown_ttl{std::chrono::minutes{1}};
    };

    explicit NextendoAccountCountryCache(Fetcher fetcher);
    NextendoAccountCountryCache(Fetcher fetcher, Timing timing);
    ~NextendoAccountCountryCache();

    // The country for pid, or "". A cached answer is returned at once and refreshed in the
    // background once it is old; only the first call for an account waits, at most first_wait.
    std::string Get(u64 pid);

    // Starts a background lookup for pid unless a fresh answer is cached or one is running.
    void Prepare(u64 pid);

private:
    struct State;
    std::shared_ptr<State> state;
};

// False when NEXTENDO_PAYS_COMPTE is "0": the field then stays empty, as before.
bool IsNextendoAccountCountryEnabled();

// Process-wide cache for the linked account, backed by the Nextendo site. Both return at once
// (Get: "") when the feature is off, no account is linked, or the build has no web service.
std::string GetNextendoAccountCountry();
void PrepareNextendoAccountCountry();

} // namespace Service::Account
