// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <memory>

#include "common/common_types.h"
#include "core/device_memory_manager.h"

namespace VideoCommon {

/// Grow-only lock-free 64 KiB map of device memory that may hold GPU-written data.
class GpuWrittenRanges {
public:
    void Mark(DAddr addr, u64 size) noexcept {
        if (size == 0) {
            return;
        }
        // Grow-only: a plain load skips the locked RMW, since render targets are re-marked every draw.
        const u64 last = Chunk(addr + size - 1);
        for (u64 chunk = Chunk(addr); chunk <= last;) {
            const u64 word = chunk >> 6;
            const u64 word_last = std::min(last, word * 64 + 63);
            const u64 bits = (~u64{0} >> (63 - (word_last & 63))) & (~u64{0} << (chunk & 63));
            if ((words[word].load(std::memory_order_relaxed) & bits) != bits) {
                words[word].fetch_or(bits, std::memory_order_release);
            }
            chunk = word_last + 1;
        }
    }

    /// For writes whose device range is not known (e.g. sparse images): disables the filter.
    void MarkAll() noexcept {
        everything.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool MayContain(DAddr addr, u64 size) const noexcept {
        if (everything.load(std::memory_order_acquire)) {
            return true;
        }
        if (size == 0) {
            return false;
        }
        const u64 last = Chunk(addr + size - 1);
        for (u64 chunk = Chunk(addr); chunk <= last; ++chunk) {
            if (words[chunk >> 6].load(std::memory_order_acquire) & (u64{1} << (chunk & 63))) {
                return true;
            }
        }
        return false;
    }

private:
    static constexpr size_t CHUNK_BITS = 16;
    static constexpr size_t ADDRESS_BITS = 39; // MaxwellDeviceTraits::device_virtual_bits
    static constexpr u64 NUM_CHUNKS = u64{1} << (ADDRESS_BITS - CHUNK_BITS);

    static constexpr u64 Chunk(DAddr addr) noexcept {
        return (addr >> CHUNK_BITS) & (NUM_CHUNKS - 1);
    }

    std::unique_ptr<std::atomic<u64>[]> words{new std::atomic<u64>[NUM_CHUNKS / 64]{}};
    std::atomic<bool> everything{false};
};

} // namespace VideoCommon
