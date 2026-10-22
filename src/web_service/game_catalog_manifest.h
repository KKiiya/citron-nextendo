// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "common/nextendo_compatible_titles.h"

namespace WebService::GameCatalog {

// Version policies this client can evaluate. An entry requiring anything else is kept, but blocked.
inline constexpr std::string_view CapabilityExact = "versions.exact.v1";
inline constexpr std::string_view CapabilityAny = "versions.any.v1";

// Verifies a signed game_catalog envelope (catalog-key-1, Ed25519 over the exact payload bytes)
// and turns it into requirements. Throws std::runtime_error when the document must not be used.
Nextendo::CompatibleTitles::CatalogRequirements VerifyCatalog(const std::string& envelope,
                                                              const std::string& public_key,
                                                              std::int64_t now,
                                                              bool allow_expired = false);

} // namespace WebService::GameCatalog
