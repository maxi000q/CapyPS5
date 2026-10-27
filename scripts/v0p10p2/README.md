# v0.0.10pro phase 4 - honest tests

Two host binaries, compiled from the **shipped, unmodified** engine sources:

| binary | compiled from | proves |
|---|---|---|
| `classifier_test` | `core/src/common/contentProfile.cpp` | the runtime classifier, executed, locks forward-rendered 3D into the new **Game502Turbo** family (class 502), closes the old dead zones (fully opaque scenes; the 0.45..0.55 transparency gap), leaves compositing (501), deferred (500), menus and 2D content unchanged, resets between games, and latches the 4K resolution policy |
| `policy_test` | `core/src/common/enginePolicy.h`, `renderPassMode.h`, `contentProfile.cpp` | the 4K policy now **floors the engine tier at Throughput500 on every vendor** (at Q4 it moved no knob at all on Mali - the "4K added literally nothing" report), the new Forward3D502 row carries the throughput preset, the pre-existing rows are byte-identical to round 5, and the clear-mode selection table is unchanged |

Run:

```sh
./run_all.sh
```

## What these tests do NOT do

They do **not** print frame rates. No host can measure how a game runs on a
phone GPU, and a modelled FPS number is not a measurement - that mistake was
made once before (the phase-3 round reported "modelled" gains as if they were
results). Every number these tests print is either a check count or a class
constant the real code produced.

## The honest on-device FPS recipe

Absolute FPS must be measured on the target device, against the old build, in
the same game spot. The app already carries the instruments:

1. **FPS counter** - Settings -> performance overlay. It reads the real guest
   frame counter (EMA-smoothed, ~1 s reaction); it is the number to compare.
2. **Turbo badge** - the overlay now shows `T500` / `T501` / `T502` once the
   runtime classifier locks a family for the running game. Before this phase
   a forward-rendered 3D title showed no badge at all - that was the bug: no
   class ever locked, no turbo ever engaged.
3. **Class lock log** - `adb shell setprop debug.capy.workload_log 1` prints
   every 32-frame window's measured mix (`draws/frame`, `alpha`, `mesh`,
   `mrt`, `idx`, `dispatch/frame`, `maxpx`) and the resulting lock - the raw
   evidence of what the engine saw and what it decided.
4. **A/B the tag** - install the `v0.0.9proQ4` build and this build, same
   game, same spot, same settings, and compare the overlay numbers. That
   comparison - and only that - is a real FPS delta.
