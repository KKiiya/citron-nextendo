// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/common_types.h"

// [Nextendo] Island backup of Animal Crossing: New Horizons, done by the GAME itself (title
// screen, minus button, island backup settings) through olsc:u, like on a Switch and like
// Ryujinx-Nextendo 1.9.9. Nintendo's rule (ACNH 3.0.3, olsc 22.5.0): ONE slot per account, the
// last island sent replaces the previous one, restore is allowed for the account holder. The
// emulator adds no window: the game's own menu drives everything.
//
// This header holds the logic without IPC (zip, fingerprint, olsc state, restore), which the
// tests cover, and the cloud operations used by olsc:u and by the frontend after the game.
namespace Service::OLSC::NextendoIsland {

constexpr u64 AcnhTitleId = 0x01006F8002326000ULL;

// Titles whose backup goes to the Nextendo server (Device save, one per account).
constexpr bool IsStrict(u64 title_id) {
    return title_id == AcnhTitleId;
}

// ---- SaveDataBackupInfo (olsc) ----------------------------------------------------------------
// Read in ACNH 3.0.3: GetSaveDataBackupInfoCache (cmd 18) fills a fixed 0x410 buffer and the game
// reads +8 (PosixTime). The list (cmd 25, object cmd 101) returns items of the same size: +0 the id
// the game passes back to DownloadSaveDataBackupAsync (cmd 26), +8 the date, +0x10 the 0x400 bytes
// of custom data the game itself set (SetCustomData, cmd 15) and reads back to show the island.
constexpr size_t BackupInfoSize = 0x410;
constexpr size_t CustomDataSize = 0x400;

void WriteBackupInfo(std::span<u8> dst, u64 id, s64 posix_time, std::span<const u8> custom);

// State of the account's island, as GET /api/save/<title>/meta returns it.
struct CloudIsland {
    bool exists = false;
    u64 rev = 0;
    s64 posix_time = 0;
    std::vector<u8> custom;
};

CloudIsland ParseMeta(std::string_view json);

// Offered for restore only when it carries the game's custom data: without it the game would fail
// the whole list (an island sent by another client).
bool IsRestorable(const CloudIsland& island);

// "2026-10-20T12:34:56Z", "2026-10-20T12:34:56.123+02:00" -> Unix seconds; nullopt if unreadable.
std::optional<s64> ParseIsoTime(std::string_view text);

std::string Base64Encode(std::span<const u8> data);
std::optional<std::vector<u8>> Base64Decode(std::string_view text);

// ---- fingerprint, zip, extraction -------------------------------------------------------------

// Files of the save directory that are the emulator's own, never part of the island.
bool IsEmulatorFile(std::string_view root_file_name);

// Fingerprint of the island files (sorted relative paths, sizes, contents). "" when empty.
std::string Fingerprint(const std::filesystem::path& save_dir);

// Zip of the island files, "/" paths, no directory entries, stored (ACNH .dat files are
// encrypted: compressing gains nothing).
std::vector<u8> ZipDirectory(const std::filesystem::path& save_dir);

// Cap of an extracted island (8 villagers: 21.35 MB; the server refuses above 64 MiB).
constexpr u64 MaxExtractedBytes = 64ULL << 20;

// Extracts an island into a NEW directory. Refuses any absolute path, any path leaving the
// directory, any island without main.dat, any content above MaxExtractedBytes, encrypted entries,
// unknown compression methods and mismatching sizes or CRCs. Nothing is written when it refuses.
bool ExtractIsland(std::span<const u8> zip, const std::filesystem::path& new_dir);

enum class Outcome {
    Ok,
    Unavailable,
    NetworkError,
    Refused,
    NoLocalSave,
    InvalidIsland,
    Unchanged,
    Deferred,
};

const char* OutcomeName(Outcome outcome);

// Replaces the island of save_dir by the zip's. This machine's island is first kept in
// backups_dir (last 5 "<prefix>-<date>.zip"); an invalid zip touches nothing, a failure halfway
// puts the previous island back. The emulator's own files (save size, extra data) stay.
Outcome ReplaceIsland(const std::filesystem::path& save_dir, std::span<const u8> zip,
                      const std::filesystem::path& backups_dir, const std::string& prefix);

// ---- olsc settings, custom data and cache, per (title, profile) ------------------------------

struct OlscEntry {
    u8 setting = 0; // 1 = enabled (ACNH only compares with 1), 2 = disabled, 0 = not set
    std::string custom; // base64 of the game's SetCustomData
    bool has_cache = false;
    u64 cache_id = 0;
    s64 cache_time = 0;
    std::string cache_custom;
    std::string last_uploaded_hash; // island sent or restored last (no useless resend)
    bool pending_upload = false;    // after-game upload not done yet (resumed on next start)
    std::string pending_save_root;
};

// State file directory (the emulator's config directory by default; the tests change it).
void SetStoreDirectory(const std::filesystem::path& dir);

// Key of a (title, profile): "01006f8002326000:<uid hex>".
std::string Key(u64 title_id, std::string_view uid_hex);

OlscEntry Get(const std::string& key);
void Update(const std::string& key, const std::function<void(OlscEntry&)>& change);
std::vector<std::pair<std::string, OlscEntry>> AllPending();

// The profile the game last used with olsc for this title (after-game upload).
void NoteUser(u64 title_id, const std::string& key);
std::string LastUserKey(u64 title_id);

// ---- cloud (Nextendo server) ------------------------------------------------------------------

// Linked Nextendo account with a token (and a build with the web service).
bool Usable();

// Identifier of this installation ("citron:3fa2c1d0"), kept in the island's metadata.
std::string DeviceId();

// The game's Device save directory on the host, or empty.
std::filesystem::path FindDeviceSaveDir(u64 title_id);

std::pair<Outcome, CloudIsland> GetMeta(u64 title_id);

// Puts the cache (GetSaveDataBackupInfoCache) in line with the server.
void StoreCache(const std::string& key, const CloudIsland& island);

// Sends the island of save_dir with the profile's game data. One operation at a time: an older
// upload cannot finish after a newer one. only_if_changed: the after-game upload does not resend
// an unchanged island and never runs while the game runs.
Outcome Upload(u64 title_id, const std::filesystem::path& save_dir, const std::string& key,
               bool only_if_changed);

// Restore: the account's island replaces save_dir's (see ReplaceIsland).
Outcome Download(u64 title_id, const std::filesystem::path& save_dir, const std::string& key);

// Set by the frontend: the title being emulated (0 = none). The after-game upload waits for the
// game to end (it writes its save; the upload follows).
void SetRunningTitle(u64 title_id);
bool IsGameRunning(u64 title_id);

// Like the Switch's system service, the backup happens outside the game. The upload is first
// marked pending on disk: closing the emulator before it ends leaves it pending, and it starts
// again on the next launch (RunPending).
void MarkPending(const std::string& key, const std::filesystem::path& save_dir);
Outcome UploadPending(u64 title_id, const std::filesystem::path& save_dir, const std::string& key);

// On emulator start: sends the uploads left pending, on a background thread.
void RunPendingInBackground();

// After the game: when the player enabled backup in the game (setting 1), marks the upload pending
// and, when start is true, sends it on a background thread. Never shows anything.
void AfterGame(u64 title_id, bool start);

// Waits at most timeout_ms for the background uploads to end (window close).
void WaitIdle(int timeout_ms);

} // namespace Service::OLSC::NextendoIsland
