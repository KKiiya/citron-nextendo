// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include "web_service/game_catalog_manifest.h"

namespace WebService::GameCatalog {
namespace {

using Json = nlohmann::json;
constexpr std::size_t MaxDocument = 1024 * 1024;

std::vector<std::uint8_t> Base64(const std::string& text) {
    if (text.empty() || text.size() > MaxDocument * 2 || text.size() % 4 != 0) {
        throw std::runtime_error("Invalid game catalog encoding");
    }
    std::vector<std::uint8_t> result(text.size() / 4 * 3);
    const int count =
        EVP_DecodeBlock(result.data(), reinterpret_cast<const unsigned char*>(text.data()),
                        static_cast<int>(text.size()));
    if (count < 0) {
        throw std::runtime_error("Invalid game catalog encoding");
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

std::int64_t Timestamp(const std::string& text) {
    if (text.size() < 20 || text.back() != 'Z') {
        throw std::runtime_error("Invalid game catalog timestamp");
    }
    if (text.size() > 20 &&
        (text[19] != '.' || !std::all_of(text.begin() + 20, text.end() - 1,
                                         [](char c) { return c >= '0' && c <= '9'; }))) {
        throw std::runtime_error("Invalid game catalog timestamp");
    }
    std::tm tm{};
    std::istringstream input(text.substr(0, 19));
    input >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (input.fail()) {
        throw std::runtime_error("Invalid game catalog timestamp");
    }
#ifdef _WIN32
    return static_cast<std::int64_t>(_mkgmtime(&tm));
#else
    return static_cast<std::int64_t>(timegm(&tm));
#endif
}

std::uint64_t TitleId(const Json& value) {
    const auto text = value.get<std::string>();
    if (text.size() != 16 || !std::all_of(text.begin(), text.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) {
        throw std::runtime_error("Invalid game catalog title ID");
    }
    const auto title_id = std::stoull(text, nullptr, 16);
    if (title_id == 0) {
        throw std::runtime_error("Invalid game catalog title ID");
    }
    return title_id;
}

std::vector<std::string> Strings(const Json& value, std::size_t max_items, std::size_t max_length) {
    if (!value.is_array() || value.size() > max_items) {
        throw std::runtime_error("Invalid game catalog list");
    }
    std::vector<std::string> result;
    for (const auto& item : value) {
        auto text = item.get<std::string>();
        if (text.empty() || text.size() > max_length ||
            std::any_of(text.begin(), text.end(), [](char c) {
                return static_cast<unsigned char>(c) < 0x20;
            }) ||
            std::find(result.begin(), result.end(), text) != result.end()) {
            throw std::runtime_error("Invalid or duplicate game catalog list entry");
        }
        result.push_back(std::move(text));
    }
    return result;
}

Json VerifyEnvelope(const std::string& envelope, const std::string& public_key) {
    if (envelope.size() > MaxDocument) {
        throw std::runtime_error("Game catalog too large");
    }
    const auto outer = Json::parse(envelope);
    if (outer.at("key_id") != "catalog-key-1") {
        throw std::runtime_error("Unknown game catalog signing key");
    }
    const auto key = Base64(public_key);
    const auto payload = Base64(outer.at("payload").get<std::string>());
    const auto signature = Base64(outer.at("signature").get<std::string>());
    if (key.size() != 32 || signature.size() != 64) {
        throw std::runtime_error("Invalid game catalog signature size");
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
        throw std::runtime_error("Invalid game catalog signature");
    }
    return Json::parse(payload.begin(), payload.end());
}

} // namespace

Nextendo::CompatibleTitles::CatalogRequirements VerifyCatalog(const std::string& envelope,
                                                              const std::string& public_key,
                                                              std::int64_t now,
                                                              bool allow_expired) {
    const auto doc = VerifyEnvelope(envelope, public_key);
    if (doc.at("document_type") != "game_catalog" || doc.at("schema_version") != 1 ||
        !doc.at("revision").is_number_unsigned() || doc.at("revision").get<std::uint64_t>() == 0) {
        throw std::runtime_error("Unsupported game catalog");
    }
    const auto issued = Timestamp(doc.at("issued_at").get<std::string>());
    const auto expires = Timestamp(doc.at("expires_at").get<std::string>());
    if (issued > now + 300 || expires <= issued || expires - issued > 86400 ||
        (!allow_expired && expires <= now)) {
        throw std::runtime_error("Game catalog outside its validity period");
    }

    Nextendo::CompatibleTitles::CatalogRequirements result;
    result.revision = doc.at("revision").get<std::uint64_t>();
    result.expires_at = expires;

    const auto& games = doc.at("games");
    if (!games.is_array() || games.size() > 10000) {
        throw std::runtime_error("Invalid game catalog entries");
    }
    std::set<std::uint64_t> seen;
    for (const auto& game : games) {
        const auto& ids = game.at("title_ids");
        if (!ids.is_array() || ids.empty() || ids.size() > 32 || !game.at("enabled").is_boolean()) {
            throw std::runtime_error("Invalid game catalog entry");
        }
        std::vector<std::uint64_t> title_ids;
        for (const auto& id : ids) {
            const auto title_id = TitleId(id);
            // An ID listed twice would make its requirements ambiguous: refuse the whole document.
            if (!seen.insert(title_id).second) {
                throw std::runtime_error("Duplicate game catalog title ID");
            }
            title_ids.push_back(title_id);
        }

        const bool enabled = game.at("enabled").get<bool>();
        const auto capabilities = Strings(game.at("required_capabilities"), 64, 64);
        bool evaluable = std::all_of(capabilities.begin(), capabilities.end(), [](const auto& c) {
            return c == CapabilityExact || c == CapabilityAny;
        });

        std::string policy;
        std::vector<std::string> accepted;
        std::string required;
        if (enabled || game.contains("versions")) {
            const auto& versions = game.at("versions");
            policy = versions.at("policy").get<std::string>();
            if (policy == "exact") {
                accepted = Strings(versions.at("accepted"), 64, 32);
                if (accepted.empty()) {
                    throw std::runtime_error("Game catalog entry accepts no version");
                }
                required = versions.value("recommended", accepted.front());
                if (std::find(accepted.begin(), accepted.end(), required) == accepted.end()) {
                    throw std::runtime_error("Recommended version is not accepted");
                }
            } else if (policy == "any") {
                if (versions.contains("accepted") || versions.contains("recommended")) {
                    throw std::runtime_error("Policy any lists versions");
                }
            }
            // A missing policy capability, or an unknown policy, is never read as "any version".
            evaluable = evaluable &&
                        ((policy == "exact" && std::find(capabilities.begin(), capabilities.end(),
                                                         CapabilityExact) != capabilities.end()) ||
                         (policy == "any" && std::find(capabilities.begin(), capabilities.end(),
                                                       CapabilityAny) != capabilities.end()));
        }

        for (const auto title_id : title_ids) {
            result.table.emplace(title_id, required);
            std::vector<std::string> others;
            std::copy_if(accepted.begin(), accepted.end(), std::back_inserter(others),
                         [&](const auto& version) { return version != required; });
            if (!others.empty()) {
                result.also_accepted.emplace(title_id, std::move(others));
            }
            if (!enabled || !evaluable) {
                result.blocked.insert(title_id);
            } else if (policy == "any") {
                result.any_version.insert(title_id);
            }
        }
    }
    return result;
}

} // namespace WebService::GameCatalog
