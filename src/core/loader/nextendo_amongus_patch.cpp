// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstring>

#include "common/logging.h"
#include "core/loader/nextendo_amongus_patch.h"

namespace Loader::NextendoAmongUsPatch {

namespace {

constexpr u64 kTitleId = 0x0100B0C013912000ULL;
constexpr std::size_t kNsoHeaderSize = 0x100;
constexpr std::size_t kGetterSize = 16;

// ServerManager.get_UdpUseDtls, i.e. `return this.CurrentUdpServer.UseDtls`:
//   ldr x8, [x0, #0x30] ; cbz x8, <any> ; ldrb w0, [x8, #0x22] ; ret
// For the cbz only its type (64-bit, last byte 0xB4) and register (x8, low 5 bits of its first
// byte) are fixed: the jump distance may differ between builds.
constexpr std::array<u8, 4> kHead{{0x08, 0x18, 0x40, 0xF9}};
constexpr std::array<u8, 8> kTail{{0x00, 0x89, 0x40, 0x39, 0xC0, 0x03, 0x5F, 0xD6}};
constexpr std::array<u8, 8> kPatch{{0x00, 0x00, 0x80, 0x52, 0xC0, 0x03, 0x5F, 0xD6}}; // mov w0,#0; ret

bool IsGetter(const u8* p) {
    return std::memcmp(p, kHead.data(), kHead.size()) == 0 && (p[4] & 0x1F) == 0x08 &&
           p[7] == 0xB4 && std::memcmp(p + 8, kTail.data(), kTail.size()) == 0;
}

} // namespace

std::vector<u8> ApplyIfMatch(u64 title_id, std::vector<u8> nso) {
    if (title_id != kTitleId) {
        return nso;
    }

    std::size_t found = 0;
    // The program image starts right after the 0x100-byte header and ARM64 code is 4-byte aligned.
    for (std::size_t offset = kNsoHeaderSize; offset + kGetterSize <= nso.size(); offset += 4) {
        if (!IsGetter(nso.data() + offset)) {
            continue;
        }
        if (found != 0) {
            LOG_ERROR(Loader, "[Nextendo] Among Us: more than one UseDtls getter pattern, DTLS "
                              "patch skipped");
            return nso;
        }
        found = offset;
    }

    if (found == 0) {
        LOG_ERROR(Loader, "[Nextendo] Among Us: UseDtls getter not found, DTLS patch skipped "
                          "(unsupported game version)");
        return nso;
    }

    std::memcpy(nso.data() + found, kPatch.data(), kPatch.size());
    LOG_INFO(Loader, "[Nextendo] Among Us: plain-UDP patch applied at 0x{:X}",
             found - kNsoHeaderSize);
    return nso;
}

} // namespace Loader::NextendoAmongUsPatch
