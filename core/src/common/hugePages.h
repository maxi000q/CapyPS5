// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

// Console Five Pro (v0.0.9proQ4 round 2): huge-page engine v3 for guest memory.
//
// History: Q2 advised MADV_HUGEPAGE on every big guest commit - huge win (roughly 500x
// fewer soft faults, far fewer TLB misses for every guest dereference the FEX
// translation makes), but the huge-page allocation itself happens on the faulting
// game thread, and under memory pressure the kernel can enter synchronous direct
// compaction there - a multi-millisecond stall in the middle of a frame. Q3
// disabled the advice on Android to kill the stalls and paid the whole TLB win
// back (the reported full-frame-per-10 regression). Round 2 gets both:
//
//   debug.capy.thp = 1 (Android default): the "sweeper". Big reservations are
//     advised MADV_NOHUGEPAGE so first-touch faults stay on 4 KiB pages and can
//     never stall on compaction; a low-priority background thread then walks the
//     tracked ranges, and every 2 MiB chunk whose 512 pages are ALL resident
//     (verified with mincore) is collapsed into a huge page with MADV_COLLAPSE.
//     Any compaction the collapse needs runs on the sweeper thread, never on a
//     game thread, and only memory the game actually uses is collapsed (no
//     overcommit). If the kernel lacks MADV_COLLAPSE (pre-6.1) the engine falls
//     back to the Q2 advice mode automatically.
//
//   debug.capy.thp = 2 (desktop default): the Q2 advice mode exactly - MADV_HUGEPAGE
//     on every big commit, fault-path huge allocation, khugepaged assistance.
//
//   debug.capy.thp = 0: off entirely (4 KiB pages, the Q3 behaviour).

#ifndef KYTY_COMMON_HUGEPAGES_H_
#define KYTY_COMMON_HUGEPAGES_H_

#include "common/common.h"

namespace Common {

namespace HugePages {

/** A new anonymous mapping was created (reservation paths of VirtualMemory).
 * Applies the mode's huge-page advice once per big reservation and starts tracking
 * the range for the background collapse sweeper. Cheap when THP is disabled.
 * Never called while the caller holds the virtual-memory mutex's counterpart lock
 * ordering: this takes only the sweeper's own mutex. */
void OnReservationCreated(uint64_t address, uint64_t size);

/** A sub-range of a reservation became accessible and writable/readable
 * (Protect/Commit paths). Merged into the tracked set so the sweeper can collapse
 * fully-resident chunks inside it. Cheap when THP is disabled. */
void OnCommit(uint64_t address, uint64_t size);

/** Stops the background sweeper (registered with atexit; also safe to call
 * directly from tests or a controlled shutdown). Returns immediately - the
 * detached thread notices the flag on its next wake-up. */
void StopSweeper();

/** Diagnostics: number of 2 MiB chunks collapsed to huge pages so far. */
uint64_t CollapsedChunks();

/** Diagnostics: number of sweeper passes completed so far. */
uint64_t SweepCount();

/** Diagnostics: microseconds spent inside the last completed sweep. */
uint64_t LastSweepDurationUsec();

} // namespace HugePages

} // namespace Common

#endif /* KYTY_COMMON_HUGEPAGES_H_ */
