// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#include "common/common.h"

#if KYTY_PLATFORM != KYTY_PLATFORM_LINUX
// Huge-page engine: Linux only; every entry point is a no-op elsewhere.
#else

#include "common/hugePages.h"

#include "common/contentProfile.h"
#include "common/debugFlags.h"
#include "common/enginePolicy.h"
#include "common/gpuProfile.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

#include <pthread.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#ifndef MADV_COLLAPSE
// Kernel 6.1+ (bionic headers of older NDKs do not define it yet).
#define MADV_COLLAPSE 25
#endif

namespace Common::HugePages {

namespace {

constexpr uint64_t HugePageBytes       = 2ull * 1024 * 1024;
constexpr uint64_t HugePageSubPages    = HugePageBytes / 4096; // 512
constexpr uint64_t TrackMinBytes       = 2ull * 1024 * 1024;   // track ranges >= 2 MiB
constexpr uint64_t CommitNotifyBytes   = 64ull * 1024;         // PS5 page grain
constexpr uint64_t ReservationAdviceBytes = 64ull * 1024 * 1024; // advise big reservations only
constexpr size_t   MaxTrackedRanges    = 16384;
constexpr uint32_t ActiveSweepMs       = 2000;  // sweep every 2 s while things happen
constexpr uint32_t IdleSweepMs         = 32000; // back off to 32 s when idle
constexpr uint32_t SweeperNice         = 8;     // background, never steals game time

// v0.0.9proQ4 round 3: profile 500 halves the active sweep cadence (1 s):
// the matched content class commits hundreds of MiB in its first minutes,
// and reaching full 2 MiB-page coverage twice as fast compounds the TLB
// win for the rest of the session. The sweeper stays nice-8 background
// work either way.
uint32_t ActiveSweepIntervalMs() {
        // v0.0.9proQ4 round 4: the compositing family shares the fast
        // cadence - its guest heap is as hot as any, and the TLB win
        // compounds the same way for a CPU-bound frame loop.
        // v0.0.9proQ4 round 5: the cadence ladder lives in the vendor-aware
        // table (common/enginePolicy.h). Under the classes Mali sweeps every
        // 750 ms instead of 1 s: on a UMA Mali SoC the MMU walks contend with
        // the GPU for the same DRAM bus, so reaching full 2 MiB-page coverage
        // sooner compounds the TLB win for the whole streaming period; the
        // sweeper stays nice-8 background work either way.
        return Common::EnginePolicy::TuningFor(
                   Common::GpuProfile::MaliProfileEnabled(),
                   Common::GpuProfile::AdrenoProfileEnabled(),
                   Common::EnginePolicy::CurrentTier())
            .thp_active_sweep_ms;
}

enum class ThpMode : int32_t {
        Off     = 0,
        Sweeper = 1, // Android default
        Advice  = 2, // Q2 behaviour, desktop default
};

ThpMode CurrentMode() {
        // v0.0.9proQ4 round 2: 1 = sweeper (default on Android), 2 = Q2 advice (default on
        // desktop), 0 = off. See hugePages.h for the full rationale.
#if defined(__ANDROID__)
        static Common::DebugFlags::CachedValue mode("debug.capy.thp", 1);
#else
        static Common::DebugFlags::CachedValue mode("debug.capy.thp", 2);
#endif
        const auto value = static_cast<int32_t>(mode.Get());
        if (value <= 0) {
                return ThpMode::Off;
        }
        return value == 1 ? ThpMode::Sweeper : ThpMode::Advice;
}

void ThpLogInfo(const char* text, uint64_t a = 0, uint64_t b = 0) {
        (void)text;
        (void)a;
        (void)b;
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_INFO, "CapyPS5-THP", "%s (%llu / %llu)", text,
                            static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
#else
        (void)0;
#endif
}

// ---------------------------------------------------------------- collapse canary

// 0 = not probed yet, 1 = MADV_COLLAPSE works, -1 = unsupported (pre-6.1 kernel).
// Probed exactly once on the sweeper thread so the probe cost (a couple of
// milliseconds of page touching) never lands on a game thread.
std::atomic<int32_t> g_collapse_state {0};

bool ProbeCollapseSupport() {
        auto state = g_collapse_state.load(std::memory_order_acquire);
        if (state != 0) {
                return state > 0;
        }
        void* probe = mmap(nullptr, HugePageBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (probe == MAP_FAILED) {
                g_collapse_state.store(-1, std::memory_order_release);
                return false;
        }
        // Keep the probe itself on small pages: the sweeper's collapses must work even
        // for MADV_NOHUGEPAGE ranges (which is what reservations get in sweeper mode).
        (void)madvise(probe, HugePageBytes, MADV_NOHUGEPAGE);
        auto* cursor = reinterpret_cast<volatile uint8_t*>(probe);
        for (uint64_t page = 0; page < HugePageSubPages; page++) {
                cursor[page * 4096] = static_cast<uint8_t>(page);
        }
        const bool supported = madvise(probe, HugePageBytes, MADV_COLLAPSE) == 0;
        (void)munmap(probe, HugePageBytes);
        g_collapse_state.store(supported ? 1 : -1, std::memory_order_release);
        ThpLogInfo(supported ? "MADV_COLLAPSE supported" : "MADV_COLLAPSE unsupported, Q2 advice fallback",
                   0, 0);
        return supported;
}

// ---------------------------------------------------------------- tracked ranges

struct Range {
        uint64_t start;
        uint64_t end;
};

std::mutex           g_ranges_mutex;
std::vector<Range>   g_ranges;
std::atomic<uint64_t> g_notify_epoch {0};
bool                 g_ranges_full_logged = false;

// Merged-interval insert: commits arrive as many small adjacent pieces (the guest
// commits memory in 64 KiB..16 MiB steps), and the sweeper wants whole spans.
void TrackInterval(uint64_t start, uint64_t end) {
        std::lock_guard lock(g_ranges_mutex);
        if (g_ranges.size() >= MaxTrackedRanges) {
                if (!g_ranges_full_logged) {
                        g_ranges_full_logged = true;
                        ThpLogInfo("tracked-range table full; further commits untracked", g_ranges.size(), 0);
                }
                return;
        }
        uint64_t span_start = start;
        uint64_t span_end   = end;
        // First range whose end reaches past the new start.
        auto it = std::lower_bound(g_ranges.begin(), g_ranges.end(), span_start,
                                   [](const Range& r, uint64_t s) { return r.end < s; });
        // Merge the predecessor when it touches or overlaps the new span.
        if (it != g_ranges.begin()) {
                auto pit = std::prev(it);
                if (pit->end >= span_start) {
                        span_start = std::min(span_start, pit->start);
                        span_end   = std::max(span_end, pit->end);
                        it         = g_ranges.erase(pit);
                }
        }
        // Merge every following range that touches or overlaps the new span.
        while (it != g_ranges.end() && it->start <= span_end) {
                span_start = std::min(span_start, it->start);
                span_end   = std::max(span_end, it->end);
                it         = g_ranges.erase(it);
        }
        g_ranges.insert(it, Range {span_start, span_end});
        g_notify_epoch.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------- sweeper thread

std::atomic<bool>  g_sweeper_started {false};
std::atomic<bool>  g_stop_requested {false};
std::atomic<uint64_t> g_collapsed_total {0};
std::atomic<uint64_t> g_sweeps_done {0};
std::atomic<uint64_t> g_last_sweep_us {0};

void SleepMs(uint32_t milliseconds) {
        timespec ts {};
        ts.tv_sec  = static_cast<time_t>(milliseconds / 1000u);
        ts.tv_nsec = static_cast<long>(milliseconds % 1000u) * 1000000L;
        (void)nanosleep(&ts, nullptr);
}

// One pass over every tracked range: collapse each 2 MiB chunk whose 512 pages are
// all resident. mincore makes the decision exact - the sweeper never collapses
// (never allocates) memory the game has not actually touched, and never touches a
// chunk that is still PROT_NONE. Returns the number of chunks collapsed.
uint64_t SweepOnce() {
        std::vector<Range> snapshot;
        {
                std::lock_guard lock(g_ranges_mutex);
                snapshot = g_ranges;
        }
        const auto begin = std::chrono::steady_clock::now();
        uint64_t    collapsed = 0;
        for (const auto& range: snapshot) {
                uint64_t chunk = (range.start + HugePageBytes - 1) & ~(HugePageBytes - 1);
                for (; chunk + HugePageBytes <= range.end; chunk += HugePageBytes) {
                        unsigned char resident[HugePageSubPages];
                        if (mincore(reinterpret_cast<void*>(chunk), HugePageBytes, resident) != 0) {
                                continue; // unmapped or unmappable by now - harmless
                        }
                        bool all_resident = true;
                        for (uint64_t page = 0; page < HugePageSubPages; page++) {
                                if ((resident[page] & 1u) == 0) {
                                        all_resident = false;
                                        break;
                                }
                        }
                        if (!all_resident) {
                                continue;
                        }
                        if (madvise(reinterpret_cast<void*>(chunk), HugePageBytes, MADV_COLLAPSE) == 0) {
                                collapsed++;
                        }
                }
        }
        const auto end  = std::chrono::steady_clock::now();
        const auto us   = std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
        g_last_sweep_us.store(static_cast<uint64_t>(us), std::memory_order_relaxed);
        g_sweeps_done.fetch_add(1, std::memory_order_relaxed);
        if (collapsed != 0) {
                g_collapsed_total.fetch_add(collapsed, std::memory_order_relaxed);
        }
        return collapsed;
}

// Kernel without MADV_COLLAPSE: sweeper mode was optimistically advised
// MADV_NOHUGEPAGE on the big reservations. One rescue pass converts them to the Q2
// advice so those devices keep the huge-page win through the fault path.
void RescuePassToAdviceMode() {
        std::vector<Range> snapshot;
        {
                std::lock_guard lock(g_ranges_mutex);
                snapshot = g_ranges;
        }
        uint64_t advised = 0;
        uint64_t bytes   = 0;
        for (const auto& range: snapshot) {
                if (range.end - range.start >= ReservationAdviceBytes) {
                        (void)madvise(reinterpret_cast<void*>(range.start), range.end - range.start, MADV_HUGEPAGE);
                        advised++;
                        bytes += range.end - range.start;
                }
        }
        ThpLogInfo("rescue pass: Q2 advice applied", advised, bytes);
}

void* SweeperMain(void* /*arg*/) {
#if defined(__ANDROID__) || defined(__linux__)
        pthread_setname_np(pthread_self(), "CapyTHP");
        // Background work must never steal cycles from the emulated game threads.
        if (setpriority(PRIO_PROCESS, 0, SweeperNice) != 0) {
                ThpLogInfo("sweeper: could not lower priority", 0, 0);
        }
#endif
        if (!ProbeCollapseSupport()) {
                RescuePassToAdviceMode();
                return nullptr;
        }
        uint32_t sleep_ms = ActiveSweepIntervalMs();
        uint64_t epoch_before = g_notify_epoch.load(std::memory_order_relaxed);
        while (!g_stop_requested.load(std::memory_order_relaxed)) {
                SleepMs(sleep_ms);
                if (g_stop_requested.load(std::memory_order_relaxed)) {
                        break;
                }
                const uint64_t collapsed = SweepOnce();
                const uint64_t epoch_now = g_notify_epoch.load(std::memory_order_relaxed);
                if (collapsed != 0 || epoch_now != epoch_before) {
                        // Something changed: stay (or return to) the active cadence.
                        sleep_ms      = ActiveSweepIntervalMs();
                        epoch_before  = epoch_now;
                } else {
                        // Quiet game: back off exponentially, capped.
                        sleep_ms = std::min(sleep_ms * 2u, IdleSweepMs);
                }
        }
        return nullptr;
}

void StartSweeperLocked() {
        // Caller checked g_sweeper_started under g_ranges_mutex? No: it is checked with
        // the sweeper's own start flag, raced benignly (two threads may both see false;
        // the CAS below serialises the actual spawn).
        bool expected = false;
        if (!g_sweeper_started.compare_exchange_strong(expected, true)) {
                return;
        }
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        // A tiny stack: the sweep state is a snapshot vector and a 512-byte mincore map.
        pthread_attr_setstacksize(&attr, 128 * 1024);
        pthread_t thread = 0;
        const int  created = pthread_create(&thread, &attr, SweeperMain, nullptr);
        pthread_attr_destroy(&attr);
        if (created != 0) {
                // Could not spawn: fall back to the Q2 advice mode for future reservations.
                g_sweeper_started.store(false);
                g_collapse_state.store(-1, std::memory_order_release);
                ThpLogInfo("sweeper: spawn failed, Q2 advice fallback", 0, 0);
        }
}

} // namespace

// ---------------------------------------------------------------- public API

void OnReservationCreated(uint64_t address, uint64_t size) {
        if (address == 0 || size == 0) {
                return;
        }
        switch (CurrentMode()) {
                case ThpMode::Off:
                        return;
                case ThpMode::Advice:
                        // Q2 behaviour: big reservations become fault-path huge-page eligible.
                        if (size >= ReservationAdviceBytes) {
                                (void)madvise(reinterpret_cast<void*>(address), size, MADV_HUGEPAGE);
                        }
                        return;
                case ThpMode::Sweeper:
                        break;
        }
        // Sweeper mode: first-touch faults stay on 4 KiB pages (no direct-compaction
        // stall can ever hit a game thread); the background sweeper collapses fully
        // resident chunks. If the kernel turns out not to support MADV_COLLAPSE, the
        // sweeper's rescue pass converts these back to MADV_HUGEPAGE.
        if (size >= ReservationAdviceBytes) {
                (void)madvise(reinterpret_cast<void*>(address), size, MADV_NOHUGEPAGE);
        }
        if (size >= TrackMinBytes) {
                TrackInterval(address, address + size);
        }
        StartSweeperLocked();
}

void OnCommit(uint64_t address, uint64_t size) {
        if (address == 0 || size < CommitNotifyBytes) {
                return;
        }
        if (CurrentMode() != ThpMode::Sweeper) {
                return;
        }
        if (g_collapse_state.load(std::memory_order_acquire) < 0) {
                return; // probe already decided: unsupported, rescue pass handled the advice
        }
        TrackInterval(address, address + size);
        StartSweeperLocked();
}

void StopSweeper() {
        g_stop_requested.store(true, std::memory_order_relaxed);
}

uint64_t CollapsedChunks() {
        return g_collapsed_total.load(std::memory_order_relaxed);
}

uint64_t SweepCount() {
        return g_sweeps_done.load(std::memory_order_relaxed);
}

uint64_t LastSweepDurationUsec() {
        return g_last_sweep_us.load(std::memory_order_relaxed);
}

namespace {
// The sweeper is a detached background thread; halt it before static teardown.
struct SweeperAtExit {
        SweeperAtExit() {
                (void)atexit([]() { StopSweeper(); });
        }
};
const SweeperAtExit g_sweeper_at_exit;
} // namespace

} // namespace Common::HugePages

#endif
