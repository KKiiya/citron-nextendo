// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cassert>
#include <string>
#include <utility>
#include <vector>

#include "core/file_sys/ips_layer.h"
#include "core/loader/nextendo_s3_patches.h"
#include "web_service/npln_patch_catalog.h"

namespace {
std::size_t writes{};
WebService::NplnPatchCatalog::Lookup selected;
constexpr u64 SelectedTitle = 0x0100965017338000ULL;
const std::string SelectedBuild =
    "D70CE1726C979E45ED4200E9C2B23B2A258FDC18000000000000000000000000";

std::array<u8, 32> Build(std::string hex) {
    hex.resize(64, '0');
    std::array<u8, 32> result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i] = static_cast<u8>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
    }
    return result;
}
} // namespace

namespace FileSys {
// Count writes at the IPS boundary. This test verifies loader selection, not IPS decoding.
VirtualFile PatchIPS(const VirtualFile& file, const VirtualFile&) {
    ++writes;
    return file;
}
} // namespace FileSys

namespace WebService::NplnPatchCatalog {
Lookup Find(std::uint64_t title, std::string_view build, std::string_view module,
            std::span<const std::uint8_t>) {
    return title == SelectedTitle && build == SelectedBuild && module == "main" ? selected
                                                                                : Lookup{};
}
} // namespace WebService::NplnPatchCatalog

int main() {
    const std::vector<u8> original(512, 0x55);
    const std::vector<std::pair<u64, std::string>> targets{
        {0x0100C2500FC20000ULL, "6830B3A12406CB4716FEC5ADDC35D3E2DC92D212"},
        {0x0100C2500FC20000ULL, "726D2B882DD9EF10F4A9D73EED088740630FB6C8"},
        {0x0100C2500FC20000ULL, "28C4287AEE36F7499DA60F3E68B54C70DA382D75"},
        {0x010015100B514000ULL, "FF773E90972D544EB79406EAA65396D53C43EFB9"},
        {0x0100C9A00ECE6000ULL, "44EFA84EE9C32B466EA27215D3D91DFD0EBAE625"},
        {0x0100C6F01C4F8000ULL, "BCA1A793E41A4836EF3C03286B18E816EEC62338"},
        {0x010099801C4FA000ULL, "BCA1A793E41A4836EF3C03286B18E816EEC62338"},
        {0x01006FD0080B2000ULL, "616640F27B9362502D1CE10BF01EB9A6"},
        {0x01008F6008C5E000ULL, "709BFD66115298640155FCC4979DBA151C7CC79A"},
        {0x0100A3D008C5C000ULL, "421C5411B487EB4D049DD065FEC9547773E8E598"},
        {0x0100F43008C44000ULL, "B1F12FD919EAE86AB8A978317677E64BCE443D1F"},
        {0x0100453019AA8000ULL, "EC4ED46BBC4A9EBBE7CE0AF7A4A8EBBC09E5724A"},
        {0x0100B04011742000ULL, "C9A3DD7702075ECD90101F45553895822804D38F"},
        {SelectedTitle, SelectedBuild},
        {0x0100E65002BB8000ULL, "E7F845093E8CBC68DACF011CCB620D6667B5A20B"},
    };
    for (const auto& [title, build] : targets) {
        writes = 0;
        assert(Loader::NextendoS3Patches::ApplyIfMatch(Build(build), original, "main", title) ==
               original);
        assert(writes == 0); // Missing verified data must never activate legacy NPLN bytes.
    }
    const auto ctr = Build("1C689518406930512C13DDF4217E7676");
    writes = 0;
    Loader::NextendoS3Patches::ApplyIfMatch(ctr, original, "main", 0x0100F9F00C696000ULL);
    assert(writes == 2);
    writes = 0;
    Loader::NextendoS3Patches::ApplyIfMatch(ctr, original, "sdk", 0x0100F9F00C696000ULL);
    Loader::NextendoS3Patches::ApplyIfMatch(ctr, original, "main", SelectedTitle);
    assert(writes == 0);
#ifdef ENABLE_WEB_SERVICE
    selected = {true, "signed-fixture", 1, {1}};
    Loader::NextendoS3Patches::ApplyIfMatch(Build(SelectedBuild), original, "main", SelectedTitle);
    assert(writes == 1);
    writes = 0;
    // An authoritative withdrawal or guard rejection supplies no patch bytes.
    selected.ips.clear();
    assert(Loader::NextendoS3Patches::ApplyIfMatch(Build(SelectedBuild), original, "main",
                                                   SelectedTitle) == original);
    assert(writes == 0);
#endif
}
