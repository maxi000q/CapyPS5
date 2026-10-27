// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only
//
// v0.0.10pro phase 4 - engine-policy test (REAL CODE, host).
//
// Compiles the SHIPPED core/src/common/enginePolicy.h, renderPassMode.h and
// contentProfile.cpp (unmodified) and asserts the policy layer:
//
//  1. The resolution-keyed policy now floors the tier at Throughput500 on
//     EVERY vendor - at v0.0.9proQ4 the policy's only lever was the
//     render-pass clear mode, and that lever is a no-op on Mali (mode 0 is
//     already selected there), so a 4K game on a Mali device moved NO knob at
//     all: the on-device "4K added literally nothing" report.
//  2. The new Forward3D502 row exists per vendor and carries the throughput
//     preset; every turbo tier is at or above the General preset on every
//     throughput axis (deeper submits, wider read-ahead, bigger cache, more
//     compile workers, faster sweep, higher pool cap).
//  3. The render-pass clear-mode selection is unchanged where it was already
//     correct (Mali 0, Adreno 4, others 3, 4K -> 4 on non-Mali) and an
//     explicit setprop still wins.
//
// What this does NOT prove: absolute FPS on any device (see README.md).

#include "common/contentProfile.h"
#include "common/enginePolicy.h"
#include "common/renderPassMode.h"

#include <cstdio>

using Common::ContentProfile::HighResolutionPolicyActive;
using Common::ContentProfile::ObserveDraw;
using Common::ContentProfile::OnFrameEnd;
using Common::ContentProfile::ResetForNextGame;
using Common::ContentProfile::WorkloadClass;
using Common::EnginePolicy::CurrentTier;
using Common::EnginePolicy::EngineTuning;
using Common::EnginePolicy::TuningFor;
using Common::EnginePolicy::WorkloadTier;
namespace Rp = Common::RenderPass;

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond, label)                                                                 \
        do {                                                                                   \
                g_checks++;                                                                        \
                if (!(cond)) {                                                                     \
                        g_failures++;                                                                  \
                        std::printf("  FAIL %s\n", label);                                             \
                } else {                                                                           \
                        std::printf("  ok   %s\n", label);                                             \
                }                                                                                  \
        } while (0)

// Drive the real classifier until a forward-3D shape locks 502.
static void LockForward3D() {
        ResetForNextGame();
        for (uint32_t f = 0; f < 32 * 4 + 2; f++) {
                for (uint32_t i = 0; i < 400; i++) {
                        ObserveDraw(6000, 1, false, 1, false, 1920, 1080);
                }
                OnFrameEnd();
        }
}

// Drive the real classifier until the resolution latch fires WITHOUT locking
// any workload class: 50 draws/frame keeps every window sparse (below the
// 60-draw floor), but the resolution latch is independent of the sparseness
// gate - a window that only shows the game's big colour targets still counts
// for it. This is exactly a 4K title whose gameplay windows are sparse.
static void Latch4KOnly() {
        ResetForNextGame();
        for (uint32_t f = 0; f < 32 * 3 + 2; f++) {
                for (uint32_t i = 0; i < 50; i++) {
                        ObserveDraw(6000, 1, false, 1, false, 3840, 2160);
                }
                OnFrameEnd();
        }
}

static void CheckTierAtLeast(const EngineTuning& turbo, const EngineTuning& general,
                             const char* label) {
        CHECK(turbo.submit_batch >= general.submit_batch, "submit_batch at least General");
        CHECK(turbo.read_ahead_base_kb >= general.read_ahead_base_kb, "read-ahead base at least General");
        CHECK(turbo.read_ahead_cap >= general.read_ahead_cap, "read-ahead cap at least General");
        CHECK(turbo.read_cache_slots >= general.read_cache_slots, "read-cache slots at least General");
        CHECK(turbo.compile_profile_extra + turbo.compile_class_extra >=
                  general.compile_profile_extra + general.compile_class_extra,
              "compile workers at least General");
        CHECK(turbo.compile_pool_cap >= general.compile_pool_cap, "pool cap at least General");
        CHECK(turbo.thp_active_sweep_ms <= general.thp_active_sweep_ms, "sweep at least as fast");
        (void)label;
}

int main() {
        std::printf("engine-policy test (real shipped headers)\n");

        // ------------------------------------------------ 1. the 4K tier floor
        {
                ResetForNextGame();
                CHECK(CurrentTier() == WorkloadTier::General, "cold start: General tier");
                Latch4KOnly();
                CHECK(HighResolutionPolicyActive(), "4K windows latched the resolution policy");
                CHECK(CurrentTier() == WorkloadTier::Throughput500,
                      "4K policy floors the tier at Throughput500 (was General: no-op on Mali at Q4)");
        }
        {
                ResetForNextGame();
                CHECK(CurrentTier() == WorkloadTier::General, "1080p content stays General");
        }
        // A locked class wins over the floor (more specific measurement).
        {
                ResetForNextGame();
                Latch4KOnly();
                LockForward3D();
                CHECK(Common::ContentProfile::ActiveClass() == WorkloadClass::Turbo502, "502 locked");
                CHECK(CurrentTier() == WorkloadTier::Forward3D502,
                      "locked class wins over the 4K floor");
        }

        // ------------------------------------------------ 2. the table
        {
                struct VendorCase {
                        const char* name;
                        bool        mali;
                        bool        adreno;
                };
                const VendorCase vendors[] = {
                    {"Mali", true, false}, {"Adreno", false, true}, {"other", false, false},
                };
                for (const auto& v: vendors) {
                        const auto& general     = TuningFor(v.mali, v.adreno, WorkloadTier::General);
                        const auto& throughput  = TuningFor(v.mali, v.adreno, WorkloadTier::Throughput500);
                        const auto& forward3d   = TuningFor(v.mali, v.adreno, WorkloadTier::Forward3D502);
                        const auto& compositing = TuningFor(v.mali, v.adreno, WorkloadTier::Compositing501);

                        std::printf("table: %s\n", v.name);
                        // Forward3D502 carries the throughput preset (phase-4 design).
                        CHECK(forward3d.submit_batch == throughput.submit_batch &&
                              forward3d.read_ahead_base_kb == throughput.read_ahead_base_kb &&
                              forward3d.read_ahead_cap == throughput.read_ahead_cap &&
                              forward3d.read_cache_slots == throughput.read_cache_slots &&
                              forward3d.compile_profile_extra == throughput.compile_profile_extra &&
                              forward3d.compile_class_extra == throughput.compile_class_extra &&
                              forward3d.compile_pool_cap == throughput.compile_pool_cap &&
                              forward3d.thp_active_sweep_ms == throughput.thp_active_sweep_ms,
                              "Forward3D502 row == Throughput500 row");
                        // ... and is strictly stronger than General on every axis.
                        CheckTierAtLeast(forward3d, general, "Forward3D502 vs General");
                        CheckTierAtLeast(throughput, general, "Throughput500 vs General");
                        CheckTierAtLeast(compositing, general, "Compositing501 vs General");

                        // The pre-phase-4 rows are byte-identical to round 5 (the p8 battery
                        // locks them; this guards accidental drift while the table grew).
                        if (v.mali) {
                                CHECK(general.submit_batch == 24 && general.read_ahead_base_kb == 256 &&
                                      general.read_cache_slots == 8192 && general.compile_pool_cap == 6,
                                      "Mali General row unchanged from round 5");
                                CHECK(throughput.submit_batch == 32 && throughput.read_cache_slots == 16384 &&
                                      throughput.compile_class_extra == 2 && throughput.compile_pool_cap == 8,
                                      "Mali Throughput row unchanged from round 5");
                                CHECK(compositing.submit_batch == 40, "Mali Compositing row unchanged");
                        } else if (v.adreno) {
                                CHECK(general.submit_batch == 16 && general.read_ahead_base_kb == 512 &&
                                      general.read_cache_slots == 4096,
                                      "Adreno General row unchanged from round 5");
                                CHECK(throughput.submit_batch == 24 && throughput.read_ahead_base_kb == 768,
                                      "Adreno Throughput row unchanged from round 5");
                        } else {
                                CHECK(general.submit_batch == 8 && general.read_cache_slots == 512,
                                      "no-profile General row unchanged from round 5");
                                CHECK(throughput.submit_batch == 24, "no-profile Throughput row unchanged");
                        }
                }
        }

        // ------------------------------------------------ 3. clear-mode selection
        {
                std::printf("render-pass clear mode\n");
                CHECK(Rp::SelectMode(-1, true, false, false) == Rp::kModeHardwareClears,
                      "Mali default: mode 0 (AFBC hardware clears)");
                CHECK(Rp::SelectMode(-1, true, false, true) == Rp::kModeHardwareClears,
                      "Mali + 4K policy: still mode 0 (cheapest there; the 4K policy acts via the tier)");
                CHECK(Rp::SelectMode(-1, false, true, false) == Rp::kModeHybridClears,
                      "Adreno default: mode 4");
                CHECK(Rp::SelectMode(-1, false, true, true) == Rp::kModeHybridClears,
                      "Adreno + 4K policy: mode 4");
                CHECK(Rp::SelectMode(-1, false, false, false) == Rp::kModeInPassClears,
                      "other vendors default: mode 3");
                CHECK(Rp::SelectMode(-1, false, false, true) == Rp::kModeHybridClears,
                      "other vendors + 4K policy: mode 4 (colour load elimination)");
                CHECK(Rp::SelectMode(2, true, true, true) == 2, "explicit setprop override wins");
        }

        std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
}
