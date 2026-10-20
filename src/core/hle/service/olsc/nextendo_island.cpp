// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// [Nextendo] Island backup of Animal Crossing: New Horizons: the logic without IPC nor network
// (zip, fingerprint, olsc state, restore). Port of NextendoIslandSync.cs (Ryujinx-Nextendo 1.9.9).

#include "core/hle/service/olsc/nextendo_island.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <system_error>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <zlib.h>

#include "common/fs/path_util.h"

namespace Service::OLSC::NextendoIsland {

namespace fs = std::filesystem;

namespace {

void PutU16(std::vector<u8>& out, u16 v) {
    out.push_back(static_cast<u8>(v));
    out.push_back(static_cast<u8>(v >> 8));
}

void PutU32(std::vector<u8>& out, u32 v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<u8>(v >> (8 * i)));
    }
}

u16 GetU16(std::span<const u8> d, size_t at) {
    return static_cast<u16>(d[at] | (d[at + 1] << 8));
}

u32 GetU32(std::span<const u8> d, size_t at) {
    return static_cast<u32>(d[at]) | (static_cast<u32>(d[at + 1]) << 8) |
           (static_cast<u32>(d[at + 2]) << 16) | (static_cast<u32>(d[at + 3]) << 24);
}

// Island files of a save directory: regular files, sorted by "/" relative path, without the
// emulator's own files at the root. Symbolic links are never followed nor listed.
std::vector<std::pair<std::string, fs::path>> IslandFiles(const fs::path& dir) {
    std::vector<std::pair<std::string, fs::path>> files;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) {
        return files;
    }
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::none, ec), end;
         !ec && it != end; it.increment(ec)) {
        const fs::directory_entry& e = *it;
        if (e.is_symlink(ec)) {
            if (e.is_directory(ec)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        const fs::path rel = fs::relative(e.path(), dir, ec);
        if (ec) {
            break;
        }
        const std::string rel_str = rel.generic_string();
        if (it.depth() == 0 && IsEmulatorFile(e.path().filename().string())) {
            if (e.is_directory(ec)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (e.is_regular_file(ec)) {
            files.emplace_back(rel_str, e.path());
        }
    }
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return files;
}

bool ReadFile(const fs::path& path, std::vector<u8>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::streamoff>(in.tellg());
    if (size < 0) {
        return false;
    }
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(out.data()), size)) {
        return false;
    }
    return true;
}

bool WriteFile(const fs::path& path, std::span<const u8> data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

// Entry names accepted in an island: relative, "/" separated, no ".", "..", empty part, drive,
// control or Windows-reserved character.
bool IsSafeEntryName(const std::string& name) {
    if (name.empty() || name.front() == '/' || name.size() > 512) {
        return false;
    }
    size_t start = 0;
    while (start <= name.size()) {
        const size_t slash = name.find('/', start);
        const std::string part =
            name.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' ||
            part.back() == ' ') {
            return false;
        }
        for (const char c : part) {
            const auto u = static_cast<unsigned char>(c);
            if (u < 0x20 || c == ':' || c == '\\' || c == '<' || c == '>' || c == '"' ||
                c == '|' || c == '?' || c == '*') {
                return false;
            }
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

void RemoveAll(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

struct ZipEntry {
    std::string name;
    u16 method = 0;
    u32 crc = 0;
    u32 compressed = 0;
    u32 uncompressed = 0;
    size_t data_offset = 0;
};

// Reads and checks the central directory. nullopt on anything unexpected.
std::optional<std::vector<ZipEntry>> ReadZipEntries(std::span<const u8> zip) {
    constexpr size_t EocdSize = 22;
    if (zip.size() < EocdSize) {
        return std::nullopt;
    }
    size_t eocd = std::string::npos;
    const size_t lowest = zip.size() > EocdSize + 0xFFFF ? zip.size() - EocdSize - 0xFFFF : 0;
    for (size_t at = zip.size() - EocdSize + 1; at-- > lowest;) {
        if (GetU32(zip, at) == 0x06054b50) {
            eocd = at;
            break;
        }
    }
    if (eocd == std::string::npos) {
        return std::nullopt;
    }
    const u16 count = GetU16(zip, eocd + 10);
    const u32 cd_size = GetU32(zip, eocd + 12);
    const u32 cd_offset = GetU32(zip, eocd + 16);
    if (GetU16(zip, eocd + 4) != 0 || GetU16(zip, eocd + 6) != 0 ||
        GetU16(zip, eocd + 8) != count || static_cast<u64>(cd_offset) + cd_size > eocd) {
        return std::nullopt; // multi-disk or Zip64 archive, or inconsistent directory
    }

    std::vector<ZipEntry> entries;
    size_t at = cd_offset;
    for (u16 i = 0; i < count; ++i) {
        if (at + 46 > static_cast<size_t>(cd_offset) + cd_size || GetU32(zip, at) != 0x02014b50) {
            return std::nullopt;
        }
        const u16 flags = GetU16(zip, at + 8);
        ZipEntry e;
        e.method = GetU16(zip, at + 10);
        e.crc = GetU32(zip, at + 16);
        e.compressed = GetU32(zip, at + 20);
        e.uncompressed = GetU32(zip, at + 24);
        const u16 name_len = GetU16(zip, at + 28);
        const u16 extra_len = GetU16(zip, at + 30);
        const u16 comment_len = GetU16(zip, at + 32);
        const u32 local = GetU32(zip, at + 42);
        if (at + 46 + name_len > zip.size()) {
            return std::nullopt;
        }
        e.name.assign(reinterpret_cast<const char*>(zip.data() + at + 46), name_len);
        at += 46 + static_cast<size_t>(name_len) + extra_len + comment_len;

        if ((flags & 0x1) != 0 || (e.method != 0 && e.method != 8) ||
            e.compressed == 0xFFFFFFFF || e.uncompressed == 0xFFFFFFFF) {
            return std::nullopt; // encrypted, unknown method or Zip64 entry
        }
        if (static_cast<u64>(local) + 30 > zip.size() || GetU32(zip, local) != 0x04034b50) {
            return std::nullopt;
        }
        const size_t data = static_cast<size_t>(local) + 30 + GetU16(zip, local + 26) +
                            GetU16(zip, local + 28);
        if (static_cast<u64>(data) + e.compressed > zip.size()) {
            return std::nullopt;
        }
        e.data_offset = data;
        entries.push_back(std::move(e));
    }
    return entries;
}

bool Inflate(std::span<const u8> in, std::vector<u8>& out, size_t expected) {
    out.assign(expected, 0);
    z_stream zs{};
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
        return false;
    }
    zs.next_in = const_cast<Bytef*>(in.data());
    zs.avail_in = static_cast<uInt>(in.size());
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());
    int rc = inflate(&zs, Z_FINISH);
    if (expected == 0 && rc == Z_BUF_ERROR) {
        // An empty stored-as-deflate file: only the end of stream is acceptable.
        u8 probe = 0;
        zs.next_out = &probe;
        zs.avail_out = 1;
        rc = inflate(&zs, Z_FINISH);
        const bool ok = rc == Z_STREAM_END && zs.total_out == 0;
        inflateEnd(&zs);
        return ok;
    }
    const bool ok = rc == Z_STREAM_END && zs.total_out == expected;
    inflateEnd(&zs);
    return ok;
}

u32 Crc(std::span<const u8> data) {
    uLong crc = crc32(0L, Z_NULL, 0);
    size_t done = 0;
    while (done < data.size()) {
        const size_t chunk = std::min<size_t>(data.size() - done, 1U << 30);
        crc = crc32(crc, data.data() + done, static_cast<uInt>(chunk));
        done += chunk;
    }
    return static_cast<u32>(crc);
}

std::pair<u16, u16> DosNow() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    const int year = std::max(tm.tm_year + 1900, 1980);
    const u16 dos_t = static_cast<u16>((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
    const u16 dos_d =
        static_cast<u16>(((year - 1980) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
    return {dos_t, dos_d};
}

// Howard Hinnant's days_from_civil.
s64 DaysFromCivil(s64 y, unsigned m, unsigned d) {
    y -= m <= 2 ? 1 : 0;
    const s64 era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned mp = m > 2 ? m - 3 : m + 9;
    const unsigned doy = (153 * mp + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<s64>(doe) - 719468;
}

std::mutex g_store_mutex;
fs::path g_store_dir;

fs::path StoreDir() {
    if (g_store_dir.empty()) {
        g_store_dir = Common::FS::GetCitronPath(Common::FS::CitronPath::ConfigDir);
    }
    return g_store_dir;
}

fs::path StorePath() {
    return StoreDir() / "nextendo_olsc.json";
}

nlohmann::json LoadStore() {
    std::ifstream in(StorePath(), std::ios::binary);
    if (!in) {
        return nlohmann::json::object();
    }
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    return j.is_object() ? j : nlohmann::json::object();
}

void SaveStore(const nlohmann::json& j) {
    std::error_code ec;
    fs::create_directories(StoreDir(), ec);
    const fs::path tmp = fs::path(StorePath().string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << j.dump();
        if (!out) {
            return;
        }
    }
    fs::rename(tmp, StorePath(), ec);
}

OlscEntry FromJson(const nlohmann::json& j) {
    OlscEntry e;
    if (!j.is_object()) {
        return e;
    }
    e.setting = j.value("setting", u8{0});
    e.custom = j.value("custom", std::string{});
    e.has_cache = j.value("has_cache", false);
    e.cache_id = j.value("cache_id", u64{0});
    e.cache_time = j.value("cache_time", s64{0});
    e.cache_custom = j.value("cache_custom", std::string{});
    e.last_uploaded_hash = j.value("last_uploaded_hash", std::string{});
    e.pending_upload = j.value("pending_upload", false);
    e.pending_save_root = j.value("pending_save_root", std::string{});
    return e;
}

nlohmann::json ToJson(const OlscEntry& e) {
    return {{"setting", e.setting},
            {"custom", e.custom},
            {"has_cache", e.has_cache},
            {"cache_id", e.cache_id},
            {"cache_time", e.cache_time},
            {"cache_custom", e.cache_custom},
            {"last_uploaded_hash", e.last_uploaded_hash},
            {"pending_upload", e.pending_upload},
            {"pending_save_root", e.pending_save_root}};
}

} // Anonymous namespace

void WriteBackupInfo(std::span<u8> dst, u64 id, s64 posix_time, std::span<const u8> custom) {
    if (dst.size() < BackupInfoSize) {
        return;
    }
    std::fill(dst.begin(), dst.begin() + BackupInfoSize, u8{0});
    std::memcpy(dst.data(), &id, sizeof(id));
    std::memcpy(dst.data() + 8, &posix_time, sizeof(posix_time));
    std::memcpy(dst.data() + 0x10, custom.data(), std::min(custom.size(), CustomDataSize));
}

std::optional<s64> ParseIsoTime(std::string_view t) {
    const auto num = [&t](size_t at, size_t len) -> std::optional<int> {
        if (at + len > t.size()) {
            return std::nullopt;
        }
        int v = 0;
        for (size_t i = at; i < at + len; ++i) {
            if (t[i] < '0' || t[i] > '9') {
                return std::nullopt;
            }
            v = v * 10 + (t[i] - '0');
        }
        return v;
    };
    const auto y = num(0, 4), mo = num(5, 2), d = num(8, 2), h = num(11, 2), mi = num(14, 2),
               s = num(17, 2);
    if (!y || !mo || !d || !h || !mi || !s || t[4] != '-' || t[7] != '-' ||
        (t[10] != 'T' && t[10] != 't' && t[10] != ' ') || t[13] != ':' || t[16] != ':' ||
        *mo < 1 || *mo > 12 || *d < 1 || *d > 31 || *h > 23 || *mi > 59 || *s > 60) {
        return std::nullopt;
    }
    size_t at = 19;
    if (at < t.size() && t[at] == '.') {
        ++at;
        while (at < t.size() && t[at] >= '0' && t[at] <= '9') {
            ++at;
        }
    }
    s64 offset = 0;
    if (at < t.size() && (t[at] == 'Z' || t[at] == 'z')) {
        ++at;
    } else if (at < t.size() && (t[at] == '+' || t[at] == '-')) {
        const auto oh = num(at + 1, 2), om = num(at + 4, 2);
        if (!oh || !om || t[at + 3] != ':') {
            return std::nullopt;
        }
        offset = (t[at] == '+' ? 1 : -1) * (static_cast<s64>(*oh) * 3600 + *om * 60);
        at += 6;
    }
    if (at != t.size()) {
        return std::nullopt;
    }
    return DaysFromCivil(*y, static_cast<unsigned>(*mo), static_cast<unsigned>(*d)) * 86400 +
           static_cast<s64>(*h) * 3600 + *mi * 60 + *s - offset;
}

std::string Base64Encode(std::span<const u8> data) {
    static constexpr char Table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        const u32 n = (static_cast<u32>(data[i]) << 16) |
                      (i + 1 < data.size() ? static_cast<u32>(data[i + 1]) << 8 : 0) |
                      (i + 2 < data.size() ? static_cast<u32>(data[i + 2]) : 0);
        out.push_back(Table[(n >> 18) & 63]);
        out.push_back(Table[(n >> 12) & 63]);
        out.push_back(i + 1 < data.size() ? Table[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < data.size() ? Table[n & 63] : '=');
    }
    return out;
}

std::optional<std::vector<u8>> Base64Decode(std::string_view text) {
    const auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        return c == '+' ? 62 : c == '/' ? 63 : -1;
    };
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    std::vector<u8> out;
    out.reserve(text.size() / 4 * 3);
    for (size_t i = 0; i < text.size(); i += 4) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = text[i + k];
            if (c == '=' && i + 4 == text.size() && k >= 2) {
                v[k] = 0;
                ++pad;
                continue;
            }
            if (pad > 0) {
                return std::nullopt;
            }
            v[k] = value(c);
            if (v[k] < 0) {
                return std::nullopt;
            }
        }
        const u32 n = (static_cast<u32>(v[0]) << 18) | (static_cast<u32>(v[1]) << 12) |
                      (static_cast<u32>(v[2]) << 6) | static_cast<u32>(v[3]);
        out.push_back(static_cast<u8>(n >> 16));
        if (pad < 2) {
            out.push_back(static_cast<u8>(n >> 8));
        }
        if (pad < 1) {
            out.push_back(static_cast<u8>(n));
        }
    }
    return out;
}

CloudIsland ParseMeta(std::string_view json) {
    CloudIsland c;
    const nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (!j.is_object()) {
        return c;
    }
    if (const auto it = j.find("exists"); it != j.end() && it->is_boolean()) {
        c.exists = it->get<bool>();
    }
    if (const auto it = j.find("rev"); it != j.end() && it->is_number_unsigned()) {
        c.rev = it->get<u64>();
    }
    if (const auto it = j.find("updatedAt"); it != j.end() && it->is_string()) {
        c.posix_time = ParseIsoTime(it->get<std::string>()).value_or(0);
    }
    if (const auto it = j.find("custom"); it != j.end() && it->is_string()) {
        auto raw = Base64Decode(it->get<std::string>());
        if (raw && raw->size() <= CustomDataSize) {
            c.custom = std::move(*raw);
        }
    }
    return c;
}

bool IsRestorable(const CloudIsland& island) {
    return island.exists && !island.custom.empty();
}

bool IsEmulatorFile(std::string_view name) {
    return name == ".lock" || name == ".citron_save_size" || name == "ExtraData0" ||
           name == "ExtraData1" || name == "ExtraData_" ||
           name.find("mirror_backup") != std::string_view::npos;
}

std::string Fingerprint(const fs::path& save_dir) {
    const auto files = IslandFiles(save_dir);
    if (files.empty()) {
        return "";
    }
    u64 h = 0xcbf29ce484222325ULL;
    const auto mix = [&h](const u8* p, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            h = (h ^ p[i]) * 0x100000001b3ULL;
        }
    };
    std::vector<u8> bytes;
    for (const auto& [rel, full] : files) {
        if (!ReadFile(full, bytes)) {
            return "unreadable";
        }
        mix(reinterpret_cast<const u8*>(rel.data()), rel.size());
        u8 size_le[8];
        const u64 size = bytes.size();
        for (int i = 0; i < 8; ++i) {
            size_le[i] = static_cast<u8>(size >> (8 * i));
        }
        mix(size_le, sizeof(size_le));
        mix(bytes.data(), bytes.size());
    }
    return fmt::format("{:016x}", h);
}

std::vector<u8> ZipDirectory(const fs::path& save_dir) {
    const auto files = IslandFiles(save_dir);
    if (files.empty()) {
        return {};
    }
    const auto [dos_time, dos_date] = DosNow();
    std::vector<u8> out;
    std::vector<u8> central;
    std::vector<u8> bytes;
    u16 count = 0;
    for (const auto& [rel, full] : files) {
        if (!ReadFile(full, bytes) || bytes.size() >= 0xFFFFFFFFULL || out.size() >= 0xFFFFFFFFULL ||
            count == 0xFFFF) {
            return {};
        }
        const u32 crc = Crc(bytes);
        const u32 size = static_cast<u32>(bytes.size());
        const u32 offset = static_cast<u32>(out.size());
        const u16 name_len = static_cast<u16>(rel.size());

        PutU32(out, 0x04034b50);
        PutU16(out, 20);
        PutU16(out, 0x0800); // UTF-8 names
        PutU16(out, 0);      // stored
        PutU16(out, dos_time);
        PutU16(out, dos_date);
        PutU32(out, crc);
        PutU32(out, size);
        PutU32(out, size);
        PutU16(out, name_len);
        PutU16(out, 0);
        out.insert(out.end(), rel.begin(), rel.end());
        out.insert(out.end(), bytes.begin(), bytes.end());

        PutU32(central, 0x02014b50);
        PutU16(central, 20);
        PutU16(central, 20);
        PutU16(central, 0x0800);
        PutU16(central, 0);
        PutU16(central, dos_time);
        PutU16(central, dos_date);
        PutU32(central, crc);
        PutU32(central, size);
        PutU32(central, size);
        PutU16(central, name_len);
        PutU16(central, 0);
        PutU16(central, 0);
        PutU16(central, 0);
        PutU16(central, 0);
        PutU32(central, 0);
        PutU32(central, offset);
        central.insert(central.end(), rel.begin(), rel.end());
        ++count;
    }
    if (out.size() + central.size() >= 0xFFFFFFFFULL) {
        return {};
    }
    const u32 cd_offset = static_cast<u32>(out.size());
    out.insert(out.end(), central.begin(), central.end());
    PutU32(out, 0x06054b50);
    PutU16(out, 0);
    PutU16(out, 0);
    PutU16(out, count);
    PutU16(out, count);
    PutU32(out, static_cast<u32>(central.size()));
    PutU32(out, cd_offset);
    PutU16(out, 0);
    return out;
}

bool ExtractIsland(std::span<const u8> zip, const fs::path& new_dir) {
    std::error_code ec;
    if (fs::exists(new_dir, ec)) {
        return false;
    }
    const auto entries = ReadZipEntries(zip);
    if (!entries) {
        return false;
    }

    // Everything is checked before anything is written.
    u64 total = 0;
    bool has_main = false;
    std::set<std::string> names;
    std::vector<const ZipEntry*> files;
    for (const ZipEntry& e : *entries) {
        std::string name = e.name;
        std::replace(name.begin(), name.end(), '\\', '/');
        if (!name.empty() && name.back() == '/') {
            if (!IsSafeEntryName(name.substr(0, name.size() - 1))) {
                return false;
            }
            continue; // directory entry
        }
        if (!IsSafeEntryName(name)) {
            return false;
        }
        if (name.find('/') == std::string::npos && IsEmulatorFile(name)) {
            return false;
        }
        if (!names.insert(Lower(name)).second) {
            return false; // the same file twice (also case-insensitive, for Windows)
        }
        total += e.uncompressed;
        if (total > MaxExtractedBytes) {
            return false;
        }
        if (e.method == 0 && e.compressed != e.uncompressed) {
            return false;
        }
        has_main |= name == "main.dat";
        files.push_back(&e);
    }
    if (!has_main) {
        return false;
    }

    std::vector<std::pair<fs::path, std::vector<u8>>> contents;
    contents.reserve(files.size());
    for (const ZipEntry* e : files) {
        std::string name = e->name;
        std::replace(name.begin(), name.end(), '\\', '/');
        const auto raw = zip.subspan(e->data_offset, e->compressed);
        std::vector<u8> data;
        if (e->method == 0) {
            data.assign(raw.begin(), raw.end());
        } else if (!Inflate(raw, data, e->uncompressed)) {
            return false;
        }
        if (Crc(data) != e->crc) {
            return false;
        }
        contents.emplace_back(new_dir / fs::path(name), std::move(data));
    }

    fs::create_directories(new_dir, ec);
    if (ec) {
        return false;
    }
    const fs::path root = fs::weakly_canonical(new_dir, ec);
    for (const auto& [path, data] : contents) {
        fs::create_directories(path.parent_path(), ec);
        const fs::path parent = fs::weakly_canonical(path.parent_path(), ec);
        const bool inside =
            std::mismatch(root.begin(), root.end(), parent.begin(), parent.end()).first ==
            root.end();
        if (ec || !inside || !WriteFile(path, data)) {
            RemoveAll(new_dir);
            return false;
        }
    }
    return true;
}

const char* OutcomeName(Outcome outcome) {
    switch (outcome) {
    case Outcome::Ok:
        return "Ok";
    case Outcome::Unavailable:
        return "Unavailable";
    case Outcome::NetworkError:
        return "NetworkError";
    case Outcome::Refused:
        return "Refused";
    case Outcome::NoLocalSave:
        return "NoLocalSave";
    case Outcome::InvalidIsland:
        return "InvalidIsland";
    case Outcome::Unchanged:
        return "Unchanged";
    case Outcome::Deferred:
        return "Deferred";
    }
    return "?";
}

Outcome ReplaceIsland(const fs::path& save_dir, std::span<const u8> zip, const fs::path& backups_dir,
                      const std::string& prefix) {
    std::error_code ec;
    if (save_dir.empty() || !fs::is_directory(save_dir, ec)) {
        return Outcome::NoLocalSave;
    }
    const fs::path fresh = fs::path(save_dir.string() + ".nx_restore_new");
    const fs::path old = fs::path(save_dir.string() + ".nx_restore_old");
    RemoveAll(fresh);
    RemoveAll(old);

    if (!ExtractIsland(zip, fresh)) {
        RemoveAll(fresh);
        return Outcome::InvalidIsland;
    }

    // This machine's island is kept first (last 5).
    if (!IslandFiles(save_dir).empty()) {
        const std::vector<u8> current = ZipDirectory(save_dir);
        fs::create_directories(backups_dir, ec);
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &now);
#else
        gmtime_r(&now, &tm);
#endif
        static u32 sequence = 0;
        const fs::path backup =
            backups_dir / fmt::format("{}-{:04}{:02}{:02}T{:02}{:02}{:02}{:03}Z.zip", prefix,
                                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                                      tm.tm_min, tm.tm_sec, sequence++ % 1000);
        if (current.empty() || !WriteFile(backup, current)) {
            RemoveAll(fresh);
            return Outcome::NoLocalSave;
        }
        std::vector<fs::path> kept;
        for (const auto& e : fs::directory_iterator(backups_dir, ec)) {
            const std::string name = e.path().filename().string();
            if (name.rfind(prefix + "-", 0) == 0 && e.path().extension() == ".zip") {
                kept.push_back(e.path());
            }
        }
        std::sort(kept.begin(), kept.end(), std::greater<>{});
        for (size_t i = 5; i < kept.size(); ++i) {
            fs::remove(kept[i], ec);
        }
    }

    // Top-level island entries of the save (the emulator's own files stay in place).
    std::vector<fs::path> island_entries;
    for (const auto& e : fs::directory_iterator(save_dir, ec)) {
        if (!IsEmulatorFile(e.path().filename().string())) {
            island_entries.push_back(e.path().filename());
        }
    }
    std::vector<fs::path> fresh_entries;
    for (const auto& e : fs::directory_iterator(fresh, ec)) {
        fresh_entries.push_back(e.path().filename());
    }

    fs::create_directories(old, ec);
    std::vector<fs::path> moved_out;
    std::vector<fs::path> moved_in;
    const auto roll_back = [&] {
        std::error_code rc;
        for (const auto& name : moved_in) {
            fs::remove_all(save_dir / name, rc);
        }
        for (const auto& name : moved_out) {
            fs::rename(old / name, save_dir / name, rc);
        }
        RemoveAll(fresh);
        if (fs::is_empty(old, rc)) {
            RemoveAll(old);
        }
        return Outcome::NoLocalSave; // the copy stays in backups_dir
    };

    for (const auto& name : island_entries) {
        fs::rename(save_dir / name, old / name, ec);
        if (ec) {
            return roll_back();
        }
        moved_out.push_back(name);
    }
    for (const auto& name : fresh_entries) {
        fs::rename(fresh / name, save_dir / name, ec);
        if (ec) {
            return roll_back();
        }
        moved_in.push_back(name);
    }

    RemoveAll(old);
    RemoveAll(fresh);
    return Outcome::Ok;
}

void SetStoreDirectory(const fs::path& dir) {
    std::lock_guard lk{g_store_mutex};
    g_store_dir = dir;
}

std::string Key(u64 title_id, std::string_view uid_hex) {
    return fmt::format("{:016x}:{}", title_id, Lower(std::string(uid_hex)));
}

OlscEntry Get(const std::string& key) {
    std::lock_guard lk{g_store_mutex};
    const nlohmann::json store = LoadStore();
    const auto entries = store.find("entries");
    if (entries == store.end() || !entries->is_object()) {
        return {};
    }
    const auto it = entries->find(key);
    return it == entries->end() ? OlscEntry{} : FromJson(*it);
}

void Update(const std::string& key, const std::function<void(OlscEntry&)>& change) {
    std::lock_guard lk{g_store_mutex};
    nlohmann::json store = LoadStore();
    if (!store.contains("entries") || !store["entries"].is_object()) {
        store["entries"] = nlohmann::json::object();
    }
    OlscEntry e = FromJson(store["entries"].value(key, nlohmann::json::object()));
    change(e);
    store["entries"][key] = ToJson(e);
    SaveStore(store);
}

std::vector<std::pair<std::string, OlscEntry>> AllPending() {
    std::lock_guard lk{g_store_mutex};
    const nlohmann::json store = LoadStore();
    std::vector<std::pair<std::string, OlscEntry>> out;
    const auto entries = store.find("entries");
    if (entries == store.end() || !entries->is_object()) {
        return out;
    }
    for (const auto& item : entries->items()) {
        OlscEntry e = FromJson(item.value());
        if (e.pending_upload) {
            out.emplace_back(item.key(), std::move(e));
        }
    }
    return out;
}

void NoteUser(u64 title_id, const std::string& key) {
    std::lock_guard lk{g_store_mutex};
    nlohmann::json store = LoadStore();
    const std::string title = fmt::format("{:016x}", title_id);
    if (store.contains("last_user") && store["last_user"].is_object() &&
        store["last_user"].value(title, std::string{}) == key) {
        return;
    }
    if (!store.contains("last_user") || !store["last_user"].is_object()) {
        store["last_user"] = nlohmann::json::object();
    }
    store["last_user"][title] = key;
    SaveStore(store);
}

std::string LastUserKey(u64 title_id) {
    std::lock_guard lk{g_store_mutex};
    const nlohmann::json store = LoadStore();
    const auto it = store.find("last_user");
    if (it == store.end() || !it->is_object()) {
        return "";
    }
    return it->value(fmt::format("{:016x}", title_id), std::string{});
}

} // namespace Service::OLSC::NextendoIsland
