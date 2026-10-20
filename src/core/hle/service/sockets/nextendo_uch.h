// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "common/common_types.h"

// Ultimate Chicken Horse (0100FCF002A58000, 1.13.13.765) online support. Everything here is scoped to that
// one title: other games never take any of these paths.
namespace Service::Sockets::NextendoUch {

inline constexpr u64 TitleId = 0x0100FCF002A58000ULL;

/// Size of the NXU1 admission frame: the "NXU1" tag followed by the 32-byte ticket.
inline constexpr std::size_t FrameSize = 36;

bool IsTitle(u64 program_id);

/// True for the brainCloud RPC host and the three regional allocation hosts the game contacts (the same
/// names Prelude writes into the hosts file). Case-insensitive, exact match.
bool IsServiceHost(std::string_view host);

/// UCH opens its UNET transport socket with the selector 0x10000000 and protocol IP, which has no socket
/// type of its own. It is a datagram socket: true for exactly that selector, for UCH only.
bool IsUdpSelector(u64 program_id, u32 requested_type);

/// Stores the NXU1 extension of a login response body, if it carries a valid one. The ticket is kept in
/// memory only (never logged), for at most 35 seconds, and is replaced by the next valid login response.
/// Returns true when a ticket was stored.
bool ObserveLoginBody(std::string_view json_body);

enum class BootstrapResult {
    /// Not UCH, or not the game's UNET connect request: send the packet as usual.
    NotApplicable,
    /// The connect request cannot be admitted (no valid ticket, or a destination other than the relay).
    Refused,
    /// Send `frame` first, then the game's own packet.
    SendFrame,
};

/// Decides what to do with a datagram the game sends. The ticket frame is only ever produced for the
/// connect request to the relay endpoint announced in the login response.
BootstrapResult Bootstrap(u64 program_id, std::span<const u8> packet, const std::array<u8, 4>& ip,
                          u16 port, std::array<u8, FrameSize>& frame);

/// Forgets any stored ticket (process end, tests).
void ClearTicket();

/// Watches the plaintext of a TLS connection to a UCH service host and hands every complete HTTP response
/// body to ObserveLoginBody. It only observes: it never changes what the game reads, never logs a header
/// or body, and gives up silently on anything it cannot parse.
class ResponseObserver {
public:
    void Feed(std::span<const u8> bytes);

private:
    std::vector<u8> pending;
};

} // namespace Service::Sockets::NextendoUch
