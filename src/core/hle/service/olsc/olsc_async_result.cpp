// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>

#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/olsc/olsc_async_result.h"
#include "core/hle/service/olsc/olsc_results.h"

// [UNITY-FIX] undef Win32 macros shadowing ServiceContext methods.
#undef CreateEvent
#undef CreateMutex
#undef CreateSemaphore

namespace Service::OLSC {

AsyncOperation::AsyncOperation(Core::System& system, std::function<Result()> work)
    : service_context{system, "olsc:nextendo"}, event{service_context},
      result{ResultNextendoNotDone} {
    worker = system.Kernel().RunOnHostCoreThread("olsc:nextendo", [this, fn = std::move(work)] {
        Result rc = ResultNextendoNetworkError;
        try {
            rc = fn();
        } catch (const std::exception& e) {
            LOG_WARNING(Service_OLSC, "[Nextendo] olsc: {}", e.what());
        }
        result = rc;
        done.store(true);
        if (!closing.load()) {
            event.Signal();
        }
    });
}

AsyncOperation::~AsyncOperation() {
    closing.store(true);
    if (worker.joinable()) {
        worker.join();
    }
}

Result AsyncOperation::Get() const {
    return done.load() ? result : ResultNextendoNotDone;
}

bool AsyncOperation::HasDone() const {
    return done.load();
}

Kernel::KReadableEvent* AsyncOperation::GetEvent() {
    return event.GetHandle();
}

// ---- IAsyncResult ------------------------------------------------------------------------------

IAsyncResult::IAsyncResult(Core::System& system_, std::function<Result()> work)
    : ServiceFramework{system_, "IAsyncResult"}, operation{system_, std::move(work)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IAsyncResult::Get>, "Get"},
        {1, D<&IAsyncResult::Cancel>, "Cancel"},
        {2, D<&IAsyncResult::HasDone>, "HasDone"},
        {3, D<&IAsyncResult::GetSystemEvent>, "GetSystemEvent"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

IAsyncResult::~IAsyncResult() = default;

Result IAsyncResult::Get() {
    R_RETURN(operation.Get());
}

Result IAsyncResult::Cancel() {
    R_SUCCEED();
}

Result IAsyncResult::HasDone(Out<bool> out_done) {
    *out_done = operation.HasDone();
    R_SUCCEED();
}

Result IAsyncResult::GetSystemEvent(OutCopyHandle<Kernel::KReadableEvent> out_event) {
    *out_event = operation.GetEvent();
    R_SUCCEED();
}

// ---- IAsyncListDownloadableSaveDataBackupInfo --------------------------------------------------

IAsyncListDownloadableSaveDataBackupInfo::IAsyncListDownloadableSaveDataBackupInfo(
    Core::System& system_, std::function<Result(std::vector<SaveDataBackupInfo>&)> work)
    : ServiceFramework{system_, "IAsyncListDownloadableSaveDataBackupInfo"},
      entries{std::make_shared<std::vector<SaveDataBackupInfo>>()},
      operation{system_, [list = entries, fn = std::move(work)] { return fn(*list); }} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IAsyncListDownloadableSaveDataBackupInfo::Get>, "Get"},
        {1, D<&IAsyncListDownloadableSaveDataBackupInfo::Cancel>, "Cancel"},
        {2, D<&IAsyncListDownloadableSaveDataBackupInfo::HasDone>, "HasDone"},
        {3, D<&IAsyncListDownloadableSaveDataBackupInfo::GetSystemEvent>, "GetSystemEvent"},
        {100, D<&IAsyncListDownloadableSaveDataBackupInfo::GetCount>, "GetCount"},
        {101, D<&IAsyncListDownloadableSaveDataBackupInfo::GetList>, "GetList"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

IAsyncListDownloadableSaveDataBackupInfo::~IAsyncListDownloadableSaveDataBackupInfo() = default;

Result IAsyncListDownloadableSaveDataBackupInfo::Get() {
    R_RETURN(operation.Get());
}

Result IAsyncListDownloadableSaveDataBackupInfo::Cancel() {
    R_SUCCEED();
}

Result IAsyncListDownloadableSaveDataBackupInfo::HasDone(Out<bool> out_done) {
    *out_done = operation.HasDone();
    R_SUCCEED();
}

Result IAsyncListDownloadableSaveDataBackupInfo::GetSystemEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    *out_event = operation.GetEvent();
    R_SUCCEED();
}

Result IAsyncListDownloadableSaveDataBackupInfo::GetCount(Out<u32> out_count) {
    *out_count = operation.HasDone() ? static_cast<u32>(entries->size()) : 0;
    R_SUCCEED();
}

Result IAsyncListDownloadableSaveDataBackupInfo::GetList(
    Out<u32> out_written, u32 offset,
    OutArray<SaveDataBackupInfo, BufferAttr_HipcMapAlias> out_entries) {
    u32 written = 0;
    if (operation.HasDone()) {
        for (size_t i = offset; i < entries->size() && written < out_entries.size(); ++i) {
            out_entries[written++] = (*entries)[i];
        }
    }
    *out_written = written;
    R_SUCCEED();
}

// ---- IAsyncTransferSaveDataBackup ---------------------------------------------------------------

IAsyncTransferSaveDataBackup::IAsyncTransferSaveDataBackup(Core::System& system_,
                                                           std::function<Result()> work)
    : ServiceFramework{system_, "IAsyncTransferSaveDataBackup"},
      operation{system_, std::move(work)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IAsyncTransferSaveDataBackup::Get>, "Get"},
        {1, D<&IAsyncTransferSaveDataBackup::Cancel>, "Cancel"},
        {2, D<&IAsyncTransferSaveDataBackup::HasDone>, "HasDone"},
        {3, D<&IAsyncTransferSaveDataBackup::GetSystemEvent>, "GetSystemEvent"},
        {100, D<&IAsyncTransferSaveDataBackup::GetProgress>, "GetProgress"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

IAsyncTransferSaveDataBackup::~IAsyncTransferSaveDataBackup() = default;

Result IAsyncTransferSaveDataBackup::Get() {
    R_RETURN(operation.Get());
}

Result IAsyncTransferSaveDataBackup::Cancel() {
    R_SUCCEED();
}

Result IAsyncTransferSaveDataBackup::HasDone(Out<bool> out_done) {
    *out_done = operation.HasDone();
    R_SUCCEED();
}

Result IAsyncTransferSaveDataBackup::GetSystemEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    *out_event = operation.GetEvent();
    R_SUCCEED();
}

Result IAsyncTransferSaveDataBackup::GetProgress(Out<std::array<u8, 0x30>> out_progress) {
    *out_progress = {};
    R_SUCCEED();
}

} // namespace Service::OLSC
