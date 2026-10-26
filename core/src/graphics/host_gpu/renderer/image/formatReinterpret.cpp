// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#include "graphics/host_gpu/renderer/image/formatReinterpret.h"

#include <mutex>
#include <unordered_set>

namespace Libs::Graphics::FormatReinterpret {

namespace {

// Guest-memory bucket granularity. Render targets are large (hundreds of KB to tens of MB),
// so 64 KB buckets keep the set small while pinning the mark precisely enough that a
// reinterpreted target does not flag an unrelated neighbour.
constexpr uint64_t BUCKET_SHIFT = 16;
constexpr uint64_t BUCKET_SIZE  = 1ull << BUCKET_SHIFT;

std::mutex          g_mutex;
std::unordered_set<uint64_t> g_marked_buckets;

[[nodiscard]] uint64_t BucketOf(uint64_t address) {
        return address >> BUCKET_SHIFT;
}

} // namespace

void Mark(uint64_t address, uint64_t size) {
        if (size == 0) {
                return;
        }
        const auto first = BucketOf(address);
        const auto last  = BucketOf(address + size - 1);
        std::lock_guard lock(g_mutex);
        for (auto bucket = first;; bucket++) {
                g_marked_buckets.insert(bucket);
                if (bucket == last) {
                        break;
                }
        }
}

bool IsMarked(uint64_t address, uint64_t size) {
        if (size == 0) {
                return false;
        }
        const auto first = BucketOf(address);
        const auto last  = BucketOf(address + size - 1);
        std::lock_guard lock(g_mutex);
        for (auto bucket = first;; bucket++) {
                if (g_marked_buckets.contains(bucket)) {
                        return true;
                }
                if (bucket == last) {
                        break;
                }
        }
        return false;
}

} // namespace Libs::Graphics::FormatReinterpret
