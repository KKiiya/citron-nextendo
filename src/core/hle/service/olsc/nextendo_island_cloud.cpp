// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// [Nextendo] What olsc does at Nintendo, against our server: the archived island of the Nextendo
// account on /api/save/<title> (one slot, the last one sent wins, restore allowed). Used by olsc:u
// during the game and, like the Switch's system service ("the backup happens when you are not
// playing"), after the game. Port of NextendoIslandCloud.cs (Ryujinx-Nextendo 1.9.9).

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/nextendo_account.h"
#include "core/hle/service/olsc/nextendo_island.h"

#ifdef ENABLE_WEB_SERVICE
#include "web_service/nextendo_api.h"
#endif

namespace Service::OLSC::NextendoIsland {

namespace fs = std::filesystem;

namespace {

std::atomic<u64> g_running_title{0};

std::mutex g_idle_mutex;
std::condition_variable g_idle_cv;
int g_background = 0;

fs::path ConfigDir() {
    return Common::FS::GetCitronPath(Common::FS::CitronPath::ConfigDir);
}

void RunInBackground(const char* name, std::function<void()> job) {
    {
        std::lock_guard lk{g_idle_mutex};
        ++g_background;
    }
    std::thread{[name, fn = std::move(job)] {
        fn();
        LOG_DEBUG(Service_OLSC, "[Nextendo] island: {} finished", name);
        std::lock_guard lk{g_idle_mutex};
        --g_background;
        g_idle_cv.notify_all();
    }}.detach();
}

#ifdef ENABLE_WEB_SERVICE

// One upload or restore at a time: an older upload cannot finish after a newer one.
std::mutex g_gate;

std::string TitleHex(u64 title_id) {
    return fmt::format("{:016x}", title_id);
}

Outcome FromStatus(int status) {
    return status == 0 || status >= 500 ? Outcome::NetworkError : Outcome::Refused;
}

std::vector<u8> DecodeCustom(const std::string& b64) {
    auto raw = Base64Decode(b64);
    return raw && raw->size() <= CustomDataSize ? std::move(*raw) : std::vector<u8>{};
}

Outcome UploadLocked(u64 title_id, const fs::path& save_dir, const std::string& key,
                     bool only_if_changed) {
    if (!Usable()) {
        return Outcome::Unavailable;
    }
    if (only_if_changed && IsGameRunning(title_id)) {
        return Outcome::Deferred;
    }
    const std::string hash = Fingerprint(save_dir);
    if (hash.empty()) {
        return Outcome::NoLocalSave;
    }
    const OlscEntry entry = Get(key);
    if (only_if_changed && hash == entry.last_uploaded_hash) {
        return Outcome::Unchanged;
    }

    // Snapshot in memory, checked: a save that changes while it is read is not sent.
    const std::vector<u8> zip = ZipDirectory(save_dir);
    if (zip.empty() || Fingerprint(save_dir) != hash) {
        return Outcome::Deferred;
    }
    const std::vector<u8> custom = DecodeCustom(entry.custom);

    std::vector<std::pair<std::string, std::string>> headers{
        {"X-Nextendo-Device", DeviceId()},
        {"X-Save-Device", DeviceId()},
    };
    if (!custom.empty()) {
        headers.emplace_back("X-Save-Custom", Base64Encode(custom));
    }
    const auto resp = WebService::NextendoApi::IslandRequest(
        "PUT", TitleHex(title_id), "",
        std::string(reinterpret_cast<const char*>(zip.data()), zip.size()), headers, 180);
    if (resp.status != 200) {
        LOG_WARNING(Service_OLSC, "[Nextendo] island: upload refused HTTP {} {}", resp.status,
                    resp.body.substr(0, 200));
        return FromStatus(resp.status);
    }

    CloudIsland sent = ParseMeta(resp.body);
    sent.exists = true;
    sent.custom = custom;
    if (sent.posix_time == 0) {
        sent.posix_time = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    }
    StoreCache(key, sent);
    Update(key, [&hash](OlscEntry& e) { e.last_uploaded_hash = hash; });
    LOG_INFO(Service_OLSC, "[Nextendo] island: rev {} uploaded ({} bytes)", sent.rev, zip.size());
    return Outcome::Ok;
}

Outcome DownloadLocked(u64 title_id, const fs::path& save_dir, const std::string& key) {
    if (!Usable()) {
        return Outcome::Unavailable;
    }
    std::error_code ec;
    if (save_dir.empty() || !fs::is_directory(save_dir, ec)) {
        return Outcome::NoLocalSave;
    }

    const auto resp = WebService::NextendoApi::IslandRequest(
        "GET", TitleHex(title_id), "", {}, {{"X-Nextendo-Device", DeviceId()}}, 180);
    if (resp.status != 200) {
        LOG_WARNING(Service_OLSC, "[Nextendo] island: download HTTP {}", resp.status);
        return resp.status == 204 ? Outcome::Refused : FromStatus(resp.status);
    }
    const std::span<const u8> zip{reinterpret_cast<const u8*>(resp.body.data()),
                                  resp.body.size()};

    const Outcome replaced = ReplaceIsland(save_dir, zip, ConfigDir() / "nextendo_island_backups",
                                           TitleHex(title_id));
    if (replaced != Outcome::Ok) {
        LOG_WARNING(Service_OLSC, "[Nextendo] island: restore not applied ({}), local island kept",
                    OutcomeName(replaced));
        return replaced;
    }

    // The restored island is the cloud's: no need to send it back after the game.
    const std::string restored = Fingerprint(save_dir);
    Update(key, [&restored](OlscEntry& e) {
        e.last_uploaded_hash = restored;
        e.pending_upload = false;
    });
    const auto [rc, island] = GetMeta(title_id);
    if (rc == Outcome::Ok) {
        StoreCache(key, island);
    }
    LOG_INFO(Service_OLSC, "[Nextendo] island: rev {} restored ({} bytes)", resp.save_rev,
             zip.size());
    return Outcome::Ok;
}

#endif // ENABLE_WEB_SERVICE

} // Anonymous namespace

bool Usable() {
#ifdef ENABLE_WEB_SERVICE
    return Common::NextendoAccount::IsLinked() && !Common::NextendoAccount::GetToken().empty();
#else
    return false;
#endif
}

std::string DeviceId() {
    static std::mutex m;
    static std::string id;
    std::lock_guard lk{m};
    if (!id.empty()) {
        return id;
    }
    const fs::path p = ConfigDir() / "nextendo_device_id.txt";
    std::string hex;
    {
        std::ifstream in(p);
        in >> hex;
    }
    if (hex.size() != 8 || !std::all_of(hex.begin(), hex.end(), [](char c) {
            return std::isxdigit(static_cast<unsigned char>(c)) != 0;
        })) {
        std::random_device rd;
        hex = fmt::format("{:08x}", rd());
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream out(p, std::ios::trunc);
        out << hex;
    }
    id = "citron:" + hex;
    return id;
}

fs::path FindDeviceSaveDir(u64 title_id) {
    const fs::path nand = Common::FS::GetCitronPath(Common::FS::CitronPath::NANDDir);
    const fs::path candidates[] = {
        nand / "user" / "save" / "device" / fmt::format("{:016X}", title_id) / "0",
        nand / "user" / "save" / "0000000000000000" / "00000000000000000000000000000000" /
            fmt::format("{:016X}", title_id),
    };
    std::error_code ec;
    for (const auto& dir : candidates) {
        if (fs::is_directory(dir, ec)) {
            return dir;
        }
    }
    return {};
}

std::pair<Outcome, CloudIsland> GetMeta(u64 title_id) {
#ifdef ENABLE_WEB_SERVICE
    if (!Usable()) {
        return {Outcome::Unavailable, {}};
    }
    const auto resp = WebService::NextendoApi::IslandRequest(
        "GET", TitleHex(title_id), "/meta", {}, {{"X-Nextendo-Device", DeviceId()}}, 20);
    if (resp.status != 200) {
        LOG_WARNING(Service_OLSC, "[Nextendo] island: meta HTTP {}", resp.status);
        return {FromStatus(resp.status), {}};
    }
    return {Outcome::Ok, ParseMeta(resp.body)};
#else
    (void)title_id;
    return {Outcome::Unavailable, {}};
#endif
}

void StoreCache(const std::string& key, const CloudIsland& island) {
    Update(key, [&island](OlscEntry& e) {
        e.has_cache = IsRestorable(island);
        e.cache_id = e.has_cache ? island.rev : 0;
        e.cache_time = e.has_cache ? island.posix_time : 0;
        e.cache_custom = e.has_cache ? Base64Encode(island.custom) : "";
    });
}

Outcome Upload(u64 title_id, const fs::path& save_dir, const std::string& key,
               bool only_if_changed) {
#ifdef ENABLE_WEB_SERVICE
    std::lock_guard lk{g_gate};
    return UploadLocked(title_id, save_dir, key, only_if_changed);
#else
    (void)title_id;
    (void)save_dir;
    (void)key;
    (void)only_if_changed;
    return Outcome::Unavailable;
#endif
}

Outcome Download(u64 title_id, const fs::path& save_dir, const std::string& key) {
#ifdef ENABLE_WEB_SERVICE
    std::lock_guard lk{g_gate};
    return DownloadLocked(title_id, save_dir, key);
#else
    (void)title_id;
    (void)save_dir;
    (void)key;
    return Outcome::Unavailable;
#endif
}

void SetRunningTitle(u64 title_id) {
    g_running_title.store(title_id);
}

bool IsGameRunning(u64 title_id) {
    return title_id != 0 && g_running_title.load() == title_id;
}

void MarkPending(const std::string& key, const fs::path& save_dir) {
    const std::string root = save_dir.string();
    Update(key, [&root](OlscEntry& e) {
        e.pending_upload = true;
        e.pending_save_root = root;
    });
}

Outcome UploadPending(u64 title_id, const fs::path& save_dir, const std::string& key) {
    if (IsGameRunning(title_id)) {
        return Outcome::Deferred;
    }
    const Outcome outcome = Upload(title_id, save_dir, key, true);
    if (outcome == Outcome::Ok || outcome == Outcome::Unchanged ||
        outcome == Outcome::NoLocalSave) {
        Update(key, [](OlscEntry& e) { e.pending_upload = false; });
    }
    return outcome;
}

void RunPendingInBackground() {
    RunInBackground("pending backup", [] {
        for (const auto& [key, entry] : AllPending()) {
            u64 title_id = 0;
            try {
                title_id = std::stoull(key.substr(0, key.find(':')), nullptr, 16);
            } catch (...) {
                continue;
            }
            if (!IsStrict(title_id)) {
                continue;
            }
            const Outcome outcome = UploadPending(title_id, fs::path(entry.pending_save_root), key);
            LOG_INFO(Service_OLSC, "[Nextendo] island: pending backup {}", OutcomeName(outcome));
        }
    });
}

void AfterGame(u64 title_id, bool start) {
    if (!IsStrict(title_id)) {
        return;
    }
    const std::string key = LastUserKey(title_id);
    if (key.empty() || Get(key).setting != 1) {
        return; // backup not enabled in the game
    }
    const fs::path save_dir = FindDeviceSaveDir(title_id);
    if (save_dir.empty()) {
        return;
    }
    // Marked pending BEFORE the upload: closing the emulator starts it again on the next launch.
    MarkPending(key, save_dir);
    if (!start) {
        return;
    }
    RunInBackground("after-game backup", [title_id, save_dir, key] {
        const Outcome outcome = UploadPending(title_id, save_dir, key);
        LOG_INFO(Service_OLSC, "[Nextendo] island backup after the game: {}",
                 OutcomeName(outcome));
    });
}

void WaitIdle(int timeout_ms) {
    std::unique_lock lk{g_idle_mutex};
    g_idle_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                       [] { return g_background == 0; });
}

} // namespace Service::OLSC::NextendoIsland
