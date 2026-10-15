// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <tuple>

#include "web_service/npln_patch_manifest.h"

namespace WebService::NplnPatchCatalog {

// Callers supply only manifests that passed signature and instruction validation.
// Catalog freshness controls downloads, not the lifetime of a verified patch.
class PatchState {
public:
    void Remember(const Manifest& manifest) {
        latest_revision = std::max(latest_revision, manifest.revision);

        for (const auto& patch : manifest.patches) {
            const Key key{patch.title_id, patch.build_id, patch.module};
            const auto previous = retained.find(key);
            if (previous == retained.end() || previous->second.revision < manifest.revision ||
                (previous->second.revision == manifest.revision &&
                 (!manifest.etag.empty() || previous->second.etag.empty()))) {
                retained.insert_or_assign(key, StoredPatch{manifest.revision, patch, manifest.etag});
            }
        }
    }

    bool HasTitle(std::uint64_t title_id) const {
        return std::any_of(retained.begin(), retained.end(), [title_id](const auto& item) {
            return std::get<0>(item.first) == title_id;
        });
    }

    std::uint64_t LatestRevision() const {
        return latest_revision;
    }

    const Patch* Cached(std::uint64_t title_id, std::string_view build_id) const {
        const auto found = retained.find(Key{title_id, std::string(build_id), "main"});
        return found == retained.end() ? nullptr : &found->second.patch;
    }

    std::uint64_t Revision(std::uint64_t title_id, std::string_view build_id) const {
        const auto found = retained.find(Key{title_id, std::string(build_id), "main"});
        return found == retained.end() ? 0 : found->second.revision;
    }

    std::string Tag(std::uint64_t title_id, std::string_view build_id) const {
        const auto found = retained.find(Key{title_id, std::string(build_id), "main"});
        return found == retained.end() ? std::string{} : found->second.etag;
    }

    void BeginGame(std::uint64_t title_id) {
        active.clear();
        for (const auto& [key, stored] : retained) {
            if (std::get<0>(key) == title_id) {
                active.emplace(key, stored.patch);
            }
        }
    }

    void EndGame() {
        active.clear();
    }

    const Patch* Find(std::uint64_t title_id, std::string_view build_id,
                      std::string_view module) const {
        const auto found = active.find(Key{title_id, std::string(build_id), std::string(module)});
        return found == active.end() ? nullptr : &found->second;
    }

private:
    using Key = std::tuple<std::uint64_t, std::string, std::string>;

    struct StoredPatch {
        std::uint64_t revision;
        Patch patch;
        std::string etag;
    };

    std::uint64_t latest_revision{};
    std::map<Key, StoredPatch> retained;
    std::map<Key, Patch> active;
};

} // namespace WebService::NplnPatchCatalog
