// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// [Nextendo] Island backup of Animal Crossing: New Horizons (olsc:u), logic without IPC nor
// network. Same cases as Ryujinx-Nextendo's NextendoIslandSyncTests (1.9.9).

#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/olsc/nextendo_island.h"

namespace {

namespace fs = std::filesystem;
using namespace Service::OLSC::NextendoIsland;

u32 TestCrc(const std::vector<u8>& data) {
    u32 crc = 0xFFFFFFFFU;
    for (const u8 b : data) {
        crc ^= b;
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

void Put16(std::vector<u8>& o, u32 v) {
    o.push_back(static_cast<u8>(v));
    o.push_back(static_cast<u8>(v >> 8));
}

void Put32(std::vector<u8>& o, u32 v) {
    Put16(o, v & 0xFFFF);
    Put16(o, v >> 16);
}

struct TestEntry {
    std::string name;
    std::vector<u8> data;
    bool deflate = false; // raw deflate made of one stored block
    u16 flags = 0;
};

// A zip as another client could send it: stored or deflated entries, any name.
std::vector<u8> MakeZip(const std::vector<TestEntry>& entries) {
    std::vector<u8> out, central;
    for (const auto& e : entries) {
        std::vector<u8> payload;
        if (e.deflate) {
            payload.push_back(0x01); // BFINAL, stored block
            Put16(payload, static_cast<u32>(e.data.size()));
            Put16(payload, static_cast<u32>(~e.data.size() & 0xFFFF));
            payload.insert(payload.end(), e.data.begin(), e.data.end());
        } else {
            payload = e.data;
        }
        const u32 offset = static_cast<u32>(out.size());
        const u32 crc = TestCrc(e.data);
        const u16 method = e.deflate ? 8 : 0;
        Put32(out, 0x04034b50);
        Put16(out, 20);
        Put16(out, e.flags);
        Put16(out, method);
        Put16(out, 0);
        Put16(out, 0x21);
        Put32(out, crc);
        Put32(out, static_cast<u32>(payload.size()));
        Put32(out, static_cast<u32>(e.data.size()));
        Put16(out, static_cast<u32>(e.name.size()));
        Put16(out, 0);
        out.insert(out.end(), e.name.begin(), e.name.end());
        out.insert(out.end(), payload.begin(), payload.end());

        Put32(central, 0x02014b50);
        Put16(central, 20);
        Put16(central, 20);
        Put16(central, e.flags);
        Put16(central, method);
        Put16(central, 0);
        Put16(central, 0x21);
        Put32(central, crc);
        Put32(central, static_cast<u32>(payload.size()));
        Put32(central, static_cast<u32>(e.data.size()));
        Put16(central, static_cast<u32>(e.name.size()));
        Put16(central, 0);
        Put16(central, 0);
        Put16(central, 0);
        Put16(central, 0);
        Put32(central, 0);
        Put32(central, offset);
        central.insert(central.end(), e.name.begin(), e.name.end());
    }
    const u32 cd_offset = static_cast<u32>(out.size());
    out.insert(out.end(), central.begin(), central.end());
    Put32(out, 0x06054b50);
    Put16(out, 0);
    Put16(out, 0);
    Put16(out, static_cast<u32>(entries.size()));
    Put16(out, static_cast<u32>(entries.size()));
    Put32(out, static_cast<u32>(central.size()));
    Put32(out, cd_offset);
    Put16(out, 0);
    return out;
}

fs::path TempDir(const char* tag) {
    std::random_device rd;
    return fs::temp_directory_path() / (std::string(tag) + std::to_string(rd()));
}

void WriteBytes(const fs::path& p, const std::vector<u8>& d) {
    fs::create_directories(p.parent_path());
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
}

std::vector<u8> ReadBytes(const fs::path& p) {
    std::ifstream i(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(i), std::istreambuf_iterator<char>()};
}

// A Citron Device save: island files plus the emulator's own files.
fs::path MakeSave(u8 content) {
    const fs::path root = TempDir("nxsave_");
    WriteBytes(root / "main.dat", {content, content});
    WriteBytes(root / "Villager0" / "personal.dat", {content});
    WriteBytes(root / ".citron_save_size", {1, 2, 3, 4});
    WriteBytes(root / "ExtraData0", {5, 6});
    return root;
}

} // namespace

TEST_CASE("NextendoIsland: only ACNH is strict", "[olsc]") {
    REQUIRE(IsStrict(0x01006F8002326000ULL));
    REQUIRE_FALSE(IsStrict(0x0100C2500FC20000ULL));
    REQUIRE_FALSE(IsStrict(0));
}

TEST_CASE("NextendoIsland: fingerprint follows the island, not the emulator files", "[olsc]") {
    const fs::path a = MakeSave(1);
    const std::string before = Fingerprint(a);
    REQUIRE(before.size() == 16);
    WriteBytes(a / ".citron_save_size", {9, 9, 9, 9});
    REQUIRE(Fingerprint(a) == before);
    WriteBytes(a / "Villager0" / "personal.dat", {2});
    REQUIRE(Fingerprint(a) != before);
    REQUIRE(Fingerprint(TempDir("nxnone_")).empty());
    fs::remove_all(a);
}

TEST_CASE("NextendoIsland: zip round trip with forward slashes and no emulator file", "[olsc]") {
    const fs::path a = MakeSave(7);
    const std::vector<u8> zip = ZipDirectory(a);
    REQUIRE(!zip.empty());
    const std::string text(zip.begin(), zip.end());
    REQUIRE(text.find("Villager0/personal.dat") != std::string::npos);
    REQUIRE(text.find("Villager0\\") == std::string::npos);
    REQUIRE(text.find(".citron_save_size") == std::string::npos);
    REQUIRE(text.find("ExtraData0") == std::string::npos);

    const fs::path out = TempDir("nxout_");
    REQUIRE(ExtractIsland(zip, out));
    REQUIRE(ReadBytes(out / "main.dat") == std::vector<u8>{7, 7});
    REQUIRE(ReadBytes(out / "Villager0" / "personal.dat") == std::vector<u8>{7});
    REQUIRE(Fingerprint(out) == Fingerprint(a));
    fs::remove_all(a);
    fs::remove_all(out);
}

TEST_CASE("NextendoIsland: deflated islands from other clients are read", "[olsc]") {
    const std::vector<u8> data(3000, 0x5A);
    const auto zip = MakeZip({{"main.dat", data, true}, {"Villager0\\personal.dat", {1}, true}});
    const fs::path out = TempDir("nxdefl_");
    REQUIRE(ExtractIsland(zip, out));
    REQUIRE(ReadBytes(out / "main.dat") == data);
    REQUIRE(ReadBytes(out / "Villager0" / "personal.dat") == std::vector<u8>{1});
    fs::remove_all(out);
}

TEST_CASE("NextendoIsland: trapped zips are refused and nothing is written", "[olsc]") {
    const std::vector<u8> one{1};
    std::vector<std::vector<u8>> bad{
        MakeZip({{"main.dat", one}, {"../evil.dat", one}}),
        MakeZip({{"main.dat", one}, {"a/../../evil.dat", one}}),
        MakeZip({{"main.dat", one}, {"..\\evil.dat", one}}),
        MakeZip({{"main.dat", one}, {"C:/evil.dat", one}}),
        MakeZip({{"/main.dat", one}}),
        MakeZip({{"main.dat", one}, {"a//b.dat", one}}),
        MakeZip({{"main.dat", one}, {"Main.DAT", one}}),
        MakeZip({{"main.dat", one}, {".citron_save_size", one}}),
        MakeZip({{"Villager0/personal.dat", one}}),
        MakeZip({{"main.dat", one, false, 0x1}}), // encrypted
        {'P', 'K', 3, 4, 0, 0},
        {},
    };
    // Declared sizes above the cap (a zip bomb) are refused before anything is inflated.
    {
        auto big = MakeZip({{"main.dat", one}});
        const u32 huge = static_cast<u32>(MaxExtractedBytes + 1);
        const auto cd = static_cast<size_t>(big[big.size() - 6]) |
                        (static_cast<size_t>(big[big.size() - 5]) << 8);
        std::memcpy(big.data() + cd + 24, &huge, 4);
        bad.push_back(big);
    }
    // A wrong CRC is refused.
    {
        auto crc = MakeZip({{"main.dat", {1, 2, 3}}});
        crc[14] ^= 0xFF;
        const auto cd = static_cast<size_t>(crc[crc.size() - 6]) |
                        (static_cast<size_t>(crc[crc.size() - 5]) << 8);
        crc[cd + 16] ^= 0xFF;
        bad.push_back(crc);
    }
    for (const auto& zip : bad) {
        const fs::path target = TempDir("nxbad_");
        REQUIRE_FALSE(ExtractIsland(zip, target));
        REQUIRE_FALSE(fs::exists(target));
        REQUIRE_FALSE(fs::exists(target.parent_path() / "evil.dat"));
    }
}

TEST_CASE("NextendoIsland: SaveDataBackupInfo layout", "[olsc]") {
    std::vector<u8> info(BackupInfoSize, 0xAA);
    const std::vector<u8> custom{'i', 'l', 'e', 0};
    WriteBackupInfo(info, 0x1122334455667788ULL, 1760000000, custom);
    u64 id = 0;
    s64 posix = 0;
    std::memcpy(&id, info.data(), 8);
    std::memcpy(&posix, info.data() + 8, 8);
    REQUIRE(id == 0x1122334455667788ULL);
    REQUIRE(posix == 1760000000);
    REQUIRE(std::memcmp(info.data() + 0x10, custom.data(), custom.size()) == 0);
    for (size_t i = 0x10 + custom.size(); i < BackupInfoSize; ++i) {
        REQUIRE(info[i] == 0);
    }
}

TEST_CASE("NextendoIsland: server meta is parsed", "[olsc]") {
    const std::string custom = Base64Encode(std::vector<u8>{'i', 'l', 'e', 0});
    const CloudIsland c = ParseMeta(R"({"exists":true,"rev":7,"updatedAt":"2026-10-20T10:00:00Z","custom":")" +
                                    custom + "\"}");
    REQUIRE(c.exists);
    REQUIRE(c.rev == 7);
    REQUIRE(c.posix_time == 1792490400);
    REQUIRE(IsRestorable(c));
    REQUIRE(ParseIsoTime("2026-10-20T12:00:00.250+02:00") == 1792490400);
    REQUIRE_FALSE(ParseIsoTime("hier").has_value());

    // Without the game's data the game would fail the whole list: not offered.
    REQUIRE_FALSE(IsRestorable(ParseMeta(R"({"exists":true,"rev":3})")));
    REQUIRE_FALSE(IsRestorable(ParseMeta(R"({"exists":false,"rev":0})")));
    REQUIRE_FALSE(IsRestorable(ParseMeta(R"({"exists":true,"rev":3,"custom":"pas du base64"})")));
    REQUIRE_FALSE(IsRestorable(ParseMeta("pas du json")));
}

TEST_CASE("NextendoIsland: base64 round trip", "[olsc]") {
    for (size_t n = 0; n < 8; ++n) {
        std::vector<u8> data(n);
        for (size_t i = 0; i < n; ++i) {
            data[i] = static_cast<u8>(0xF0 + i);
        }
        REQUIRE(Base64Decode(Base64Encode(data)) == data);
    }
    REQUIRE(Base64Encode(std::vector<u8>{'M', 'a'}) == "TWE=");
    REQUIRE_FALSE(Base64Decode("TW=E").has_value());
}

TEST_CASE("NextendoIsland: restore swaps the island and keeps a copy", "[olsc]") {
    const fs::path save = MakeSave(1);
    const fs::path backups = TempDir("nxbak_");
    const fs::path cloud = MakeSave(2);
    WriteBytes(cloud / "Villager1" / "personal.dat", {2});
    const std::vector<u8> zip = ZipDirectory(cloud);
    WriteBytes(save / "Villager5" / "personal.dat", {1}); // a villager who left in the cloud

    REQUIRE(ReplaceIsland(save, zip, backups, "01006f8002326000") == Outcome::Ok);
    REQUIRE(Fingerprint(save) == Fingerprint(cloud));
    REQUIRE_FALSE(fs::exists(save / "Villager5"));
    REQUIRE(ReadBytes(save / ".citron_save_size") == std::vector<u8>{1, 2, 3, 4});
    REQUIRE(ReadBytes(save / "ExtraData0") == std::vector<u8>{5, 6});
    REQUIRE_FALSE(fs::exists(fs::path(save.string() + ".nx_restore_new")));
    REQUIRE_FALSE(fs::exists(fs::path(save.string() + ".nx_restore_old")));

    size_t copies = 0;
    for (const auto& e : fs::directory_iterator(backups)) {
        ++copies;
        const fs::path check = TempDir("nxchk_");
        REQUIRE(ExtractIsland(ReadBytes(e.path()), check));
        REQUIRE(ReadBytes(check / "Villager5" / "personal.dat") == std::vector<u8>{1});
        fs::remove_all(check);
    }
    REQUIRE(copies == 1);

    // Only the last 5 copies are kept.
    for (int i = 0; i < 6; ++i) {
        REQUIRE(ReplaceIsland(save, zip, backups, "01006f8002326000") == Outcome::Ok);
    }
    copies = 0;
    for ([[maybe_unused]] const auto& e : fs::directory_iterator(backups)) {
        ++copies;
    }
    REQUIRE(copies == 5);

    fs::remove_all(save);
    fs::remove_all(cloud);
    fs::remove_all(backups);
}

TEST_CASE("NextendoIsland: an invalid download keeps the local island", "[olsc]") {
    const fs::path save = MakeSave(3);
    const fs::path backups = TempDir("nxbak_");
    const std::string before = Fingerprint(save);
    const auto bad = MakeZip({{"main.dat", {1}}, {"../evil.dat", {1}}});
    REQUIRE(ReplaceIsland(save, bad, backups, "01006f8002326000") == Outcome::InvalidIsland);
    REQUIRE(Fingerprint(save) == before);
    REQUIRE_FALSE(fs::exists(backups));
    REQUIRE(ReplaceIsland(TempDir("nxmissing_"), ZipDirectory(save), backups,
                          "01006f8002326000") == Outcome::NoLocalSave);
    fs::remove_all(save);
}

TEST_CASE("NextendoIsland: enable flow replay and pending upload", "[olsc]") {
    const fs::path dir = TempDir("nxolsc_");
    fs::create_directories(dir);
    SetStoreDirectory(dir);

    const u64 acnh = 0x01006F8002326000ULL;
    const std::string user = Key(acnh, "DC6F8D4E825A4D9C858F6DA4178BE3D0");
    const std::string other = Key(acnh, "00000000000000010000000000000000");
    REQUIRE(user == "01006f8002326000:dc6f8d4e825a4d9c858f6da4178be3d0");
    REQUIRE(Get(user).setting == 0); // never set: "Backups - Not Set"

    // What the game does (10, 14, 15, 14), replayed from the first console run.
    Update(other, [](OlscEntry& e) { e.setting = 2; });
    Update(user, [](OlscEntry& e) { e.setting = 2; });
    std::vector<u8> custom(405, 'i');
    custom.push_back(0);
    const std::string b64 = Base64Encode(custom);
    Update(user, [&b64](OlscEntry& e) { e.custom = b64; });
    Update(user, [](OlscEntry& e) { e.setting = 1; });
    NoteUser(acnh, user);

    REQUIRE(Get(user).setting == 1);
    REQUIRE(Get(other).setting == 2);
    REQUIRE(Base64Decode(Get(user).custom) == custom);
    REQUIRE(LastUserKey(acnh) == user);

    // Island cache from the server (cmd 18), date at +8 in PosixTime.
    CloudIsland island;
    island.exists = true;
    island.rev = 9;
    island.posix_time = 1792490400;
    island.custom = custom;
    StoreCache(user, island);
    REQUIRE(Get(user).has_cache);
    REQUIRE(Get(user).cache_id == 9);
    REQUIRE(Get(user).cache_time == 1792490400);

    // End of game: marked pending before the upload, found again on the next launch.
    MarkPending(user, fs::path("save") / "1b");
    const auto pending = AllPending();
    REQUIRE(pending.size() == 1);
    REQUIRE(pending[0].first == user);
    REQUIRE(fs::path(pending[0].second.pending_save_root) == fs::path("save") / "1b");
    REQUIRE(Get(user).setting == 1); // the mark does not touch the setting

    // Never while the game runs.
    SetRunningTitle(acnh);
    REQUIRE(IsGameRunning(acnh));
    REQUIRE(UploadPending(acnh, fs::path("save") / "1b", user) == Outcome::Deferred);
    REQUIRE(Get(user).pending_upload);
    SetRunningTitle(0);
    REQUIRE_FALSE(IsGameRunning(acnh));

    fs::remove_all(dir);
}
