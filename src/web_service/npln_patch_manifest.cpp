// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <ctime>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include "web_service/npln_patch_manifest.h"

namespace WebService::NplnPatchCatalog {
namespace {

using Json = nlohmann::json;
constexpr std::size_t MaxDocument = 1024 * 1024;

std::vector<std::uint8_t> Base64(const std::string& text) {
    if (text.empty() || text.size() > MaxDocument || text.size() % 4 != 0) {
        throw std::runtime_error("Invalid patch catalog encoding");
    }

    std::vector<std::uint8_t> result(text.size() / 4 * 3);
    const int count =
        EVP_DecodeBlock(result.data(), reinterpret_cast<const unsigned char*>(text.data()),
                        static_cast<int>(text.size()));

    if (count < 0) {
        throw std::runtime_error("Invalid patch catalog encoding");
    }

    std::size_t size = static_cast<std::size_t>(count);
    if (text.back() == '=') {
        --size;
    }
    if (text.size() > 1 && text[text.size() - 2] == '=') {
        --size;
    }
    result.resize(size);
    return result;
}

std::vector<std::uint8_t> Hex(const std::string& text, std::size_t max_bytes) {
    if (text.empty() || text.size() % 2 != 0 || text.size() / 2 > max_bytes) {
        throw std::runtime_error("Invalid patch hex bytes");
    }

    std::vector<std::uint8_t> result;
    auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9')
            return static_cast<unsigned>(c - '0');
        if (c >= 'A' && c <= 'F')
            return static_cast<unsigned>(c - 'A' + 10);
        if (c >= 'a' && c <= 'f')
            return static_cast<unsigned>(c - 'a' + 10);
        throw std::runtime_error("Invalid patch hex digit");
    };

    for (std::size_t i = 0; i < text.size(); i += 2) {
        result.push_back(static_cast<std::uint8_t>((digit(text[i]) << 4) | digit(text[i + 1])));
    }
    return result;
}

std::int64_t Timestamp(const std::string& text) {
    if (text.size() < 20 || text.back() != 'Z') {
        throw std::runtime_error("Invalid patch catalog timestamp");
    }
    if (text.size() > 20 &&
        (text[19] != '.' || !std::all_of(text.begin() + 20, text.end() - 1,
                                         [](char c) { return c >= '0' && c <= '9'; }))) {
        throw std::runtime_error("Invalid patch catalog timestamp");
    }

    std::tm tm{};
    std::istringstream input(text.substr(0, 19));
    input >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (input.fail()) {
        throw std::runtime_error("Invalid patch catalog timestamp");
    }
#ifdef _WIN32
    return static_cast<std::int64_t>(_mkgmtime(&tm));
#else
    return static_cast<std::int64_t>(timegm(&tm));
#endif
}

void ValidateRecords(const Patch& patch) {
    const auto& bytes = patch.ips;
    if (bytes.size() < 15 || bytes.size() > 65536 ||
        std::string(bytes.begin(), bytes.begin() + 5) != "IPS32" ||
        std::string(bytes.end() - 4, bytes.end()) != "EEOF") {
        throw std::runtime_error("Invalid IPS32 patch");
    }

    std::size_t position = 5;
    std::size_t record = 0;
    std::uint64_t previous_end = 0;
    for (const auto& guard : patch.guards) {
        if (position + 6 > bytes.size() - 4) {
            throw std::runtime_error("Truncated IPS32 patch");
        }
        std::uint32_t offset = 0;
        for (int i = 0; i < 4; ++i) {
            offset = (offset << 8) | bytes[position++];
        }
        const std::size_t length =
            (static_cast<std::size_t>(bytes[position]) << 8) | bytes[position + 1];
        position += 2;

        if (length == 0 || offset < 0x100 || offset < previous_end ||
            static_cast<std::uint64_t>(offset) + length > 512ULL * 1024 * 1024 ||
            position + length > bytes.size() - 4 || offset != guard.offset ||
            length != guard.original.size() || length != guard.patched.size() ||
            !std::equal(guard.patched.begin(), guard.patched.end(), bytes.begin() + position)) {
            throw std::runtime_error("IPS32 write does not match its guard");
        }
        position += length;
        previous_end = static_cast<std::uint64_t>(offset) + length;
        ++record;
    }
    if (position != bytes.size() - 4 || record == 0) {
        throw std::runtime_error("Unprotected IPS32 write");
    }
}

Json VerifyDocument(const std::string& envelope, const std::string& public_key,
                    std::string_view document_type, std::int64_t now, bool allow_expired) {
    if (envelope.size() > MaxDocument) {
        throw std::runtime_error("Patch catalog too large");
    }

    const auto outer = Json::parse(envelope);
    if (outer.at("key_id") != "npln-patches-key-1") {
        throw std::runtime_error("Unknown patch signing key");
    }
    const auto key = Base64(public_key);
    const auto payload = Base64(outer.at("payload").get<std::string>());
    const auto signature = Base64(outer.at("signature").get<std::string>());
    if (key.size() != 32 || signature.size() != 64) {
        throw std::runtime_error("Invalid patch signature size");
    }

    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> verifier(
        EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, key.data(), key.size()),
        EVP_PKEY_free);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                    EVP_MD_CTX_free);
    if (!verifier || !context ||
        EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, verifier.get()) != 1 ||
        EVP_DigestVerify(context.get(), signature.data(), signature.size(), payload.data(),
                         payload.size()) != 1) {
        throw std::runtime_error("Invalid patch catalog signature");
    }

    const auto doc = Json::parse(payload.begin(), payload.end());
    if (doc.at("document_type").get<std::string>() != document_type || doc.at("schema_version") != 1 ||
        !doc.at("revision").is_number_unsigned() || doc.at("revision").get<std::uint64_t>() == 0) {
        throw std::runtime_error("Unsupported patch catalog");
    }
    const auto issued = Timestamp(doc.at("issued_at").get<std::string>());
    const auto expires = Timestamp(doc.at("expires_at").get<std::string>());
    if (issued > now + 300 || expires <= issued || expires - issued > 86400 ||
        (!allow_expired && expires <= now)) {
        throw std::runtime_error("Patch catalog outside its validity period");
    }

    return doc;
}

void ValidateTag(const std::string& tag) {
    if (tag.size() != 66 || tag.front() != '"' || tag.back() != '"') {
        throw std::runtime_error("Invalid signed index tag");
    }
    Hex(tag.substr(1, 64), 32);
}

} // namespace

TitleIndex VerifyTitleIndex(const std::string& envelope, const std::string& public_key,
                            std::int64_t now, bool allow_expired) {
    const auto doc = VerifyDocument(envelope, public_key, "npln_title_index", now, allow_expired);
    TitleIndex index{doc.at("revision").get<std::uint64_t>(),
                     Timestamp(doc.at("expires_at").get<std::string>()),
                     doc.at("index_etag").get<std::string>(),
                     {}};
    ValidateTag(index.etag);
    const auto& entries = doc.at("titles");
    if (!entries.is_array() || entries.size() > 10000) {
        throw std::runtime_error("Invalid title index entries");
    }
    for (const auto& entry : entries) {
        const auto title = entry.at("title_id").get<std::string>();
        if (title.size() != 16 || !entry.at("enabled").is_boolean()) {
            throw std::runtime_error("Invalid title index target");
        }
        std::uint64_t title_id = 0;
        for (auto byte : Hex(title, 8)) {
            title_id = (title_id << 8) | byte;
        }
        if (title_id == 0 ||
            !index.titles.emplace(title_id, entry.at("enabled").get<bool>()).second) {
            throw std::runtime_error("Invalid or duplicate title index target");
        }
    }
    return index;
}

void TitleIndexState::Remember(const TitleIndex& index) {
    if (index.revision < revision || (index.revision == revision && index.etag != etag)) {
        throw std::runtime_error("Title index revision rollback or conflict rejected");
    }
    for (const auto& [title, enabled] : index.titles) {
        titles[title] = enabled;
    }
    revision = index.revision;
    etag = index.etag;
}

bool TitleIndexState::HasTitle(std::uint64_t title_id) const {
    const auto entry = titles.find(title_id);
    return entry != titles.end() && entry->second;
}

Manifest VerifyManifest(const std::string& envelope, const std::string& public_key,
                        std::int64_t now, bool allow_expired) {
    const auto doc = VerifyDocument(envelope, public_key, "npln_patch_catalog", now, allow_expired);
    const auto expires = Timestamp(doc.at("expires_at").get<std::string>());
    Manifest manifest{doc.at("revision").get<std::uint64_t>(), expires, {}};
    manifest.etag = doc.value("patch_etag", std::string{});
    if (!manifest.etag.empty()) {
        if (manifest.etag.size() != 66 || manifest.etag.front() != '"' ||
            manifest.etag.back() != '"') {
            throw std::runtime_error("Invalid signed patch revision tag");
        }
        Hex(manifest.etag.substr(1, 64), 32);
    }
    const auto& entries = doc.at("patches");
    if (!entries.is_array() || entries.size() > 256) {
        throw std::runtime_error("Invalid patch catalog entries");
    }

    std::set<std::tuple<std::uint64_t, std::string, std::string>> seen;
    for (const auto& entry : entries) {
        const auto title = entry.at("title_id").get<std::string>();
        const auto build = entry.at("build_id").get<std::string>();
        if (title.size() != 16 || build.size() != 64) {
            throw std::runtime_error("Invalid patch target");
        }
        const auto title_bytes = Hex(title, 8);
        Hex(build, 32);
        std::uint64_t title_id = 0;
        for (auto byte : title_bytes) {
            title_id = (title_id << 8) | byte;
        }
        if (entry.at("offset_basis") != "nso_header_included" ||
            entry.at("required_capabilities") != Json::array({"npln.ips32.v1"})) {
            throw std::runtime_error("Unsupported patch capability");
        }

        Patch patch{entry.at("id").get<std::string>(),
                    title_id,
                    build,
                    entry.at("module").get<std::string>(),
                    entry.at("patch_version").get<std::uint64_t>(),
                    entry.at("enabled").get<bool>(),
                    Base64(entry.at("ips32_base64").get<std::string>()),
                    {}};
        patch.final = entry.value("final", false);
        if (title_id == 0 || patch.id.empty() || patch.id.size() > 80 || patch.module != "main" ||
            patch.version == 0 || !seen.emplace(title_id, build, patch.module).second) {
            throw std::runtime_error("Invalid or duplicate patch identity");
        }

        std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
        SHA256(patch.ips.data(), patch.ips.size(), digest.data());
        if (Hex(entry.at("sha256").get<std::string>(), digest.size()) !=
            std::vector<std::uint8_t>(digest.begin(), digest.end())) {
            throw std::runtime_error("Invalid patch hash");
        }

        const auto& guards = entry.at("guards");
        if (!guards.is_array() || guards.empty() || guards.size() > 256) {
            throw std::runtime_error("Missing patch guards");
        }
        for (const auto& guard : guards) {
            patch.guards.push_back({guard.at("offset").get<std::uint32_t>(),
                                    Hex(guard.at("original_hex").get<std::string>(), 65535),
                                    Hex(guard.at("patched_hex").get<std::string>(), 65535)});
        }
        ValidateRecords(patch);
        manifest.patches.push_back(std::move(patch));
    }
    return manifest;
}

bool CheckGuards(const Patch& patch, std::span<const std::uint8_t> image) {
    for (const auto& guard : patch.guards) {
        if (guard.offset > image.size() || guard.original.size() > image.size() - guard.offset) {
            return false;
        }
        const auto begin = image.begin() + guard.offset;
        if (!std::equal(guard.original.begin(), guard.original.end(), begin) &&
            !std::equal(guard.patched.begin(), guard.patched.end(), begin)) {
            return false;
        }
    }
    return true;
}

} // namespace WebService::NplnPatchCatalog
