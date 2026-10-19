// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "common/common_types.h"

namespace Loader::NextendoAmongUsPatch {

// Among Us makes its game connection over its own DTLS, which only accepts a certificate held by
// Innersloth. Every online path takes the flag from ServerManager.get_UdpUseDtls(); this makes that
// getter return false so the client speaks plain UDP to a community server. The getter is found by its
// code pattern and patched only when exactly one match exists. Takes [NSOHeader][segments].
std::vector<u8> ApplyIfMatch(u64 title_id, std::vector<u8> nso);

} // namespace Loader::NextendoAmongUsPatch
