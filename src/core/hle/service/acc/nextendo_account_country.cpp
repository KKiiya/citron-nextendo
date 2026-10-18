// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "common/logging.h"
#include "common/nextendo_account.h"
#include "core/hle/service/acc/nextendo_account_country.h"
#include "core/hle/service/acc/nextendo_cached_account_info.h"

#ifdef ENABLE_WEB_SERVICE
#include "web_service/nextendo_api.h"
#endif

namespace Service::Account {

struct NextendoAccountCountryCache::State {
    using Clock = std::chrono::steady_clock;

    State(Fetcher fetcher_, Timing timing_) : fetcher{std::move(fetcher_)}, timing{timing_} {}

    bool IsFresh(u64 for_pid, Clock::time_point now) const {
        if (!has_value || pid != for_pid) {
            return false;
        }
        const auto ttl = country.empty() ? timing.unknown_ttl : timing.known_ttl;
        return now - at < ttl;
    }

    // Starts one background lookup; the caller holds mutex. The thread owns a reference to the
    // state, so it may outlive the cache object.
    static void StartLookup(const std::shared_ptr<State>& self, u64 for_pid) {
        if (self->asking) {
            return;
        }
        self->asking = true;
        try {
            std::thread([self, for_pid] { self->RunLookup(for_pid); }).detach();
        } catch (const std::system_error&) {
            self->asking = false;
        }
    }

    void RunLookup(u64 for_pid) {
        std::optional<std::string> result;
        try {
            result = fetcher(for_pid);
        } catch (const std::exception&) {
            result.reset();
        }

        std::scoped_lock lock{mutex};
        asking = false;

        std::string value = result ? NormalizeNextendoCountry(*result) : std::string{};
        // Site unreachable: keep the last country known for this account.
        if (!result && has_value && pid == for_pid) {
            value = country;
        }

        has_value = true;
        pid = for_pid;
        country = std::move(value);
        at = Clock::now();
        answered.notify_all();
    }

    const Fetcher fetcher;
    const Timing timing;

    std::mutex mutex;
    std::condition_variable answered;
    bool has_value = false;
    bool asking = false;
    u64 pid = 0;
    std::string country;
    Clock::time_point at{};
};

NextendoAccountCountryCache::NextendoAccountCountryCache(Fetcher fetcher)
    : NextendoAccountCountryCache(std::move(fetcher), Timing{}) {}

NextendoAccountCountryCache::NextendoAccountCountryCache(Fetcher fetcher, Timing timing)
    : state{std::make_shared<State>(std::move(fetcher), timing)} {}

NextendoAccountCountryCache::~NextendoAccountCountryCache() = default;

std::string NextendoAccountCountryCache::Get(u64 pid) {
    std::unique_lock lock{state->mutex};

    if (state->has_value && state->pid == pid) {
        if (!state->IsFresh(pid, State::Clock::now())) {
            State::StartLookup(state, pid);
        }
        return state->country;
    }

    State::StartLookup(state, pid);
    const bool answered = state->answered.wait_for(lock, state->timing.first_wait, [&] {
        return state->has_value && state->pid == pid;
    });
    return answered ? state->country : std::string{};
}

void NextendoAccountCountryCache::Prepare(u64 pid) {
    std::scoped_lock lock{state->mutex};
    if (!state->IsFresh(pid, State::Clock::now())) {
        State::StartLookup(state, pid);
    }
}

bool IsNextendoAccountCountryEnabled() {
    const char* env = std::getenv("NEXTENDO_PAYS_COMPTE");
    return env == nullptr || std::string_view{env} != "0";
}

namespace {

#ifdef ENABLE_WEB_SERVICE
constexpr bool HasWebService = true;
#else
constexpr bool HasWebService = false;
#endif

std::optional<std::string> FetchLinkedAccountCountry(u64 pid) {
#ifdef ENABLE_WEB_SERVICE
    // The request carries the stored token: drop the answer if the link changed meanwhile, so one
    // account's country is never cached under another.
    if (Common::NextendoAccount::GetPid() != pid) {
        return std::nullopt;
    }
    auto country = WebService::NextendoApi::GetAccountCountry();
    if (Common::NextendoAccount::GetPid() != pid) {
        return std::nullopt;
    }
    if (!country) {
        LOG_WARNING(Service_ACC, "[Nextendo] Account country lookup failed");
    }
    return country;
#else
    (void)pid;
    return std::nullopt;
#endif
}

NextendoAccountCountryCache& SharedCache() {
    static NextendoAccountCountryCache cache{&FetchLinkedAccountCountry};
    return cache;
}

std::optional<u64> LinkedPidForCountry() {
    if (!HasWebService || !IsNextendoAccountCountryEnabled() ||
        !Common::NextendoAccount::IsLinked()) {
        return std::nullopt;
    }
    const u64 pid = Common::NextendoAccount::GetPid();
    if (pid == 0) {
        return std::nullopt;
    }
    return pid;
}

} // Anonymous namespace

std::string GetNextendoAccountCountry() {
    const auto pid = LinkedPidForCountry();
    return pid ? SharedCache().Get(*pid) : std::string{};
}

void PrepareNextendoAccountCountry() {
    if (const auto pid = LinkedPidForCountry()) {
        SharedCache().Prepare(*pid);
    }
}

} // namespace Service::Account
