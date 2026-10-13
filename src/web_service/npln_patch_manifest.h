// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace WebService::NplnPatchCatalog {

struct Guard {
    std::uint32_t offset;
    std::vector<std::uint8_t> original;
    std::vector<std::uint8_t> patched;
};

struct Patch {
    std::string id;
    std::uint64_t title_id;
    std::string build_id;
    std::string module;
    std::uint64_t version;
    bool enabled;
    std::vector<std::uint8_t> ips;
    std::vector<Guard> guards;
};

struct Manifest {
    std::uint64_t revision;
    std::int64_t expires;
    std::vector<Patch> patches;
};

Manifest VerifyManifest(const std::string& envelope, const std::string& public_key,
                        std::int64_t now, bool allow_expired = false);
bool CheckGuards(const Patch& patch, std::span<const std::uint8_t> image);

} // namespace WebService::NplnPatchCatalog
