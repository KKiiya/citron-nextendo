// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <optional>

#include "common/nextendo_account.h"
#include "core/hle/service/acc/nextendo_baas_nsa.h"

#ifdef ENABLE_WEB_SERVICE
#include "web_service/nextendo_api.h"
#endif

namespace Service::Account {

namespace {

bool Enabled() {
    const char* env = std::getenv("NEXTENDO_NSA_BAAS");
    return env == nullptr || std::string_view{env} != "0";
}

std::mutex g_mutex;
u64 g_pid = 0;
u64 g_nsa = 0;
std::optional<std::chrono::steady_clock::time_point> g_asked_at;
std::atomic<bool> g_mode{false};

} // Anonymous namespace

bool TitleUsesBaasNsa(u64 program_id) {
    return program_id == AcnhProgramId;
}

u64 ParseBaasNsaHex(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    if (value.size() != 16) {
        return 0;
    }
    u64 out = 0;
    for (const char c : value) {
        u64 digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<u64>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<u64>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<u64>(c - 'A' + 10);
        } else {
            return 0;
        }
        out = (out << 4) | digit;
    }
    return out;
}

u64 ChooseAnnouncedNsa(u64 program_id, u64 pid, u64 baas_nsa) {
    return TitleUsesBaasNsa(program_id) && baas_nsa != 0 ? baas_nsa : pid;
}

u64 GetLinkedAccountBaasNsa() {
    if (!Enabled() || !Common::NextendoAccount::IsLinked()) {
        return 0;
    }
    const u64 pid = Common::NextendoAccount::GetPid();
    if (pid == 0) {
        return 0;
    }

    {
        std::scoped_lock lock{g_mutex};
        if (g_pid != pid) {
            g_pid = pid;
            g_nsa = 0;
            g_asked_at.reset();
        }
        if (g_nsa != 0) {
            return g_nsa;
        }
        const auto now = std::chrono::steady_clock::now();
        if (g_asked_at && now - *g_asked_at < std::chrono::minutes{1}) {
            return 0;
        }
        g_asked_at = now;
    }

    u64 asked = 0;
#ifdef ENABLE_WEB_SERVICE
    if (const auto nsa = WebService::NextendoApi::GetAccountNsa()) {
        asked = ParseBaasNsaHex(*nsa);
    }
#endif

    std::scoped_lock lock{g_mutex};
    // The link may have changed meanwhile: never keep one account's NSA under another.
    if (g_pid == pid && g_nsa == 0 && asked != 0 && Common::NextendoAccount::GetPid() == pid) {
        g_nsa = asked;
    }
    return g_pid == pid ? g_nsa : 0;
}

void SetBaasNsaMode(bool active) {
    g_mode.store(active);
}

bool IsBaasNsaModeActive() {
    return g_mode.load();
}

} // namespace Service::Account
