// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#include "loader/fexInstructionFixups.h"

#include <Zydis/Zydis.h>

#include <algorithm>
#include <vector>

namespace Loader {

namespace {

// The canonical 3-byte x86-64 NOP: 0F 1F 00 ("nop dword ptr [rax]"). Same length as the
// instructions it replaces, so every following branch offset stays valid.
constexpr uint8_t kThreeByteNop[3] = {0x0f, 0x1f, 0x00};

bool IsPrivilegedHint(const ZydisDecodedInstruction& instruction) {
	// MONITORX (0F 01 C8-CD area) arms an address range; MWAITX (0F 01 FB) sleeps the
	// thread on it. Both are unprivileged on real Zen 2 silicon and privileged to FEX.
	return instruction.length == 3 &&
	       (instruction.mnemonic == ZYDIS_MNEMONIC_MONITORX ||
	        instruction.mnemonic == ZYDIS_MNEMONIC_MWAITX);
}

} // namespace

uint64_t RewriteFexPrivilegedHints(uint64_t segment_addr, uint64_t segment_size,
                                   std::span<const uintptr_t> function_starts) {
	if (segment_size == 0 || function_starts.empty()) {
		return 0;
	}
	const uint64_t segment_end = segment_addr + segment_size;

	// Keep only the starts that fall inside this segment, sorted and deduplicated; each
	// pair of neighbours then bounds exactly one function body.
	std::vector<uintptr_t> starts;
	starts.reserve(function_starts.size());
	for (const auto start: function_starts) {
		if (start >= segment_addr && start < segment_end) {
			starts.push_back(start);
		}
	}
	std::sort(starts.begin(), starts.end());
	starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
	if (starts.empty()) {
		return 0;
	}

	ZydisDecoder decoder;
	if (!ZYAN_SUCCESS(
	        ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) {
		return 0;
	}

	uint64_t rewritten = 0;
	for (size_t i = 0; i < starts.size(); i++) {
		const uint64_t body_end = i + 1 < starts.size() ? starts[i + 1] : segment_end;
		for (uint64_t address = starts[i]; address < body_end;) {
			ZydisDecodedInstruction instruction;
			if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(
			        &decoder, nullptr, reinterpret_cast<const void*>(address), body_end - address,
			        &instruction))) {
				// Alignment padding or trailing data: decoding stops where the function's
				// stream ends; the next eh_frame start picks up after it.
				break;
			}
			if (IsPrivilegedHint(instruction)) {
				uint8_t* bytes = reinterpret_cast<uint8_t*>(address);
				bytes[0]       = kThreeByteNop[0];
				bytes[1]       = kThreeByteNop[1];
				bytes[2]       = kThreeByteNop[2];
				rewritten++;
			}
			address += instruction.length;
		}
	}
	return rewritten;
}

} // namespace Loader
