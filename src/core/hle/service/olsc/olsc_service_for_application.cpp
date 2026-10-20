// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <vector>

#include "core/core.h"
#include "core/hle/kernel/k_transfer_memory.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/olsc/nextendo_island.h"
#include "core/hle/service/olsc/olsc_async_result.h"
#include "core/hle/service/olsc/olsc_results.h"
#include "core/hle/service/olsc/olsc_service_for_application.h"

namespace Service::OLSC {

namespace Island = NextendoIsland;

namespace olsc_app_detail {

Result ToResult(Island::Outcome outcome) {
    switch (outcome) {
    case Island::Outcome::Ok:
    case Island::Outcome::Unchanged:
        return ResultSuccess;
    case Island::Outcome::Unavailable:
        return ResultNextendoUnavailable;
    case Island::Outcome::Refused:
        return ResultNextendoServerRefused;
    case Island::Outcome::NoLocalSave:
        return ResultNextendoNoLocalSave;
    case Island::Outcome::InvalidIsland:
        return ResultNextendoInvalidIsland;
    default:
        return ResultNextendoNetworkError;
    }
}

bool IsNullUid(const NS::Uid& uid) {
    return uid.uuid.IsInvalid();
}

} // namespace olsc_app_detail

using namespace olsc_app_detail;

IOlscServiceForApplication::IOlscServiceForApplication(Core::System& system_)
    : ServiceFramework{system_, "olsc:u"} {
    // clang-format off
        static const FunctionInfo functions[] = {
            {0, D<&IOlscServiceForApplication::Initialize>, "Initialize"},
            {10, D<&IOlscServiceForApplication::VerifySaveDataBackupLicenseAsync>, "VerifySaveDataBackupLicenseAsync"},
            {13, D<&IOlscServiceForApplication::GetSaveDataBackupSetting>, "GetSaveDataBackupSetting"},
            {14, D<&IOlscServiceForApplication::SetSaveDataBackupSettingEnabled>, "SetSaveDataBackupSettingEnabled"},
            {15, D<&IOlscServiceForApplication::SetCustomData>, "SetCustomData"},
            {16, nullptr, "DeleteSaveDataBackupSetting"},
            {18, D<&IOlscServiceForApplication::GetSaveDataBackupInfoCache>, "GetSaveDataBackupInfoCache"},
            {19, D<&IOlscServiceForApplication::UpdateSaveDataBackupInfoCacheAsync>, "UpdateSaveDataBackupInfoCacheAsync"},
            {22, nullptr, "DeleteSaveDataBackupAsync"},
            {25, D<&IOlscServiceForApplication::ListDownloadableSaveDataBackupInfoAsync>, "ListDownloadableSaveDataBackupInfoAsync"},
            {26, D<&IOlscServiceForApplication::DownloadSaveDataBackupAsync>, "DownloadSaveDataBackupAsync"},
            {27, D<&IOlscServiceForApplication::UploadSaveDataBackupAsync>, "UploadSaveDataBackupAsync"},
            {9010, nullptr, "VerifySaveDataBackupLicenseAsyncForDebug"},
            {9013, nullptr, "GetSaveDataBackupSettingForDebug"},
            {9014, nullptr, "SetSaveDataBackupSettingEnabledForDebug"},
            {9015, nullptr, "SetCustomDataForDebug"},
            {9016, nullptr, "DeleteSaveDataBackupSettingForDebug"},
            {9018, nullptr, "GetSaveDataBackupInfoCacheForDebug"},
            {9019, nullptr, "UpdateSaveDataBackupInfoCacheAsyncForDebug"},
            {9022, nullptr, "DeleteSaveDataBackupAsyncForDebug"},
            {9025, nullptr, "ListDownloadableSaveDataBackupInfoAsyncForDebug"},
            {9026, nullptr, "DownloadSaveDataBackupAsyncForDebug"},
        };
    // clang-format on

    RegisterHandlers(functions);
}

IOlscServiceForApplication::~IOlscServiceForApplication() = default;

u64 IOlscServiceForApplication::TitleId() const {
    return system.GetApplicationProcessProgramID();
}

std::string IOlscServiceForApplication::KeyFor(const NS::Uid& account_id) const {
    const u64 title_id = TitleId();
    std::string key = Island::Key(title_id, account_id.uuid.RawString());
    if (Island::IsStrict(title_id)) {
        Island::NoteUser(title_id, key);
    }
    return key;
}

Result IOlscServiceForApplication::Initialize(ClientProcessId process_id) {
    LOG_INFO(Service_OLSC, "called");
    initialized = true;
    R_SUCCEED();
}

Result IOlscServiceForApplication::VerifySaveDataBackupLicenseAsync(
    Out<SharedPointer<IAsyncResult>> out_async, NS::Uid account_id) {
    R_UNLESS(initialized, ResultNotInitialized);
    const u64 title_id = TitleId();
    LOG_INFO(Service_OLSC, "[Nextendo] olsc: VerifySaveDataBackupLicenseAsync {}",
             account_id.uuid.FormattedString());

    // At Nintendo: the subscription. Here: a usable Nextendo account AND a server that archives
    // this game (until it does, backup cannot be enabled, instead of failing at every save).
    *out_async = std::make_shared<IAsyncResult>(system, [title_id]() -> Result {
        if (!Island::IsStrict(title_id)) {
            return ResultNextendoUnavailable;
        }
        return ToResult(Island::GetMeta(title_id).first);
    });
    R_SUCCEED();
}

Result IOlscServiceForApplication::GetSaveDataBackupSetting(Out<u8> out_save_data_backup_setting,
                                                            NS::Uid account_id) {
    if (!Island::IsStrict(TitleId())) {
        LOG_WARNING(Service_OLSC, "(STUBBED) called");
        *out_save_data_backup_setting = 0;
        R_SUCCEED();
    }
    R_UNLESS(initialized, ResultNotInitialized);
    R_UNLESS(!IsNullUid(account_id), ResultNullArgument);
    // ACNH only compares with 1 (enabled): 0x5b7b00 and its 9 callers.
    *out_save_data_backup_setting = Island::Get(KeyFor(account_id)).setting;
    R_SUCCEED();
}

Result IOlscServiceForApplication::SetSaveDataBackupSettingEnabled(bool enabled,
                                                                   NS::Uid account_id) {
    if (!Island::IsStrict(TitleId())) {
        LOG_WARNING(Service_OLSC, "(STUBBED) called, enabled={}, account_id={}", enabled,
                    account_id.uuid.FormattedString());
        R_SUCCEED();
    }
    R_UNLESS(initialized, ResultNotInitialized);
    R_UNLESS(!IsNullUid(account_id), ResultNullArgument);
    Island::Update(KeyFor(account_id), [enabled](Island::OlscEntry& e) {
        e.setting = static_cast<u8>(enabled ? 1 : 2);
    });
    LOG_INFO(Service_OLSC, "[Nextendo] olsc: SetSaveDataBackupSettingEnabled {} {}",
             account_id.uuid.FormattedString(), enabled);
    R_SUCCEED();
}

Result IOlscServiceForApplication::SetCustomData(NS::Uid account_id,
                                                 InBuffer<BufferAttr_HipcMapAlias> custom_data) {
    R_UNLESS(initialized, ResultNotInitialized);
    if (!Island::IsStrict(TitleId())) {
        LOG_WARNING(Service_OLSC, "(STUBBED) called, size={}", custom_data.size());
        R_SUCCEED();
    }
    R_UNLESS(!IsNullUid(account_id), ResultNullArgument);
    const size_t size = std::min(custom_data.size(), Island::CustomDataSize);
    const std::string b64 = Island::Base64Encode(custom_data.subspan(0, size));
    Island::Update(KeyFor(account_id), [&b64](Island::OlscEntry& e) { e.custom = b64; });
    R_SUCCEED();
}

Result IOlscServiceForApplication::GetSaveDataBackupInfoCache(
    NS::Uid account_id, OutLargeData<std::array<u8, 0x410>, BufferAttr_HipcMapAlias> out_info) {
    R_UNLESS(initialized, ResultNotInitialized);
    if (!Island::IsStrict(TitleId())) {
        R_THROW(ResultSaveDataBackupInfoCacheNotFound);
    }
    const Island::OlscEntry entry = Island::Get(KeyFor(account_id));
    if (!entry.has_cache) {
        R_THROW(ResultSaveDataBackupInfoCacheNotFound);
    }
    std::vector<u8> custom = Island::Base64Decode(entry.cache_custom).value_or(std::vector<u8>{});
    if (custom.size() > Island::CustomDataSize) {
        custom.clear();
    }
    Island::WriteBackupInfo(*out_info, entry.cache_id, entry.cache_time, custom);
    R_SUCCEED();
}

Result IOlscServiceForApplication::UpdateSaveDataBackupInfoCacheAsync(
    Out<SharedPointer<IAsyncResult>> out_async, NS::Uid account_id) {
    R_UNLESS(initialized, ResultNotInitialized);
    const u64 title_id = TitleId();
    const std::string key = KeyFor(account_id);
    *out_async = std::make_shared<IAsyncResult>(system, [title_id, key]() -> Result {
        if (!Island::IsStrict(title_id)) {
            return ResultNextendoUnavailable;
        }
        const auto [rc, island] = Island::GetMeta(title_id);
        if (rc == Island::Outcome::Ok) {
            Island::StoreCache(key, island);
        }
        return ToResult(rc);
    });
    R_SUCCEED();
}

Result IOlscServiceForApplication::ListDownloadableSaveDataBackupInfoAsync(
    Out<SharedPointer<IAsyncListDownloadableSaveDataBackupInfo>> out_async, u32 work_size,
    NS::Uid account_id, InCopyHandle<Kernel::KTransferMemory> work_memory) {
    // The game destroys its transfer memory (0x2000 in ACNH) as soon as the call returns: it is
    // not used here.
    (void)work_size;
    (void)work_memory;
    R_UNLESS(initialized, ResultNotInitialized);
    const u64 title_id = TitleId();
    const std::string key = KeyFor(account_id);
    *out_async = std::make_shared<IAsyncListDownloadableSaveDataBackupInfo>(
        system, [title_id, key](std::vector<SaveDataBackupInfo>& entries) -> Result {
            if (!Island::IsStrict(title_id)) {
                return ResultNextendoUnavailable;
            }
            const auto [rc, island] = Island::GetMeta(title_id);
            if (rc != Island::Outcome::Ok) {
                return ToResult(rc);
            }
            Island::StoreCache(key, island);
            if (Island::IsRestorable(island)) {
                SaveDataBackupInfo info{};
                Island::WriteBackupInfo(info, island.rev, island.posix_time, island.custom);
                entries.push_back(info);
            }
            LOG_INFO(Service_OLSC, "[Nextendo] olsc: {} island(s) to restore", entries.size());
            return ResultSuccess;
        });
    R_SUCCEED();
}

Result IOlscServiceForApplication::DownloadSaveDataBackupAsync(
    Out<SharedPointer<IAsyncTransferSaveDataBackup>> out_async, NS::Uid account_id,
    u64 backup_id) {
    R_UNLESS(initialized, ResultNotInitialized);
    const u64 title_id = TitleId();
    const std::string key = KeyFor(account_id);
    LOG_INFO(Service_OLSC, "[Nextendo] olsc: DownloadSaveDataBackupAsync {} id={}",
             account_id.uuid.FormattedString(), backup_id);
    *out_async =
        std::make_shared<IAsyncTransferSaveDataBackup>(system, [title_id, key]() -> Result {
            if (!Island::IsStrict(title_id)) {
                return ResultNextendoUnavailable;
            }
            return ToResult(Island::Download(title_id, Island::FindDeviceSaveDir(title_id), key));
        });
    R_SUCCEED();
}

Result IOlscServiceForApplication::UploadSaveDataBackupAsync(
    Out<SharedPointer<IAsyncTransferSaveDataBackup>> out_async, NS::Uid account_id) {
    R_UNLESS(initialized, ResultNotInitialized);
    const u64 title_id = TitleId();
    const std::string key = KeyFor(account_id);
    LOG_INFO(Service_OLSC, "[Nextendo] olsc: UploadSaveDataBackupAsync {}",
             account_id.uuid.FormattedString());
    *out_async =
        std::make_shared<IAsyncTransferSaveDataBackup>(system, [title_id, key]() -> Result {
            if (!Island::IsStrict(title_id)) {
                return ResultNextendoUnavailable;
            }
            return ToResult(
                Island::Upload(title_id, Island::FindDeviceSaveDir(title_id), key, false));
        });
    R_SUCCEED();
}

} // namespace Service::OLSC
