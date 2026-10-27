// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#ifndef COMMON_CONTENT_PROFILE_H
#define COMMON_CONTENT_PROFILE_H

#include <cstdint>
#include <string>

// Content-signature runtime profiles.
//
// The GPU profiles (common/gpuProfile.h) tune the engine for the host device;
// content profiles tune it for what is being run. Some retail content has
// workload characteristics - very high draw counts, long sequential asset
// streams, large hot descriptor sets - that benefit from a different set of
// engine defaults than the general-purpose baseline, while everything else
// must keep the baseline untouched.
//
// Recognition is fingerprint-based and fully opaque:
//
//   * Retail packages and their extracted directories carry standard
//     identifier tokens (publisher prefix, retail content id, internal
//     content marker) in their file and directory names.
//   * Those tokens are reduced to 64-bit FNV-1a fingerprints at runtime and
//     compared against the stored constants below. The token text itself is
//     deliberately absent from this source tree: nothing in the repository
//     names, quotes or embeds any content identity - only these opaque
//     integers, exactly like the per-title hash tables every mainstream
//     emulator uses for patch and compat databases.
//   * A score system decides activation: the internal content marker is
//     decisive on its own (it is unique per title family), the retail
//     content id is strong but not decisive, the publisher prefix is a weak
//     hint only. Near-miss identifiers (one digit off, a sibling release,
//     a different title of the same publisher) score far below the
//     activation threshold.
//
// When a signature matches, profile 500 (a high-throughput preset) applies.
// It is entirely internal: there is no setting, no UI, no log line (unless
// explicitly asked for via debug.capy.profile500_log) and no behaviour change
// for any other content. An explicit user value for any affected knob always
// wins over the profile - the profile only replaces built-in defaults.
namespace Common::ContentProfile {

// Opaque 64-bit fingerprints of the identifier tokens of one retail title
// family (FNV-1a-64 over the token text). See contentProfile.cpp.
inline constexpr uint64_t kSigPublisherPrefix = 0xAAEBC87A374679B9ull;
inline constexpr uint64_t kSigRetailContentId = 0xC4116A7482E39C26ull;
inline constexpr uint64_t kSigContentMarker   = 0x368AFE1ACA517B3Aull;

// Activation threshold. Marker alone (3) activates; content id + publisher
// prefix (2+1) activates; content id alone (2) or publisher alone (1) does
// not - a weak match must never flip the engine into the preset.
inline constexpr int kActivationScore = 3;

/**
 * Whether profile 500 is active for the content currently running.
 * Safe to call from any thread; cheap (an atomic load behind a debug-flag
 * fast path). `debug.capy.profile500`: 1 = force on, 0 = force off,
 * anything else = signature decides (the default).
 */
[[nodiscard]] bool Profile500Enabled();

/**
 * Evaluate the content signature and set the profile state. Called once at
 * the top of Emulator::Run - before any subsystem initialises - with the
 * executable path (a retail package file or an eboot inside an extracted
 * tree) and the app0 directory. Both are optional; an empty path simply
 * contributes no tokens.
 */
void ActivateFromPaths(const std::string& elf_path, const std::string& app0_dir);

/** Test hook: drop any active profile (state returns to "no signature seen"). */
void ResetForTesting();

} // namespace Common::ContentProfile

#endif /* COMMON_CONTENT_PROFILE_H */
