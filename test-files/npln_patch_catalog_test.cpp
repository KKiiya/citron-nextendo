// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cassert>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include "web_service/npln_patch_manifest.h"
#include "web_service/npln_patch_state.h"

using nlohmann::json;
using namespace WebService::NplnPatchCatalog;

std::string Base64(std::span<const unsigned char> bytes) {
    std::string result((bytes.size() + 2) / 3 * 4 + 1, '\0');
    const int length = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()),
                                       bytes.data(), static_cast<int>(bytes.size()));
    result.resize(static_cast<std::size_t>(length));
    return result;
}

int main(int argc, char** argv) {
    if (argc == 3) {
        std::ifstream input(argv[1], std::ios::binary);
        const std::string body((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
        const auto manifest =
            VerifyManifest(body, argv[2], static_cast<std::int64_t>(std::time(nullptr)));
        assert(manifest.patches.size() == 1 &&
               manifest.patches[0].title_id == 0x0100965017338000ULL);
        std::cout << "Production patch catalog signature verified, revision " << manifest.revision
                  << "\n";
        return 0;
    }

    std::array<unsigned char, 32> seed{};
    seed.fill(7); // Test-only key; unrelated to the production signer.
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size()),
        EVP_PKEY_free);
    assert(key);

    std::array<unsigned char, 32> public_bytes{};
    std::size_t public_length = public_bytes.size();
    assert(EVP_PKEY_get_raw_public_key(key.get(), public_bytes.data(), &public_length) == 1);
    const auto public_key = Base64(public_bytes);

    auto sign = [&](const json& payload) {
        const std::string text = payload.dump();
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                        EVP_MD_CTX_free);
        assert(EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get()) == 1);
        std::array<unsigned char, 64> signature{};
        std::size_t size = signature.size();
        assert(EVP_DigestSign(context.get(), signature.data(), &size,
                              reinterpret_cast<const unsigned char*>(text.data()),
                              text.size()) == 1);

        return json{
            {"key_id", "npln-patches-key-1"},
            {"payload",
             Base64(std::span(reinterpret_cast<const unsigned char*>(text.data()), text.size()))},
            {"signature", Base64(signature)}}
            .dump();
    };

    json patch{
        {"id", "mpj-230-certificate"},
        {"enabled", true},
        {"title_id", "0100965017338000"},
        {"build_id", "D70CE1726C979E45ED4200E9C2B23B2A258FDC18000000000000000000000000"},
        {"module", "main"},
        {"patch_version", 1},
        {"required_capabilities", {"npln.ips32.v1"}},
        {"offset_basis", "nso_header_included"},
        {"ips32_base64", "SVBTMzIArTm8AAQqAIBSRUVPRg=="},
        {"sha256", "a5e3e1e03a7b94cd2e4b5569f9c6189320291401328bbbd5f88788406de27e0a"},
        {"guards",
         {{{"offset", 0xAD39BC}, {"original_hex", "AAE24039"}, {"patched_hex", "2A008052"}}}}};
    json document{{"document_type", "npln_patch_catalog"},
                  {"schema_version", 1},
                  {"revision", 1},
                  {"issued_at", "1970-01-01T00:00:00Z"},
                  {"expires_at", "1970-01-01T01:00:00Z"},
                  {"patches", {patch}}};

    const auto valid = sign(document);
    const auto manifest = VerifyManifest(valid, public_key, 1);
    assert(manifest.revision == 1 && manifest.patches.size() == 1);
    assert(manifest.patches[0].title_id == 0x0100965017338000ULL);

    std::vector<std::uint8_t> image(0xAD39BC + 4);
    const std::array<std::uint8_t, 4> original{0xAA, 0xE2, 0x40, 0x39};
    std::copy(original.begin(), original.end(), image.begin() + 0xAD39BC);
    assert(CheckGuards(manifest.patches[0], image));
    image[0xAD39BC] = 0;
    assert(!CheckGuards(manifest.patches[0], image));
    assert(!CheckGuards(manifest.patches[0], std::span(image).first(100)));

    auto rejected = [&](const std::string& envelope, std::int64_t now = 1) {
        bool failed = false;
        try {
            VerifyManifest(envelope, public_key, now);
        } catch (const std::exception&) {
            failed = true;
        }
        assert(failed);
    };

    rejected(valid, 3600);
    const auto expired = VerifyManifest(valid, public_key, 3600, true);
    assert(expired.patches.size() == 1);

    auto individual_document = document;
    individual_document["patch_etag"] = "\"" + std::string(64, 'a') + "\"";
    individual_document["patches"][0]["final"] = true;
    const auto final_manifest = VerifyManifest(sign(individual_document), public_key, 1);
    assert(final_manifest.patches[0].final);
    assert(final_manifest.etag == individual_document["patch_etag"].get<std::string>());
    auto invalid_tag = individual_document;
    invalid_tag["patch_etag"] = "untrusted header value";
    rejected(sign(invalid_tag));

    PatchState state;
    state.Remember(expired);
    const auto title_id = expired.patches[0].title_id;
    const auto build_id = expired.patches[0].build_id;
    state.BeginGame(title_id);
    assert(state.Find(title_id, build_id, "main")->version == 1);
    assert(state.Find(title_id, std::string(64, 'F'), "main") == nullptr);
    assert(state.Find(title_id + 1, build_id, "main") == nullptr);

    auto updated = expired;
    updated.revision = 2;
    updated.patches[0].version = 2;
    state.Remember(updated);
    assert(state.Find(title_id, build_id, "main")->version == 1);
    state.EndGame();
    assert(state.Find(title_id, build_id, "main") == nullptr);
    state.BeginGame(title_id);
    assert(state.Find(title_id, build_id, "main")->version == 2);

    // A catalog for a new build must not discard a previously verified older build.
    auto new_build = updated;
    new_build.revision = 3;
    new_build.patches[0].build_id = std::string(64, 'F');
    state.Remember(new_build);
    state.EndGame();
    state.BeginGame(title_id);
    assert(state.Find(title_id, build_id, "main")->version == 2);
    assert(state.Find(title_id, std::string(64, 'F'), "main") != nullptr);

    auto withdrawn = updated;
    withdrawn.revision = 4;
    withdrawn.patches[0].enabled = false;
    state.Remember(withdrawn);
    assert(state.Find(title_id, build_id, "main")->enabled);
    state.Remember(expired);
    state.EndGame();
    state.BeginGame(title_id);
    assert(!state.Find(title_id, build_id, "main")->enabled);
    assert(state.LatestRevision() == 4);

    PatchState final_state;
    final_state.Remember(final_manifest);
    // Importing a legacy full-catalog cache of the same revision must not erase
    // the signed individual patch tag or its final policy.
    final_state.Remember(expired);
    assert(final_state.Cached(title_id, build_id)->final);
    assert(final_state.Tag(title_id, build_id) == final_manifest.etag);
    assert(final_state.Cached(title_id, std::string(64, 'F')) == nullptr);

    auto tampered = json::parse(valid);
    tampered["signature"] = std::string(88, 'A');
    rejected(tampered.dump());
    bool expired_tamper_rejected = false;
    try {
        VerifyManifest(tampered.dump(), public_key, 3600, true);
    } catch (const std::exception&) {
        expired_tamper_rejected = true;
    }
    assert(expired_tamper_rejected);

    for (const std::string kind : {"hash", "guard", "offset", "capability", "duplicate", "type"}) {
        auto bad = document;
        auto& entry = bad["patches"][0];
        if (kind == "hash")
            entry["sha256"] = std::string(64, '0');
        if (kind == "guard")
            entry["guards"][0]["patched_hex"] = "FFFFFFFF";
        if (kind == "offset")
            entry["guards"][0]["offset"] = 0;
        if (kind == "capability")
            entry["required_capabilities"] = {"unsupported"};
        if (kind == "duplicate")
            bad["patches"].push_back(entry);
        if (kind == "type")
            bad["document_type"] = "game_catalog";
        rejected(sign(bad));
    }

    json index_doc{{"document_type", "npln_title_index"},
                   {"schema_version", 1},
                   {"revision", 1},
                   {"issued_at", "1970-01-01T00:00:00Z"},
                   {"expires_at", "1970-01-01T01:00:00Z"},
                   {"index_etag", "\"" + std::string(64, 'a') + "\""},
                   {"titles", {{{"title_id", "0100000000000001"}, {"enabled", true}}}}};
    const auto index = VerifyTitleIndex(sign(index_doc), public_key, 1);
    TitleIndexState discovery;
    discovery.Remember(index);
    assert(discovery.HasTitle(0x0100000000000001ULL));
    assert(!discovery.HasTitle(title_id));
    auto partial = index;
    partial.revision = 2;
    partial.etag = "\"" + std::string(64, 'b') + "\"";
    partial.titles.clear();
    partial.titles[title_id] = true;
    discovery.Remember(partial);
    assert(discovery.HasTitle(0x0100000000000001ULL));
    auto removal = partial;
    removal.revision = 3;
    removal.etag = "\"" + std::string(64, 'c') + "\"";
    removal.titles[0x0100000000000001ULL] = false;
    discovery.Remember(removal);
    assert(!discovery.HasTitle(0x0100000000000001ULL));
    assert(discovery.HasTitle(title_id));
    bool rollback_rejected = false;
    try {
        discovery.Remember(index);
    } catch (const std::exception&) {
        rollback_rejected = true;
    }
    assert(rollback_rejected);
    bool conflict_rejected = false;
    auto conflict = removal;
    conflict.etag = "\"" + std::string(64, 'd') + "\"";
    try {
        discovery.Remember(conflict);
    } catch (const std::exception&) {
        conflict_rejected = true;
    }
    assert(conflict_rejected);
    // A new process reconstructs discovery from signed history, even after expiration.
    TitleIndexState restarted;
    restarted.Remember(VerifyTitleIndex(sign(index_doc), public_key, 3600, true));
    assert(restarted.HasTitle(0x0100000000000001ULL));
    auto index_rejected = [&](const std::string& body, std::int64_t now = 1) {
        bool failed = false;
        try {
            VerifyTitleIndex(body, public_key, now);
        } catch (const std::exception&) {
            failed = true;
        }
        assert(failed);
    };
    index_rejected(sign(index_doc), 3600);
    index_rejected(valid);     // A patch document cannot be substituted for an index.
    rejected(sign(index_doc)); // Nor can an index be treated as a patch.
    for (const std::string kind :
         {"signature", "duplicate", "title", "enabled", "tag", "revision", "schema"}) {
        auto bad = index_doc;
        if (kind == "signature") {
            auto envelope = json::parse(sign(bad));
            envelope["signature"] = std::string(88, 'A');
            index_rejected(envelope.dump());
            continue;
        }
        if (kind == "duplicate")
            bad["titles"].push_back(bad["titles"][0]);
        if (kind == "title")
            bad["titles"][0]["title_id"] = "0000000000000000";
        if (kind == "enabled")
            bad["titles"][0]["enabled"] = "yes";
        if (kind == "tag")
            bad["index_etag"] = "unsigned-tag";
        if (kind == "revision")
            bad["revision"] = 0;
        if (kind == "schema")
            bad["schema_version"] = 2;
        index_rejected(sign(bad));
    }

    std::cout << "Signed NPLN manifests, guards, retained patches and session snapshots passed.\n";
}
