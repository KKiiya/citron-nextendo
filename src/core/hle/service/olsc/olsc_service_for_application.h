// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/ns/ns_types.h"
#include "core/hle/service/service.h"

namespace Kernel {
class KTransferMemory;
}

namespace Service::OLSC {

class IAsyncResult;
class IAsyncListDownloadableSaveDataBackupInfo;
class IAsyncTransferSaveDataBackup;

// [Nextendo] Commands and types come from Animal Crossing: New Horizons 3.0.3's client (proxy
// 0x47f9090, wrappers 0x5b7a60..0x5b7ed0, requests 0x5b9828..0x5bb004): they make the island
// backup work from the game's menu (minus button on the title screen). Games without a Nextendo
// backup keep the previous behaviour.
class IOlscServiceForApplication final : public ServiceFramework<IOlscServiceForApplication> {
public:
    explicit IOlscServiceForApplication(Core::System& system_);
    ~IOlscServiceForApplication() override;

private:
    Result Initialize(ClientProcessId process_id);
    Result VerifySaveDataBackupLicenseAsync(Out<SharedPointer<IAsyncResult>> out_async,
                                            NS::Uid account_id);
    Result GetSaveDataBackupSetting(Out<u8> out_save_data_backup_setting, NS::Uid account_id);
    Result SetSaveDataBackupSettingEnabled(bool enabled, NS::Uid account_id);
    Result SetCustomData(NS::Uid account_id, InBuffer<BufferAttr_HipcMapAlias> custom_data);
    Result GetSaveDataBackupInfoCache(
        NS::Uid account_id,
        OutLargeData<std::array<u8, 0x410>, BufferAttr_HipcMapAlias> out_info);
    Result UpdateSaveDataBackupInfoCacheAsync(Out<SharedPointer<IAsyncResult>> out_async,
                                              NS::Uid account_id);
    Result ListDownloadableSaveDataBackupInfoAsync(
        Out<SharedPointer<IAsyncListDownloadableSaveDataBackupInfo>> out_async, u32 work_size,
        NS::Uid account_id, InCopyHandle<Kernel::KTransferMemory> work_memory);
    Result DownloadSaveDataBackupAsync(Out<SharedPointer<IAsyncTransferSaveDataBackup>> out_async,
                                       NS::Uid account_id, u64 backup_id);
    Result UploadSaveDataBackupAsync(Out<SharedPointer<IAsyncTransferSaveDataBackup>> out_async,
                                     NS::Uid account_id);

    u64 TitleId() const;
    std::string KeyFor(const NS::Uid& account_id) const;

    bool initialized{};
};

} // namespace Service::OLSC
