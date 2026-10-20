// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/result.h"

namespace Service::OLSC {

constexpr Result ResultNullArgument{ErrorModule::OLSC, 100};
constexpr Result ResultNotInitialized{ErrorModule::OLSC, 101};

// [Nextendo] ACNH 3.0.3 (0x5b7bf0) reads "no cache" in the 2179-1200..1399 range of
// GetSaveDataBackupInfoCache and GIVES UP on any other code: only this one may come out.
constexpr Result ResultSaveDataBackupInfoCacheNotFound{ErrorModule::OLSC, 1200};

// [Nextendo] Failures of the asynchronous operations: the game shows them through nn::err
// (2179-09xx), without special handling (only 2124-0200..0269 and 2179-0010 have one).
constexpr Result ResultNextendoUnavailable{ErrorModule::OLSC, 900};
constexpr Result ResultNextendoNetworkError{ErrorModule::OLSC, 901};
constexpr Result ResultNextendoServerRefused{ErrorModule::OLSC, 902};
constexpr Result ResultNextendoNoLocalSave{ErrorModule::OLSC, 903};
constexpr Result ResultNextendoInvalidIsland{ErrorModule::OLSC, 904};
constexpr Result ResultNextendoNotDone{ErrorModule::OLSC, 905};

} // namespace Service::OLSC
