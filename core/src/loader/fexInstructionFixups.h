// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#ifndef KYTY_LOADER_FEX_INSTRUCTION_FIXUPS_H_
#define KYTY_LOADER_FEX_INSTRUCTION_FIXUPS_H_

#include <cstdint>
#include <span>

namespace Loader {

/** Rewrites x86-64 hint instructions the FEXCore JIT cannot execute, in place, before
 * the translator ever sees them.
 *
 * The PS5's Zen 2 CPU runs MONITORX and MWAITX as power-friendly spin-wait hints; FEX
 * treats both as privileged ring-0 instructions and emits a halt for them, which kills
 * the thread on the first contested lock (audio middleware spawns such a watcher at
 * boot). Both are exactly three bytes, so each becomes the canonical three-byte NOP
 * (0F 1F 00): the memory monitor degrades to nothing and a wait loop degrades to a
 * plain spin, which the surrounding conditional already expects - MWAITX is
 * architecturally allowed to wake at any time.
 *
 * Decoding is anchored at eh_frame function starts (see DecodeEhFrameFunctionStarts)
 * so embedded data is never mistaken for code. The caller must pass a writable,
 * already-loaded executable segment and flush the instruction cache afterwards.
 *
 * @return how many instructions were rewritten. */
uint64_t RewriteFexPrivilegedHints(uint64_t segment_addr, uint64_t segment_size,
                                   std::span<const uintptr_t> function_starts);

} // namespace Loader

#endif /* KYTY_LOADER_FEX_INSTRUCTION_FIXUPS_H_ */
