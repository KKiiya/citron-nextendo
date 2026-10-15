// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string_view>
#include <vector>

#include "common/common_types.h"

namespace Loader::NextendoS3Patches {

// Applies the verified catalog snapshot selected before this game launch. NPLN titles
// have no built-in fallback. CTR/Demonware retains its exact-build built-in patches.
// The loader never performs network requests or changes the selected patch mid-game.
//
// nso must be [NSOHeader][decompressed segment data], the same layout used by
// PatchManager::PatchNSO. Online patches remain independent of user mod patches.
std::vector<u8> ApplyIfMatch(const std::array<u8, 0x20>& build_id, std::vector<u8> nso,
                             std::string_view module_name, u64 title_id);

} // namespace Loader::NextendoS3Patches
