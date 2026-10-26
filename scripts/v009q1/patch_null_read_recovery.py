#!/usr/bin/env python3
"""v0.0.9proQ1 round 2: guest null-pointer READ recovery (core).

Files touched (binary-safe, exact bytes):
  core/src/cpu/cpuBackend.h     - declare Cpu::RecoverGuestNullRead
  core/src/cpu/cpuBackend.cpp   - asm/sigcontext.h include + the emulator itself
  core/src/loader/runtimeLinker.cpp - hook into KytyExceptionHandler

A guest data READ below 64 KiB used to end the session with "the game itself
crashed" (the user's Mali device log: null-pointer read at 0x11, type=1 code=11).
Android cannot map the null page (mmap_min_addr = 0x10000), so the faulting A64
load instruction is emulated against a zero page instead: the destination
register receives what a zeroed page would return (zero), the PC advances past
the load and the session keeps running. Only exactly-matched plain loads (no
writeback, no atomics, no exclusives) are recovered; a budget stops real
null-deref loops; writes are never faked. `debug.capy.null_read_recovery=0`
restores the old behaviour.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def patch(path: Path, edits, must_find_all: bool = True):
    """edits: list of (anchor_bytes, replacement_bytes). Applies each once."""
    data = path.read_bytes()
    for old, new in edits:
        if data.count(old) != 1:
            n = data.count(old)
            print(f"FAIL {path.name}: anchor found {n}x (expected 1):")
            print(old[:160])
            if must_find_all:
                sys.exit(1)
        data = data.replace(old, new, 1)
    path.write_bytes(data)
    print(f"ok {path} ({len(edits)} edit(s))")


def nl(path: Path) -> bytes:
    """The file's own line ending."""
    data = path.read_bytes()
    return b"\r\n" if b"\r\n" in data else b"\n"


def block(text: str, eol: bytes) -> bytes:
    return text.replace("\n", eol.decode()).encode()


# ---------------------------------------------------------------- cpuBackend.h
hdr = ROOT / "core/src/cpu/cpuBackend.h"
eol = nl(hdr)
patch(
    hdr,
    [
        (
            block(
                "/** Guest RIP of the translated code at host address `host_pc` on this thread (0 when unknown). */\n"
                "uint64_t GuestRipFromHostPc(uint64_t host_pc);\n",
                eol,
            ),
            block(
                "/** Guest RIP of the translated code at host address `host_pc` on this thread (0 when unknown). */\n"
                "uint64_t GuestRipFromHostPc(uint64_t host_pc);\n"
                "/**\n"
                " * Recovers a guest null-pointer READ (fault below 64 KiB inside translated code) by\n"
                " * emulating the faulting A64 load against a zero page: the destination register\n"
                " * receives zero and execution resumes past the instruction. Returns false when the\n"
                " * recovery is disabled (debug.capy.null_read_recovery=0), the budget is spent or the\n"
                " * instruction is not an exactly-matched plain load; the caller then reports the\n"
                " * fault as before. Writes are never faked.\n"
                " */\n"
                "bool RecoverGuestNullRead(void* native_context, uint64_t fault_address);\n",
                eol,
            ),
        )
    ],
)

# -------------------------------------------------------------- cpuBackend.cpp
cpp = ROOT / "core/src/cpu/cpuBackend.cpp"
eol = nl(cpp)

fun = r'''// Console Five Pro (v0.0.9proQ1): guest null-pointer READ recovery. A data read below
// 64 KiB from translated guest code used to end the whole session with "the game
// itself crashed" - even when it was a one-off hiccup the game would have survived
// (a race, an HLE gap, an unused-but-uninitialised pointer). The null page cannot be
// mapped on Android (mmap_min_addr), so the load is emulated instead: the faulting
// A64 instruction is decoded, its destination receives what a zeroed page would
// return (zero for every sign- or zero-extending load) and the PC advances past it.
// Only exactly-matched plain loads are recovered - unsigned-offset, unscaled (LDUR)
// and register-offset single loads plus GPR LDP (signed offset); no writeback forms,
// no atomics, no exclusives, no literals. Anything else falls through to the normal
// crash report. Writes are never faked. A budget stops a real null-deref loop from
// becoming a silent hang. `setprop debug.capy.null_read_recovery 0` restores the
// old behaviour.
bool RecoverGuestNullRead(void* native_context, uint64_t fault_address) {
#if defined(__aarch64__)
	static Common::DebugFlags::CachedValue enabled("debug.capy.null_read_recovery", 1);
	if (enabled.Get() == 0) {
		return false;
	}
	static constexpr uint32_t kBudget = 64;
	static std::atomic<uint32_t> recovered {0};
	if (recovered.load(std::memory_order_relaxed) >= kBudget) {
		return false;
	}

	auto* context = static_cast<ucontext_t*>(native_context);
	if (context == nullptr) {
		return false;
	}
	const uintptr_t pc   = static_cast<uintptr_t>(context->uc_mcontext.pc);
	const uint32_t insn  = *reinterpret_cast<const uint32_t*>(pc);
	uint64_t*       gprs = reinterpret_cast<uint64_t*>(context->uc_mcontext.regs);
	const uint32_t  rt   = insn & 0x1fu;
	const uint32_t  rt2  = (insn >> 10) & 0x1fu;

	enum Dest { kNone, kGpr, kSimd };
	Dest      dest  = kNone;
	uint32_t  bytes = 0;
	bool      pair  = false;

	// Every case below loads from a zeroed region, so the destination is zero no
	// matter the sign/zero extension; `bytes` is the memory the load touches.
	switch (insn & 0xFFC00000u) {
		// GPR loads, unsigned immediate offset (this group has no writeback forms).
		case 0x39400000: dest = kGpr;  bytes = 1;  break; // LDRB Wt
		case 0x39800000: dest = kGpr;  bytes = 1;  break; // LDRSB Wt
		case 0x39C00000: dest = kGpr;  bytes = 1;  break; // LDRSB Xt
		case 0x79400000: dest = kGpr;  bytes = 2;  break; // LDRH Wt
		case 0x79800000: dest = kGpr;  bytes = 2;  break; // LDRSH Wt
		case 0x79C00000: dest = kGpr;  bytes = 2;  break; // LDRSH Xt
		case 0xB9400000: dest = kGpr;  bytes = 4;  break; // LDR Wt
		case 0xB9800000: dest = kGpr;  bytes = 4;  break; // LDRSW Xt
		case 0xF9400000: dest = kGpr;  bytes = 8;  break; // LDR Xt
		// SIMD&FP loads, unsigned immediate offset.
		case 0x3D400000: dest = kSimd; bytes = 1;  break; // LDR Bt
		case 0x7D400000: dest = kSimd; bytes = 2;  break; // LDR Ht
		case 0xBD400000: dest = kSimd; bytes = 4;  break; // LDR St
		case 0xFD400000: dest = kSimd; bytes = 8;  break; // LDR Dt
		case 0x3DC00000: dest = kSimd; bytes = 16; break; // LDR Qt
		// Load pair, signed offset (no writeback; the masked bits keep the
		// post/pre-index forms out). Both registers receive zero.
		case 0x29400000: dest = kGpr;  bytes = 8;  pair = true; break; // LDP Wt1, Wt2
		case 0xA9400000: dest = kGpr;  bytes = 16; pair = true; break; // LDP Xt1, Xt2
		default: break;
	}
	if (dest == kNone) {
		// Unscaled-offset loads. Bits 11:10 are masked to 00: the LDUR encoding,
		// which never writes back (post/pre-index use 01/11).
		switch (insn & 0xFFC00C00u) {
			case 0x38400000: dest = kGpr;  bytes = 1;  break; // LDURB Wt
			case 0x38800000: dest = kGpr;  bytes = 1;  break; // LDURSB Wt
			case 0x38C00000: dest = kGpr;  bytes = 1;  break; // LDURSB Xt
			case 0x78400000: dest = kGpr;  bytes = 2;  break; // LDURH Wt
			case 0x78800000: dest = kGpr;  bytes = 2;  break; // LDURSH Wt
			case 0x78C00000: dest = kGpr;  bytes = 2;  break; // LDURSH Xt
			case 0xB8400000: dest = kGpr;  bytes = 4;  break; // LDUR Wt
			case 0xB8800000: dest = kGpr;  bytes = 4;  break; // LDURSW Xt
			case 0xF8400000: dest = kGpr;  bytes = 8;  break; // LDUR Xt
			case 0x3C400000: dest = kSimd; bytes = 1;  break; // LDUR Bt
			case 0x7C400000: dest = kSimd; bytes = 2;  break; // LDUR Ht
			case 0xBC400000: dest = kSimd; bytes = 4;  break; // LDUR St
			case 0xFC400000: dest = kSimd; bytes = 8;  break; // LDUR Dt
			case 0x3CC00000: dest = kSimd; bytes = 16; break; // LDUR Qt
			default: break;
		}
	}
	if (dest == kNone) {
		// Register-offset loads (Xm-indexed; bits 11:10 masked to 10, the
		// register encoding, which never writes back).
		switch (insn & 0xFFE00C00u) {
			case 0x38600000: dest = kGpr;  bytes = 1;  break; // LDRB Wt, [Xn, Xm]
			case 0x38A00000: dest = kGpr;  bytes = 1;  break; // LDRSB Wt, [Xn, Xm]
			case 0x38E00000: dest = kGpr;  bytes = 1;  break; // LDRSB Xt, [Xn, Xm]
			case 0x78600000: dest = kGpr;  bytes = 2;  break; // LDRH Wt, [Xn, Xm]
			case 0x78A00000: dest = kGpr;  bytes = 2;  break; // LDRSH Wt, [Xn, Xm]
			case 0x78E00000: dest = kGpr;  bytes = 2;  break; // LDRSH Xt, [Xn, Xm]
			case 0xB8600000: dest = kGpr;  bytes = 4;  break; // LDR Wt, [Xn, Xm]
			case 0xB8A00000: dest = kGpr;  bytes = 4;  break; // LDRSW Xt, [Xn, Xm]
			case 0xF8600000: dest = kGpr;  bytes = 8;  break; // LDR Xt, [Xn, Xm]
			case 0x3C600000: dest = kSimd; bytes = 1;  break; // LDR Bt, [Xn, Xm]
			case 0x7C600000: dest = kSimd; bytes = 2;  break; // LDR Ht, [Xn, Xm]
			case 0xBC600000: dest = kSimd; bytes = 4;  break; // LDR St, [Xn, Xm]
			case 0xFC600000: dest = kSimd; bytes = 8;  break; // LDR Dt, [Xn, Xm]
			case 0x3CE00000: dest = kSimd; bytes = 16; break; // LDR Qt, [Xn, Xm]
			default: break;
		}
	}
	if (dest == kNone) {
		return false;
	}
	// The whole access must sit inside the unmapped null region: a load reaching
	// across the 64 KiB boundary could touch live guest memory.
	if (fault_address + bytes > 0x10000) {
		return false;
	}
	if (dest == kSimd) {
		// The SIMD destination lives in the fpsimd context inside the ucontext's
		// reserved area, behind its own magic header. Without it: no recovery.
		auto*       reserved     = reinterpret_cast<uint8_t*>(context->uc_mcontext.__reserved);
		const auto* reserved_end = reserved + sizeof(context->uc_mcontext.__reserved);
		auto*       fpsimd       = static_cast<struct fpsimd_context*>(nullptr);
		while (reserved + sizeof(struct _aarch64_ctx) <= reserved_end) {
			const auto* head = reinterpret_cast<const struct _aarch64_ctx*>(reserved);
			if (head->magic == 0 || head->size < sizeof(struct _aarch64_ctx) ||
			    reserved + head->size > reserved_end) {
				break;
			}
			if (head->magic == FPSIMD_MAGIC && head->size >= sizeof(struct fpsimd_context)) {
				fpsimd = reinterpret_cast<struct fpsimd_context*>(reserved);
				break;
			}
			reserved += head->size;
		}
		if (fpsimd == nullptr) {
			return false;
		}
		fpsimd->vregs[rt * 2]     = 0;
		fpsimd->vregs[rt * 2 + 1] = 0;
	} else if (rt != 31) { // GPR number 31 is the stack pointer here, never a load target
		gprs[rt] = 0;
		if (pair && rt2 != 31) {
			gprs[rt2] = 0;
		}
	}
	context->uc_mcontext.pc = static_cast<uint64_t>(pc + 4);
	const auto count = recovered.fetch_add(1, std::memory_order_relaxed) + 1;
	if (count <= 4 || count == kBudget) {
		ALOGW("CpuBackend: recovered guest null read #%u/%u (pc=0x%llx vaddr=0x%llx) - "
		      "returning zero and continuing",
		      count, kBudget, static_cast<unsigned long long>(pc),
		      static_cast<unsigned long long>(fault_address));
	}
	return true;
#else
	(void)native_context;
	(void)fault_address;
	return false;
#endif
}

'''

patch(
    cpp,
    [
        # asm/sigcontext.h for fpsimd_context / _aarch64_ctx / FPSIMD_MAGIC.
        (
            block(
                "#if defined(__aarch64__)\n"
                "#include <FEXCore/Utils/ArchHelpers/Arm64.h>\n"
                "#include <ucontext.h>\n"
                "#endif\n",
                eol,
            ),
            block(
                "#if defined(__aarch64__)\n"
                "#include <FEXCore/Utils/ArchHelpers/Arm64.h>\n"
                "#include <ucontext.h>\n"
                "#include <asm/sigcontext.h>\n"
                "#endif\n",
                eol,
            ),
        ),
        # The emulator itself, right before CpuBackend::Instance().
        (
            block("}\n\nCpuBackend* CpuBackend::Instance() {\n", eol),
            block("}\n\n" + fun + "CpuBackend* CpuBackend::Instance() {\n", eol),
        ),
    ],
)

# ---------------------------------------------------------- runtimeLinker.cpp
rl = ROOT / "core/src/loader/runtimeLinker.cpp"
eol = nl(rl)

hook = r'''		// Console Five Pro (v0.0.9proQ1): a guest null-pointer READ used to end the
		// session ("the game itself crashed"). Android cannot map the null page
		// (mmap_min_addr), so the faulting load is emulated against a zero page
		// instead - a one-off hiccup (a race, an HLE gap, an unused pointer) keeps
		// running, while a real null-deref loop still terminates once the recovery
		// budget is spent. Writes are never faked; debug.capy.null_read_recovery=0
		// restores the old behaviour.
		if (access == GpuAccess::Read && info->access_violation_vaddr < 0x10000 &&
		    Cpu::GuestRipFromHostPc(info->exception_address) != 0 &&
		    Cpu::RecoverGuestNullRead(info->native_context, info->access_violation_vaddr)) {
			return true;
		}
'''

patch(
    rl,
    [
        (
            block(
                "		if (access != GpuAccess::Execute && Cpu::GuestRipFromHostPc(info->exception_address) != 0 &&\n"
                "		    Libs::LibKernel::Memory::AdoptFaultingGuestWindow(info->access_violation_vaddr)) {\n"
                "			return true;\n"
                "		}\n",
                eol,
            ),
            block(
                "		if (access != GpuAccess::Execute && Cpu::GuestRipFromHostPc(info->exception_address) != 0 &&\n"
                "		    Libs::LibKernel::Memory::AdoptFaultingGuestWindow(info->access_violation_vaddr)) {\n"
                "			return true;\n"
                "		}\n" + hook,
                eol,
            ),
        )
    ],
)

print("core null-read recovery patched")
