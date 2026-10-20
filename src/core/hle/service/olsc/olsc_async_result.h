// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/os/event.h"
#include "core/hle/service/service.h"

namespace Kernel {
class KReadableEvent;
}

namespace Service::OLSC {

// [Nextendo] Asynchronous olsc operation, run on its own host thread. Its result and completion
// event back the three objects below.
class AsyncOperation {
public:
    AsyncOperation(Core::System& system, std::function<Result()> work);
    ~AsyncOperation();

    Result Get() const;
    bool HasDone() const;
    Kernel::KReadableEvent* GetEvent();

private:
    KernelHelpers::ServiceContext service_context;
    Event event;
    std::atomic<bool> done{false};
    std::atomic<bool> closing{false};
    Result result;
    std::jthread worker;
};

// Commands read in ACNH 3.0.3's client (vtables 0x47f9208, 0x47f9328, 0x47f9470): 0 Get (the
// operation's result), 1 Cancel, 2 HasDone -> b8, 3 GetSystemEvent -> handle<copy>. The game waits
// for the event, calls HasDone then Get (0x5bb398).
class IAsyncResult final : public ServiceFramework<IAsyncResult> {
public:
    IAsyncResult(Core::System& system_, std::function<Result()> work);
    ~IAsyncResult() override;

private:
    Result Get();
    Result Cancel();
    Result HasDone(Out<bool> out_done);
    Result GetSystemEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);

    AsyncOperation operation;
};

using SaveDataBackupInfo = std::array<u8, 0x410>;

// Result of ListDownloadableSaveDataBackupInfoAsync (cmd 25), read in ACNH 3.0.3 (0x5bacd8,
// 0x5bae14, caller 0x25cce10): 100 -> u32 item count; 101 (u32 start index, output buffer of
// 0x410-byte items) -> u32 written. The game reads at most 5 items, one by one.
class IAsyncListDownloadableSaveDataBackupInfo final
    : public ServiceFramework<IAsyncListDownloadableSaveDataBackupInfo> {
public:
    IAsyncListDownloadableSaveDataBackupInfo(
        Core::System& system_, std::function<Result(std::vector<SaveDataBackupInfo>&)> work);
    ~IAsyncListDownloadableSaveDataBackupInfo() override;

private:
    Result Get();
    Result Cancel();
    Result HasDone(Out<bool> out_done);
    Result GetSystemEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result GetCount(Out<u32> out_count);
    Result GetList(Out<u32> out_written, u32 offset,
                   OutArray<SaveDataBackupInfo, BufferAttr_HipcMapAlias> out_entries);

    std::shared_ptr<std::vector<SaveDataBackupInfo>> entries;
    AsyncOperation operation;
};

// Result of DownloadSaveDataBackupAsync (cmd 26) and UploadSaveDataBackupAsync (cmd 27): command
// 100 returns 0x30 bytes (0x5bb180). ACNH does not call it; the operation ends in one go, so the
// progress is zero.
class IAsyncTransferSaveDataBackup final : public ServiceFramework<IAsyncTransferSaveDataBackup> {
public:
    IAsyncTransferSaveDataBackup(Core::System& system_, std::function<Result()> work);
    ~IAsyncTransferSaveDataBackup() override;

private:
    Result Get();
    Result Cancel();
    Result HasDone(Out<bool> out_done);
    Result GetSystemEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result GetProgress(Out<std::array<u8, 0x30>> out_progress);

    AsyncOperation operation;
};

} // namespace Service::OLSC
