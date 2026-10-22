// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/common_types.h"

class QObject;

// Player counts from GET /service-catalog/v1/online-counts (one aggregate document for every
// catalog title, filtered locally). Polled every 30s, and only while the main window is visible and
// at least one installed game is a Nextendo title; nothing is requested otherwise.
namespace Nextendo::OnlineCounts {

// parent should outlive every For() call; it is the window whose visibility gates polling.
// Safe to call more than once.
void Start(QObject* parent);

// Reported by the game list for every installed title that Nextendo supports.
void NoteInstalled(u64 program_id);

// One immediate poll, regardless of installed titles (e.g. the population dialog).
void RefreshNow();

// 0 when unknown or older than 60s.
int For(u64 program_id);

} // namespace Nextendo::OnlineCounts
