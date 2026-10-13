// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

#include <httplib.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "web_service/npln_patch_catalog.h"
#include "web_service/npln_patch_manifest.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace WebService::NplnPatchCatalog {
namespace {

constexpr std::string_view PublicKey = "LAI+Pi+GZtj3p4DpNM5ORBraw1cbZCVSMFgUmAVWNeA=";
constexpr std::size_t MaxDocument = 1024 * 1024;

struct State {
    std::mutex mutex;
    std::once_flag initialized;
    std::optional<Manifest> manifest;
    std::chrono::steady_clock::time_point next_refresh{};
    bool downloading{};
    std::jthread worker;
};

State& Cache() {
    static State state;
    return state;
}

std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::filesystem::path CachePath() {
    return Common::FS::GetCitronPath(Common::FS::CitronPath::CacheDir) / "nextendo-npln-patches" /
           "catalog-v1.json";
}

void LoadCache() {
    try {
        const auto path = CachePath();
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return;
        }

        std::string envelope(MaxDocument + 1, '\0');
        input.read(envelope.data(), static_cast<std::streamsize>(envelope.size()));
        envelope.resize(static_cast<std::size_t>(input.gcount()));
        const auto manifest = VerifyManifest(envelope, std::string(PublicKey), Now(), true);
        auto& state = Cache();
        std::scoped_lock lock(state.mutex);
        state.manifest = manifest;
        LOG_INFO(WebService, "[Nextendo] NPLN patch cache verified, revision {}",
                 manifest.revision);
    } catch (const std::exception& error) {
        LOG_WARNING(WebService, "[Nextendo] NPLN patch cache rejected: {}", error.what());
    }
}

void SaveCache(const std::string& envelope) {
    const auto path = CachePath();
    const auto temporary = path.parent_path() / "catalog-v1.tmp";
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(envelope.data(), static_cast<std::streamsize>(envelope.size()));
        output.flush();
        if (!output) {
            throw std::runtime_error("Cannot persist verified patch catalog");
        }
    }

#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("Cannot replace verified patch catalog cache");
    }
#else
    std::filesystem::rename(temporary, path);
#endif
}

void Download() {
    try {
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

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        std::string body;
        const auto response =
            client.Get("/service-catalog/v1/npln-patches",
                       httplib::Headers{{"User-Agent", "citron"}, {"Accept", "application/json"}},
                       [&](const char* data, std::size_t length) {
                           if (std::chrono::steady_clock::now() >= deadline ||
                               length > MaxDocument - body.size()) {
                               return false;
                           }
                           body.append(data, length);
                           return true;
                       });

        if (!response || response->status != 200) {
            throw std::runtime_error("Patch catalog download unavailable");
        }

        const auto manifest = VerifyManifest(body, std::string(PublicKey), Now());
        auto& state = Cache();
        std::scoped_lock lock(state.mutex);
        if (state.manifest && manifest.revision < state.manifest->revision) {
            throw std::runtime_error("Patch catalog revision rollback rejected");
        }

        SaveCache(body);
        state.manifest = manifest;
        LOG_INFO(WebService,
                 "[Nextendo] NPLN patch catalog downloaded and verified, revision {}, {} entries",
                 manifest.revision, manifest.patches.size());
    } catch (const std::exception& error) {
        LOG_ERROR(WebService, "[Nextendo] NPLN patch catalog refresh failed: {}", error.what());
    }

    auto& state = Cache();
    std::scoped_lock lock(state.mutex);
    state.downloading = false;
}

const Patch* Select(const State& state, std::uint64_t title_id, std::string_view build_id,
                    std::string_view module) {
    if (!state.manifest || state.manifest->expires <= Now()) {
        return nullptr;
    }

    for (const auto& patch : state.manifest->patches) {
        if (patch.title_id == title_id && patch.build_id == build_id && patch.module == module) {
            return &patch;
        }
    }
    return nullptr;
}

} // namespace

void RefreshAsync() {
    auto& state = Cache();
    std::call_once(state.initialized, LoadCache);
    std::scoped_lock lock(state.mutex);
    const auto now = std::chrono::steady_clock::now();
    if (state.downloading || now < state.next_refresh) {
        return;
    }

    state.downloading = true;
    state.next_refresh = now + std::chrono::minutes(5);
    state.worker = std::jthread(Download);
}

bool HasEntry(std::uint64_t title_id, std::string_view build_id, std::string_view module) {
    auto& state = Cache();
    std::scoped_lock lock(state.mutex);
    return Select(state, title_id, build_id, module) != nullptr;
}

Lookup Find(std::uint64_t title_id, std::string_view build_id, std::string_view module,
            std::span<const std::uint8_t> image) {
    auto& state = Cache();
    std::scoped_lock lock(state.mutex);
    const auto patch = Select(state, title_id, build_id, module);
    if (!patch) {
        return {};
    }

    Lookup result{true, patch->id, patch->version, {}};
    if (!patch->enabled) {
        LOG_WARNING(WebService, "[Nextendo] NPLN patch {} withdrawn by signed catalog", patch->id);
        return result;
    }
    if (!CheckGuards(*patch, image)) {
        LOG_ERROR(WebService,
                  "[Nextendo] NPLN patch {} rejected: original instructions do not match",
                  patch->id);
        return result;
    }

    result.ips = patch->ips;
    return result;
}

} // namespace WebService::NplnPatchCatalog
