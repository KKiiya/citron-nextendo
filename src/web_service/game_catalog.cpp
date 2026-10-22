// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>

#include <httplib.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/nextendo_compatible_titles.h"
#include "web_service/game_catalog.h"
#include "web_service/game_catalog_manifest.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace WebService::GameCatalog {
namespace {

// Public half of catalog-key-1, the key that signs service-catalog/v1/game-catalog.
constexpr std::string_view PublicKey = "xI+atR6VesrCH89ZDZD4t3WQLcYOFEhk5F0HLMzgWk4=";
constexpr std::size_t MaxDocument = 1024 * 1024;

struct Cached {
    std::uint64_t revision{};
    std::string etag;
};

std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::filesystem::path CacheDirectory() {
    return Common::FS::GetCitronPath(Common::FS::CitronPath::CacheDir) / "nextendo-game-catalog";
}

std::string ReadFile(const std::filesystem::path& path, std::size_t max) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::string data(max + 1, '\0');
    input.read(data.data(), static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<std::size_t>(input.gcount()));
    return data.size() > max ? std::string{} : data;
}

void WriteFile(const std::filesystem::path& path, const std::string& data) {
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(data.data(), static_cast<std::streamsize>(data.size()));
        output.flush();
        if (!output) {
            throw std::runtime_error("Cannot write the game catalog cache");
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("Cannot replace the game catalog cache");
    }
#else
    std::filesystem::rename(temporary, path);
#endif
}

// Every cached envelope is verified again; an altered or unsigned file is never used.
Cached LoadCache() {
    Cached cached;
    try {
        const auto envelope = ReadFile(CacheDirectory() / "game-catalog.json", MaxDocument);
        if (envelope.empty()) {
            return cached;
        }
        auto requirements = VerifyCatalog(envelope, std::string(PublicKey), Now(), true);
        cached.revision = requirements.revision;
        cached.etag = ReadFile(CacheDirectory() / "game-catalog.etag", 128);
        const bool expired = requirements.expires_at <= Now();
        Nextendo::CompatibleTitles::PublishCatalog(std::move(requirements));
        LOG_INFO(WebService, "[Nextendo] Game catalog cache verified, revision {}{}",
                 cached.revision, expired ? " (expired: built-in table until refreshed)" : "");
    } catch (const std::exception& error) {
        LOG_WARNING(WebService, "[Nextendo] Game catalog cache rejected: {}", error.what());
        cached = {};
    }
    return cached;
}

// Returns true when a verified catalog (new or unchanged) was confirmed by the server.
bool Refresh(Cached& cached) {
    httplib::Client client("https://nextendo.network");
    client.set_connection_timeout(5);
    client.set_read_timeout(5);
    client.set_write_timeout(5);
    client.set_follow_location(false);
    client.enable_server_certificate_verification(true);
#ifdef __linux__
    for (const char* path : {"/etc/ssl/certs/ca-certificates.crt",
                             "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/cert.pem"}) {
        if (std::filesystem::exists(path)) {
            client.set_ca_cert_path(path);
            break;
        }
    }
#endif
    // A 304 never extends the signed validity, so only revalidate a copy that is still current.
    const bool current = Nextendo::CompatibleTitles::ActiveCatalogRevision() != 0;
    httplib::Headers headers{{"User-Agent", "citron"}, {"Accept", "application/json"}};
    if (current && !cached.etag.empty()) {
        headers.emplace("If-None-Match", cached.etag);
    }

    std::string body;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto response = client.Get("/service-catalog/v1/game-catalog", headers,
                                     [&](const char* data, std::size_t length) {
                                         if (std::chrono::steady_clock::now() >= deadline ||
                                             length > MaxDocument - body.size()) {
                                             return false;
                                         }
                                         body.append(data, length);
                                         return true;
                                     });
    if (response && response->status == 304 && current && !cached.etag.empty() &&
        response->get_header_value("ETag") == cached.etag) {
        return true;
    }
    if (!response || response->status != 200) {
        throw std::runtime_error("Game catalog download unavailable");
    }

    auto requirements = VerifyCatalog(body, std::string(PublicKey), Now());
    if (requirements.revision < cached.revision) {
        throw std::runtime_error("Game catalog revision rollback rejected");
    }
    const auto etag = response->get_header_value("ETag");
    try {
        WriteFile(CacheDirectory() / "game-catalog.json", body);
        WriteFile(CacheDirectory() / "game-catalog.etag", etag);
    } catch (const std::exception& error) {
        LOG_WARNING(WebService, "[Nextendo] Game catalog cache write failed: {}", error.what());
    }
    cached.revision = requirements.revision;
    cached.etag = etag;
    const auto titles = requirements.table.size();
    Nextendo::CompatibleTitles::PublishCatalog(std::move(requirements));
    LOG_INFO(WebService, "[Nextendo] Game catalog verified, revision {}, {} titles",
             cached.revision, titles);
    return true;
}

void Run() {
    Cached cached = LoadCache();
    constexpr std::array<int, 4> retry_minutes{1, 5, 15, 60};
    std::size_t failures = 0;
    std::mt19937 random{std::random_device{}()};
    while (true) {
        std::chrono::seconds wait;
        try {
            Refresh(cached);
            failures = 0;
            // About six hours, spread so clients don't all ask at the same moment.
            wait = std::chrono::seconds(6 * 3600 + std::uniform_int_distribution<int>(-1800, 1800)(random));
        } catch (const std::exception& error) {
            LOG_WARNING(WebService, "[Nextendo] Game catalog refresh failed; retaining verified copy: {}",
                        error.what());
            wait = std::chrono::minutes(retry_minutes[std::min(failures, retry_minutes.size() - 1)]);
            ++failures;
        }
        std::this_thread::sleep_for(wait);
    }
}

} // namespace

void Start() {
    static std::once_flag started;
    std::call_once(started, [] { std::thread(Run).detach(); });
}

} // namespace WebService::GameCatalog
