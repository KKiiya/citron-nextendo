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
    auto tampered = json::parse(valid);
    tampered["signature"] = std::string(88, 'A');
    rejected(tampered.dump());

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

    std::cout << "Signed NPLN patch manifest and original-byte guard tests passed.\n";
}
