// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only
//
// v0.0.10pro phase 4 - classifier test (REAL CODE, host).
//
// This binary compiles and links the SHIPPED core/src/common/contentProfile.cpp
// (plus its headers, unmodified) and drives it through its public API the way
// the engine does: ObserveDraw()/ObserveDispatch() from the command-processing
// thread's perspective, OnFrameEnd() once per guest frame. It asserts what the
// classifier actually locks for a set of measured command-stream shapes.
//
// What this proves: the shipped classification code, executed, classifies
// forward-rendered 3D (the Game502Turbo family) and the previously dead-zone
// mixes into a turbo class, keeps compositing/deferred behaviour unchanged,
// keeps menus and 2D content out, and latches the resolution policy.
//
// What this does NOT prove: absolute FPS on any device - that needs real
// content on real hardware (see README.md for the honest on-device recipe).

#include "common/contentProfile.h"

#include <cstdio>
#include <cstdlib>

using Common::ContentProfile::ActiveClass;
using Common::ContentProfile::HighResolutionPolicyActive;
using Common::ContentProfile::ObserveDispatch;
using Common::ContentProfile::ObserveDraw;
using Common::ContentProfile::OnFrameEnd;
using Common::ContentProfile::ResetForNextGame;
using Common::ContentProfile::WorkloadClass;

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK_EQ(actual, expected, label)                                                  \
	do {                                                                                   \
		g_checks++;                                                                        \
		if ((actual) != (expected)) {                                                      \
			g_failures++;                                                                  \
			std::printf("  FAIL %s: expected %u, got %u\n", label,                         \
			            static_cast<uint32_t>(expected), static_cast<uint32_t>(actual));    \
		} else {                                                                           \
			std::printf("  ok   %s\n", label);                                             \
		}                                                                                  \
	} while (0)

// Shape of one synthetic guest frame. All values are counts and ratios of the
// engine's own work - exactly what the producers see on device.
struct FrameShape {
	uint32_t draws          = 0;
	uint32_t index_per_draw = 0;
	double   blend_fraction = 0.0; // 0..1
	double   mesh_fraction  = 0.0; // 0..1
	double   mrt_fraction   = 0.0; // 0..1
	uint32_t dispatches     = 0;
	uint32_t target_w       = 1920;
	uint32_t target_h       = 1080;
};

static void FeedFrame(const FrameShape& shape) {
	const uint32_t blend_draws = static_cast<uint32_t>(shape.draws * shape.blend_fraction);
	const uint32_t mesh_draws  = static_cast<uint32_t>(shape.draws * shape.mesh_fraction);
	const uint32_t mrt_draws   = static_cast<uint32_t>(shape.draws * shape.mrt_fraction);
	for (uint32_t i = 0; i < shape.draws; i++) {
		ObserveDraw(shape.index_per_draw, 1, i < blend_draws, i < mrt_draws ? 2u : 1u,
		            i < mesh_draws, shape.target_w, shape.target_h);
	}
	for (uint32_t i = 0; i < shape.dispatches; i++) {
		ObserveDispatch();
	}
	OnFrameEnd();
}

// Feed `windows` full classification windows (32 frames each) and return what
// the classifier holds afterwards.
static WorkloadClass LockAfter(uint32_t windows, const FrameShape& shape) {
	const uint32_t frames = windows * 32 + 2; // two extra frames: not another window
	for (uint32_t f = 0; f < frames; f++) {
		FeedFrame(shape);
	}
	return ActiveClass();
}

int main() {
	std::printf("classifier test (real shipped contentProfile.cpp)\n");

	// ---------------------------------------------------------------- A
	// Forward-rendered 3D, fully opaque, single target, no mesh/compute:
	// the plain action-game shape. At v0.0.9proQ4 this classified as NOTHING
	// (family 500 demanded alpha >= 0.02, MRT >= 4% and mesh-or-compute) and
	// the engine kept the General preset forever - the on-device "the turbo
	// did nothing" report. Now: Game502Turbo.
	{
		ResetForNextGame();
		FrameShape forward3d;
		forward3d.draws          = 400;
		forward3d.index_per_draw = 6000;
		forward3d.blend_fraction = 0.0;
		forward3d.mesh_fraction  = 0.0;
		forward3d.mrt_fraction   = 0.0;
		forward3d.dispatches     = 0;
		std::printf("A: forward 3D, opaque, single target\n");
		CHECK_EQ(LockAfter(4, forward3d), WorkloadClass::Turbo502, "locks Turbo502 (was None)");
	}

	// ---------------------------------------------------------------- B
	// Forward 3D inside the old 0.45..0.55 transparency dead zone: matched
	// neither family 500 (alpha <= 0.45) nor 501 (alpha >= 0.55) at Q4.
	{
		ResetForNextGame();
		FrameShape deadzone;
		deadzone.draws          = 300;
		deadzone.index_per_draw = 8000;
		deadzone.blend_fraction = 0.50;
		std::printf("B: forward 3D, alpha 0.50 (old dead zone)\n");
		CHECK_EQ(LockAfter(4, deadzone), WorkloadClass::Turbo502, "locks Turbo502 (was None)");
	}

	// ---------------------------------------------------------------- C
	// Small-scene forward 3D: few, very large draws. 70 draws/frame is above
	// the 60-draw family floor; the lowered window minimum (1920 < 2000) lets
	// the window count as signal.
	{
		ResetForNextGame();
		FrameShape small;
		small.draws          = 70;
		small.index_per_draw = 40000;
		small.blend_fraction = 0.1;
		std::printf("C: small-scene forward 3D (70 large draws/frame)\n");
		CHECK_EQ(LockAfter(4, small), WorkloadClass::Turbo502, "locks Turbo502");
	}

	// ---------------------------------------------------------------- D
	// Layered alpha compositing (the Game501Turbo family): behaviour must not
	// change with the new family in the chain.
	{
		ResetForNextGame();
		FrameShape sprites;
		sprites.draws          = 2000;
		sprites.index_per_draw = 6;
		sprites.blend_fraction = 0.9;
		sprites.mesh_fraction  = 0.0;
		std::printf("D: sprite compositing\n");
		CHECK_EQ(LockAfter(4, sprites), WorkloadClass::Turbo501, "still locks Turbo501");
	}

	// ---------------------------------------------------------------- E
	// Deferred-style high-throughput 3D (Game500Turbo): unchanged.
	{
		ResetForNextGame();
		FrameShape deferred;
		deferred.draws          = 300;
		deferred.index_per_draw = 30000;
		deferred.blend_fraction = 0.2;
		deferred.mesh_fraction  = 0.2;
		deferred.mrt_fraction   = 0.5;
		deferred.dispatches     = 20;
		std::printf("E: deferred 3D (MRT + mesh + compute)\n");
		CHECK_EQ(LockAfter(4, deferred), WorkloadClass::Turbo500, "still locks Turbo500");
	}

	// ---------------------------------------------------------------- F
	// Opaque DEFERRED 3D: alpha 0.0. At Q4 the alpha >= 0.02 lower bound
	// pushed even MRT+mesh scenes out of family 500; now the band is [0, 0.55).
	{
		ResetForNextGame();
		FrameShape opaque_deferred;
		opaque_deferred.draws          = 300;
		opaque_deferred.index_per_draw = 30000;
		opaque_deferred.blend_fraction = 0.0;
		opaque_deferred.mesh_fraction  = 0.2;
		opaque_deferred.mrt_fraction   = 0.5;
		opaque_deferred.dispatches     = 20;
		std::printf("F: deferred 3D, fully opaque (old alpha >= 0.02 gap)\n");
		CHECK_EQ(LockAfter(4, opaque_deferred), WorkloadClass::Turbo500, "locks Turbo500 (was None)");
	}

	// ---------------------------------------------------------------- G
	// Sparse menu: no classification signal, no lock.
	{
		ResetForNextGame();
		FrameShape menu;
		menu.draws          = 30;
		menu.index_per_draw = 500;
		menu.blend_fraction = 0.5;
		std::printf("G: sparse menu (30 draws/frame)\n");
		CHECK_EQ(LockAfter(4, menu), WorkloadClass::None, "stays None");
	}

	// ---------------------------------------------------------------- H
	// Sprite-scale 2D with moderate transparency: draws are tiny (6 indices),
	// far below the 700-index geometry bar - must NOT be pulled into 502.
	{
		ResetForNextGame();
		FrameShape flat2d;
		flat2d.draws          = 500;
		flat2d.index_per_draw = 6;
		flat2d.blend_fraction = 0.3;
		std::printf("H: 2D sprite-scale game (alpha 0.3)\n");
		CHECK_EQ(LockAfter(4, flat2d), WorkloadClass::None, "stays None (geometry bar holds)");
	}

	// ---------------------------------------------------------------- I
	// Resolution latch: two consecutive windows of multi-megapixel targets
	// (3840x2160 = 8.29M pixels) flip the resolution policy on. Small targets
	// never do. The policy is what floors the engine tier at Throughput500 in
	// enginePolicy.h - the fix for "4K changed nothing on Mali".
	{
		ResetForNextGame();
		CHECK_EQ(HighResolutionPolicyActive(), false, "4K policy off before any 4K work");
		FrameShape game4k;
		game4k.draws          = 400;
		game4k.index_per_draw = 6000;
		game4k.target_w       = 3840;
		game4k.target_h       = 2160;
		LockAfter(3, game4k);
		std::printf("I: 4K targets latch the resolution policy\n");
		CHECK_EQ(HighResolutionPolicyActive(), true, "4K policy on after two 4K windows");
	}
	{
		ResetForNextGame();
		FrameShape game1080;
		game1080.draws          = 400;
		game1080.index_per_draw = 6000;
		LockAfter(3, game1080);
		CHECK_EQ(HighResolutionPolicyActive(), false, "1080p targets never latch it");
	}

	// ---------------------------------------------------------------- J
	// Reset semantics: a second game never inherits the first game's class.
	{
		ResetForNextGame();
		FrameShape forward3d;
		forward3d.draws          = 400;
		forward3d.index_per_draw = 6000;
		LockAfter(4, forward3d);
		CHECK_EQ(ActiveClass(), WorkloadClass::Turbo502, "first game locked 502");
		ResetForNextGame();
		CHECK_EQ(ActiveClass(), WorkloadClass::None, "reset clears the lock");
		FrameShape sprites;
		sprites.draws          = 2000;
		sprites.index_per_draw = 6;
		sprites.blend_fraction = 0.9;
		CHECK_EQ(LockAfter(4, sprites), WorkloadClass::Turbo501,
		         "second game locks its own class (no inheritance)");
	}

	// ---------------------------------------------------------------- K
	// Re-lock hysteresis: a 502 game that walks into a compositing-heavy menu
	// for a few windows keeps its class; four decisive windows change it.
	{
		ResetForNextGame();
		FrameShape forward3d;
		forward3d.draws          = 400;
		forward3d.index_per_draw = 6000;
		LockAfter(4, forward3d);
		FrameShape sprites;
		sprites.draws          = 2000;
		sprites.index_per_draw = 6;
		sprites.blend_fraction = 0.9;
		LockAfter(3, sprites);
		std::printf("K: re-lock hysteresis\n");
		CHECK_EQ(ActiveClass(), WorkloadClass::Turbo502, "3 disagreeing windows do not re-lock");
		LockAfter(2, sprites);
		CHECK_EQ(ActiveClass(), WorkloadClass::Turbo501, "4 decisive windows re-lock to 501");
	}

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
