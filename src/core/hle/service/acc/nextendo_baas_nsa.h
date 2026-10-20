// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

#include "common/common_types.h"

namespace Service::Account {

// [Nextendo] The NSA (NetworkServiceAccountId) the linked account has on a real Switch: its
// baasUserID (16 hex digits), served by the Nextendo site on /api/me ("nsa"). It is the id the
// account's Switch friends know it by (nx-account friendId) and its emulator friends receive
// ("nsa" in /api/friends).
//
// Animal Crossing: New Horizons compares the NSA each player announces (Pia session, message card
// header) with the console's nn::friends list (Friend::GetAccountId): session at 0x1c4ae20 and
// 0x1cbfc40, received cards at 0x1cb6ef0 and 0x1cb8e00 (main 3.0.3). A Switch announces its
// baasUserID and lists its friends by baasUserID; emulators announced the pid and listed friends
// by pid, so a Switch and an emulator never saw each other as friends. In that title, citron now
// announces the baasUserID and lists its friends by their "nsa", like a Switch (and like
// Ryujinx-Nextendo). Unknown baasUserID (older site, unreachable): pid everywhere, as before.
// NEXTENDO_NSA_BAAS=0 turns it off.

constexpr u64 AcnhProgramId = 0x01006F8002326000ULL;

bool TitleUsesBaasNsa(u64 program_id);

// 16 hex digits exactly, non zero, else 0.
u64 ParseBaasNsaHex(std::string_view value);

// The NSA to announce: the baasUserID in the titles that need it when known, else the pid.
u64 ChooseAnnouncedNsa(u64 program_id, u64 pid, u64 baas_nsa);

// The linked account's baasUserID, or 0. Looked up once on the site (3 s at most, retried after a
// minute on failure); once known it never changes for that account in this session.
u64 GetLinkedAccountBaasNsa();

// Set by the account service each time a title asks for its NSA: true while the running title
// announces the baasUserID, so the friend service lists friends by their "nsa" as well.
void SetBaasNsaMode(bool active);
bool IsBaasNsaModeActive();

} // namespace Service::Account
