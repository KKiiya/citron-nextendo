// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace WebService::NplnPatchCatalog {

struct Lookup {
    bool authoritative{};
    std::string id;
    std::uint64_t version{};
    std::vector<std::uint8_t> ips;
};

// Prepare on a frontend worker before loading the ROM. The loader never does HTTP.
// Start once per process. Wait only on a frontend worker, never in the loader.
void StartTitleIndexCheck();
void WaitForTitleIndex();
bool IsCompatibleTitle(std::uint64_t title_id);
void PrepareForGame(std::uint64_t title_id, const std::string& build_id);
void EndGame();
bool HasEntry(std::uint64_t title_id, std::string_view build_id, std::string_view module);
Lookup Find(std::uint64_t title_id, std::string_view build_id, std::string_view module,
            std::span<const std::uint8_t> image);

} // namespace WebService::NplnPatchCatalog
