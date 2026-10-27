#!/usr/bin/env python3
"""v0.0.10pro phase 1: guest wild-pointer STORE recovery (core).

Files touched (binary-safe, exact bytes; runtimeLinker.cpp/memory.cpp are CRLF+tab):
  core/src/kernel/memory.h             - declare Memory::IsRangeOutsideGuestMemory
  core/src/kernel/memory.cpp           - the helper (mirrors AdoptFaultingGuestWindow checks)
  core/src/cpu/cpuBackend.h            - declare Cpu::RecoverGuestWildStore
  core/src/cpu/cpuBackend.cpp          - the store-decoding emulator
  core/src/loader/runtimeLinker.cpp    - hook into KytyExceptionHandler

The v0.0.9proQ1 crash log (build f9884a5, Mali device, heavy Unity title): the game's
render worker wrote through a garbage base register -

  Unhandled host exception: type=1 code=11 pc=0x... access=1 address=0x3a0888c1
  in runtimeLinker.cpp:893

The faulting address is neither null (so the v0.0.9proQ1 null-READ recovery does not
apply) nor inside any guest mapping: nothing the game can ever read aliases it. A
one-off wild store through a corrupted pointer (a race, an HLE gap returning garbage)
should not end the session, so the faulting plain A64 store is decoded and skipped:
no register state changes (store destinations are sources), the PC advances and the
session keeps running. Exactly-matched plain stores only (STR/STUR/STP/register
offset, GPR + SIMD) - no writeback forms, no atomics, no exclusives, no load/store
expressions that also write a register. The whole access must sit outside every
guest mapping and reservation; a budget (64) stops a genuine wild-store loop from
becoming a silent hang. `setprop debug.capy.wild_store_recovery 0` restores the old
behaviour.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def patch(path: Path, edits, must_find_all: bool = True):
    """edits: list of (anchor_bytes, replacement_bytes). Applies each once. Idempotent."""
    data = path.read_bytes()
    for old, new in edits:
        if data.count(new) == 1 and new != old:
            print(f"skip {path.name}: edit already applied")
            continue
        n = data.count(old)
        if n != 1:
            print(f"FAIL {path.name}: anchor found {n}x (expected 1):")
            print(old[:160])
            if must_find_all:
                sys.exit(1)
        data = data.replace(old, new, 1)
    path.write_bytes(data)
    print(f"ok {path} ({len(edits)} edit(s))")


# ---------------------------------------------------------------- memory.h (LF)
memory_h = ROOT / "core/src/kernel/memory.h"
patch(memory_h, [
    (
        b"""[[nodiscard]] bool HandleGpuFault(Graphics::PageFaultAccess access, uint64_t fault_vaddr) noexcept;
[[nodiscard]] bool AdoptFaultingGuestWindow(uint64_t fault_vaddr) noexcept;
""",
        b"""[[nodiscard]] bool HandleGpuFault(Graphics::PageFaultAccess access, uint64_t fault_vaddr) noexcept;
[[nodiscard]] bool AdoptFaultingGuestWindow(uint64_t fault_vaddr) noexcept;
// Console Five Pro (v0.0.10pro phase 1): true when [address, address+size) aliases no guest
// memory - neither the big guest reservation nor any registered virtual range. The wild-store
// recovery calls this from the SIGSEGV handler, mirroring AdoptFaultingGuestWindow's checks.
[[nodiscard]] bool IsRangeOutsideGuestMemory(uint64_t address, uint64_t size) noexcept;
""",
    ),
])

# ------------------------------------------------------------- memory.cpp (CRLF)
memory_cpp = ROOT / "core/src/kernel/memory.cpp"
patch(memory_cpp, [
    # The helper right after AdoptFaultingGuestWindow's body. Anchor on the NoteOnce tail
    # of that function plus the following struct opening (unique bytes).
    (
        b"""\t                             static_cast<unsigned long long>(page));\r
\treturn true;\r
#else\r
\t(void)fault_vaddr;\r
\treturn false;\r
#endif\r
}\r
\r
struct PrtAperture {\r
""",
        b"""\t                             static_cast<unsigned long long>(page));\r
\treturn true;\r
#else\r
\t(void)fault_vaddr;\r
\treturn false;\r
#endif\r
}\r
\r
bool IsRangeOutsideGuestMemory(uint64_t address, uint64_t size) noexcept {\r
\t// Console Five Pro (v0.0.10pro phase 1): the wild-store recovery runs in the SIGSEGV\r
\t// handler and needs to know whether a faulting access could ever alias guest memory.\r
\t// Same ownership checks as AdoptFaultingGuestWindow: the big guest reservation plus\r
\t// every registered virtual range. An empty or wrapped range counts as guest-owned\r
\t// (recover nothing).\r
\tif (size == 0 || UINT64_MAX - address < size) {\r
\t\treturn false;\r
\t}\r
\tif (g_guest_address_space != nullptr && g_guest_address_space->Owns(address, size)) {\r
\t\treturn false;\r
\t}\r
\tif (g_virtual_ranges != nullptr && g_virtual_ranges->HasOverlap(address, size)) {\r
\t\treturn false;\r
\t}\r
\treturn true;\r
}\r
\r
struct PrtAperture {\r
""",
    ),
])

# ---------------------------------------------------------- cpuBackend.h (LF+tab)
cpu_h = ROOT / "core/src/cpu/cpuBackend.h"
patch(cpu_h, [
    (
        b"""bool RecoverGuestNullRead(void* native_context, uint64_t fault_address);
""",
        b"""bool RecoverGuestNullRead(void* native_context, uint64_t fault_address);
/**
 * Recovers a guest plain STORE through a wild pointer by skipping it: when the whole
 * access lies outside every guest mapping and reservation, nothing the game can read
 * aliases the target, so dropping the store is unobservable. No register state changes
 * (store destinations are sources); the PC advances past the instruction. Returns false
 * when the recovery is disabled (debug.capy.wild_store_recovery=0), the budget is spent,
 * the address is a true null dereference (below 64 KiB keeps the honest crash report) or
 * the instruction is not an exactly-matched plain store.
 */
bool RecoverGuestWildStore(void* native_context, uint64_t fault_address);
""",
    ),
])

# -------------------------------------------------------- cpuBackend.cpp (LF+tab)
cpu_cpp = ROOT / "core/src/cpu/cpuBackend.cpp"
patch(cpu_cpp, [
    # New function right after RecoverGuestNullRead, before CpuBackend::Instance().
    (
        b"""CpuBackend* CpuBackend::Instance() {
""",
        b"""// Console Five Pro (v0.0.10pro phase 1): guest wild-pointer STORE recovery. The
// v0.0.9proQ1 crash log ended a heavy Unity title's session with "Unhandled host
// exception: type=1 code=11 access=1 address=0x3a0888c1" - the render worker stored
// through a garbage base register to an address that is neither null nor inside any
// guest mapping. Nothing the game can ever read aliases such an address, so the store
// is skipped and the session continues instead of dying on a one-off wild write (a
// race, an HLE gap returning garbage). Exactly-matched plain stores only: STR/STUR
// (unsigned/unscaled immediate), STP (signed offset, no writeback) and register-offset
// STR, GPR and SIMD - no post/pre-index writeback, no atomics, no exclusives. Store
// destinations are sources, so no register state is touched; only the PC advances.
// True null dereferences (below 64 KiB) keep the honest crash report; a budget stops
// a genuine wild-store loop from becoming a silent hang.
// `setprop debug.capy.wild_store_recovery 0` restores the old behaviour.
bool RecoverGuestWildStore(void* native_context, uint64_t fault_address) {
#if defined(__aarch64__)
\tstatic Common::DebugFlags::CachedValue enabled("debug.capy.wild_store_recovery", 1);
\tif (enabled.Get() == 0) {
\t\treturn false;
\t}
\t// A store below 64 KiB is a genuine null dereference: report it as before instead of
\t// silently dropping stores into the null page.
\tif (fault_address < 0x10000) {
\t\treturn false;
\t}
\tstatic constexpr uint32_t kBudget = 64;
\tstatic std::atomic<uint32_t> recovered {0};
\tif (recovered.load(std::memory_order_relaxed) >= kBudget) {
\t\treturn false;
\t}

\tauto* context = static_cast<ucontext_t*>(native_context);
\tif (context == nullptr) {
\t\treturn false;
\t}
\tconst uintptr_t pc = static_cast<uintptr_t>(context->uc_mcontext.pc);
\tconst uint32_t insn = *reinterpret_cast<const uint32_t*>(pc);

\tuint32_t bytes = 0;
\t// The masked groups below are the store mirrors of the null-read decoder's load
\t// tables (bit 22 clear = store). `bytes` is the memory the store touches; no
\t// destination register is written, so a single size per form is all that matters.
\tswitch (insn & 0xFFC00000u) {
\t\t// GPR stores, unsigned immediate offset (no writeback forms in this group).
\t\tcase 0x39000000: bytes = 1;  break; // STRB Wt
\t\tcase 0x79000000: bytes = 2;  break; // STRH Wt
\t\tcase 0xB9000000: bytes = 4;  break; // STR Wt
\t\tcase 0xF9000000: bytes = 8;  break; // STR Xt
\t\t// SIMD&FP stores, unsigned immediate offset.
\t\tcase 0x3D000000: bytes = 1;  break; // STR Bt
\t\tcase 0x7D000000: bytes = 2;  break; // STR Ht
\t\tcase 0xBD000000: bytes = 4;  break; // STR St
\t\tcase 0xFD000000: bytes = 8;  break; // STR Dt
\t\tcase 0x3D800000: bytes = 16; break; // STR Qt
\t\t// Store pair, signed offset (the masked bits keep post/pre-index out).
\t\tcase 0x29000000: bytes = 8;  break; // STP Wt1, Wt2
\t\tcase 0xA9000000: bytes = 16; break; // STP Xt1, Xt2
\t\tcase 0x2D000000: bytes = 8;  break; // STP St1, St2
\t\tcase 0x6D000000: bytes = 16; break; // STP Dt1, Dt2
\t\tcase 0xAD000000: bytes = 32; break; // STP Qt1, Qt2
\t\tdefault: break;
\t}
\tif (bytes == 0) {
\t\t// Unscaled-offset stores. Bits 11:10 masked to 00: the STUR encoding, which never
\t\t// writes back.
\t\tswitch (insn & 0xFFC00C00u) {
\t\t\tcase 0x38000000: bytes = 1;  break; // STURB Wt
\t\t\tcase 0x78000000: bytes = 2;  break; // STURH Wt
\t\t\tcase 0xB8000000: bytes = 4;  break; // STUR Wt
\t\t\tcase 0xF8000000: bytes = 8;  break; // STUR Xt
\t\t\tcase 0x3C000000: bytes = 1;  break; // STUR Bt
\t\t\tcase 0x7C000000: bytes = 2;  break; // STUR Ht
\t\t\tcase 0xBC000000: bytes = 4;  break; // STUR St
\t\t\tcase 0xFC000000: bytes = 8;  break; // STUR Dt
\t\t\tcase 0x3C800000: bytes = 16; break; // STUR Qt
\t\t\tdefault: break;
\t\t}
\t}
\tif (bytes == 0) {
\t\t// Register-offset stores (Xm-indexed; bits 11:10 masked to 10, the register
\t\t// encoding, which never writes back).
\t\tswitch (insn & 0xFFE00C00u) {
\t\t\tcase 0x38200800: bytes = 1;  break; // STRB Wt, [Xn, Xm]
\t\t\tcase 0x78200800: bytes = 2;  break; // STRH Wt, [Xn, Xm]
\t\t\tcase 0xB8200800: bytes = 4;  break; // STR Wt, [Xn, Xm]
\t\t\tcase 0xF8200800: bytes = 8;  break; // STR Xt, [Xn, Xm]
\t\t\tcase 0x3C200800: bytes = 1;  break; // STR Bt, [Xn, Xm]
\t\t\tcase 0x7C200800: bytes = 2;  break; // STR Ht, [Xn, Xm]
\t\t\tcase 0xBC200800: bytes = 4;  break; // STR St, [Xn, Xm]
\t\t\tcase 0xFC200800: bytes = 8;  break; // STR Dt, [Xn, Xm]
\t\t\tcase 0x3CA00800: bytes = 16; break; // STR Qt, [Xn, Xm]
\t\t\tdefault: break;
\t\t}
\t}
\tif (bytes == 0) {
\t\treturn false;
\t}
\t// The whole access must lie outside every guest mapping and reservation: a store
\t// reaching into guest memory is never dropped.
\tif (!Libs::LibKernel::Memory::IsRangeOutsideGuestMemory(fault_address, bytes)) {
\t\treturn false;
\t}
\tcontext->uc_mcontext.pc = static_cast<uint64_t>(pc + 4);
\tconst auto count = recovered.fetch_add(1, std::memory_order_relaxed) + 1;
\tif (count <= 4 || count == kBudget) {
\t\tALOGW("CpuBackend: skipped guest wild store #%u/%u (pc=0x%llx vaddr=0x%llx) - "
\t\t      "target aliases no guest memory, continuing",
\t\t      count, kBudget, static_cast<unsigned long long>(pc),
\t\t      static_cast<unsigned long long>(fault_address));
\t}
\treturn true;
#else
\t(void)native_context;
\t(void)fault_address;
\treturn false;
#endif
}

CpuBackend* CpuBackend::Instance() {
""",
    ),
])

# ---------------------------------------------------- runtimeLinker.cpp (CRLF+tab)
linker_cpp = ROOT / "core/src/loader/runtimeLinker.cpp"
patch(linker_cpp, [
    (
        b"""\t\tif (access == GpuAccess::Read && info->access_violation_vaddr < 0x10000 &&\r
\t\t    Cpu::GuestRipFromHostPc(info->exception_address) != 0 &&\r
\t\t    Cpu::RecoverGuestNullRead(info->native_context, info->access_violation_vaddr)) {\r
\t\t\treturn true;\r
\t\t}\r
""",
        b"""\t\tif (access == GpuAccess::Read && info->access_violation_vaddr < 0x10000 &&\r
\t\t    Cpu::GuestRipFromHostPc(info->exception_address) != 0 &&\r
\t\t    Cpu::RecoverGuestNullRead(info->native_context, info->access_violation_vaddr)) {\r
\t\t\treturn true;\r
\t\t}\r
\t\t// Console Five Pro (v0.0.10pro phase 1): a plain guest STORE through a wild pointer (a\r
\t\t// heavy Unity title's render worker wrote via a garbage base register to 0x3a0888c1)\r
\t\t// ended the session the same way. When the whole access lies outside every guest\r
\t\t// mapping and reservation, nothing the game can read aliases the target - skip the\r
\t\t// store and continue. debug.capy.wild_store_recovery=0 restores the old behaviour.\r
\t\tif (access == GpuAccess::Write &&\r
\t\t    Cpu::GuestRipFromHostPc(info->exception_address) != 0 &&\r
\t\t    Cpu::RecoverGuestWildStore(info->native_context, info->access_violation_vaddr)) {\r
\t\t\treturn true;\r
\t\t}\r
""",
    ),
])

print("wild-store recovery: all files patched")
