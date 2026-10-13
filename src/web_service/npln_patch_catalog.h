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

// Network work is always asynchronous. The NSO loader reads verified local state.
void RefreshAsync();
bool HasEntry(std::uint64_t title_id, std::string_view build_id, std::string_view module);
Lookup Find(std::uint64_t title_id, std::string_view build_id, std::string_view module,
            std::span<const std::uint8_t> image);

} // namespace WebService::NplnPatchCatalog
