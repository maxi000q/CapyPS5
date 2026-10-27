// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#include "common/contentProfile.h"

#include "common/debugFlags.h"

#include <atomic>
#include <cstddef>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace Common::ContentProfile {

namespace {

// FNV-1a-64 (the same parameters the loader uses elsewhere in the project).
constexpr uint64_t kFnvOffsetBasis = 0xCBF29CE484222325ull;
constexpr uint64_t kFnvPrime       = 0x100000001B3ull;

// 0 = no signature evaluated yet (or none matched), 1 = profile active.
std::atomic<uint32_t> g_profile_active {0};

// Uppercase an ASCII character; digits and letters pass through, everything
// else becomes 0 and acts as a token delimiter. Retail identifier strings
// are conventionally uppercase already; folding case makes the signature
// robust against renamed-lowercase copies without changing the fingerprint
// domain.
inline char UpperAscii(char c) {
	if (c >= 'a' && c <= 'z') {
		return static_cast<char>(c - 'a' + 'A');
	}
	if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
		return c;
	}
	return '\0';
}

// Points `begin`/`end` at the last path segment of `text` ("/" separated,
// also tolerating "\" for desktop paths). An empty input yields an empty
// segment. No filesystem calls: the engine only ever splits the string it
// was handed, so a malformed path can neither throw nor touch the disk.
void LastPathSegment(const std::string& text, size_t& begin, size_t& end) {
	begin = 0;
	end   = 0;
	if (text.empty()) {
		return;
	}
	size_t start = text.find_last_of("/\\");
	if (start == std::string::npos) {
		start = 0;
	} else {
		start++;
	}
	if (start >= text.size()) {
		return;
	}
	begin = start;
	end   = text.size();
}

// Points `begin`/`end` at the second-to-last path segment (the parent
// directory name). Extracted trees are usually named after their content
// identifier, so the directory carries the same fingerprint material as the
// package file name would.
void ParentPathSegment(const std::string& text, size_t& begin, size_t& end) {
	begin = 0;
	end   = 0;
	if (text.empty()) {
		return;
	}
	size_t last = text.find_last_of("/\\");
	if (last == std::string::npos || last == 0) {
		return;
	}
	size_t stop = last;
	size_t prev = text.find_last_of("/\\", last - 1);
	size_t start = (prev == std::string::npos) ? 0 : prev + 1;
	if (start >= stop) {
		return;
	}
	begin = start;
	end   = stop;
}

// Hash one token (a maximal run of alphanumeric characters) with FNV-1a-64.
// Called once per token per path segment; token counts are tiny (a retail
// identifier contributes at most a handful of tokens).
uint64_t HashToken(const std::string& text, size_t begin, size_t end) {
	uint64_t hash = kFnvOffsetBasis;
	for (size_t i = begin; i < end; i++) {
		const char folded = UpperAscii(text[i]);
		if (folded == '\0') {
			// Delimiters end the current token; the caller restarts hashing
			// at the next alphanumeric character.
			break;
		}
		hash ^= static_cast<uint64_t>(static_cast<unsigned char>(folded));
		hash *= kFnvPrime;
	}
	return hash;
}

// Weight of a single token fingerprint. The marker is a unique internal
// codename - seeing it is identification. The retail content id is strong
// but regional variants and sibling packages reuse prefixes, so it needs
// corroboration. The publisher prefix is a weak hint: whole catalogues
// share it.
int TokenWeight(uint64_t fingerprint) {
	if (fingerprint == kSigContentMarker) {
		return 3;
	}
	if (fingerprint == kSigRetailContentId) {
		return 2;
	}
	if (fingerprint == kSigPublisherPrefix) {
		return 1;
	}
	return 0;
}

// Score one path segment: split it into alphanumeric tokens, hash each,
// accumulate the weights of any fingerprint hits.
void ScoreSegment(const std::string& text, size_t begin, size_t end, int& score) {
	size_t i = begin;
	while (i < end) {
		// Skip delimiters.
		while (i < end && UpperAscii(text[i]) == '\0') {
			i++;
		}
		if (i >= end) {
			break;
		}
		const uint64_t fingerprint = HashToken(text, i, end);
		score += TokenWeight(fingerprint);
		// Advance past the token.
		while (i < end && UpperAscii(text[i]) != '\0') {
			i++;
		}
	}
}

// Score a full path by its last two segments (file name and parent
// directory name). Anything deeper in the hierarchy is deliberately ignored:
// shared directories ("imported", "games") are not content identifiers and
// must not influence the score.
int ScorePath(const std::string& path) {
	int score = 0;
	if (path.empty()) {
		return score;
	}
	size_t begin = 0;
	size_t end   = 0;
	LastPathSegment(path, begin, end);
	if (end > begin) {
		ScoreSegment(path, begin, end, score);
	}
	ParentPathSegment(path, begin, end);
	if (end > begin) {
		ScoreSegment(path, begin, end, score);
	}
	return score;
}

} // namespace

void ActivateFromPaths(const std::string& elf_path, const std::string& app0_dir) {
	// The executable path is the primary carrier: for package-based content
	// it is the package file itself (whose name is the full identifier), for
	// extracted trees it is .../eboot.bin with the content identifier in the
	// parent directory name. The app0 directory name is a third look at the
	// same material; it can only add weight, never fabricate a match on its
	// own (a weak publisher token alone stays below the threshold).
	int score = 0;
	score += ScorePath(elf_path);
	score += ScorePath(app0_dir);

	const bool active = score >= kActivationScore;
	g_profile_active.store(active ? 1u : 0u, std::memory_order_release);

	// Fully silent by default. The telemetry flag exists for on-device
	// diagnosis of the fingerprint engine itself; it never prints content
	// identity, only the score and the resulting state.
	static DebugFlags::CachedValue telemetry("debug.capy.profile500_log", 0);
	if (telemetry.Get() != 0) {
#if defined(__ANDROID__)
		__android_log_print(ANDROID_LOG_INFO, "CapyPS5-Profile",
		                    "content signature score %d -> profile 500 %s",
		                    score, active ? "active" : "inactive");
#else
		(void)score;
		(void)active;
#endif
	}
}

bool Profile500Enabled() {
	// Emergency kill-switch / force-on for debugging. The signature result
	// is the default; the flag only exists so a misbehaving device can be
	// diagnosed without a rebuild.
	static DebugFlags::CachedValue force("debug.capy.profile500", -1);
	switch (force.Get()) {
		case 1: return true;
		case 0: return false;
		default: break;
	}
	return g_profile_active.load(std::memory_order_acquire) != 0;
}

void ResetForTesting() {
	g_profile_active.store(0u, std::memory_order_release);
}

} // namespace Common::ContentProfile
