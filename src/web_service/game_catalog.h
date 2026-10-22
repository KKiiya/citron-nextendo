// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace WebService::GameCatalog {

// Loads the last verified catalog from the cache, then keeps it fresh in the background: about
// every six hours with an ETag, retrying after 1, 5, 15 and 60 minutes on failure. Every accepted
// document is published to Nextendo::CompatibleTitles, so readers never wait on the network.
// Safe to call more than once.
void Start();

} // namespace WebService::GameCatalog
