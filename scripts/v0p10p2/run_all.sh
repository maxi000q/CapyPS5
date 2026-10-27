#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
# SPDX-License-Identifier: GPL-2.0-only
#
# v0.0.10pro phase 4 - the honest test runner.
#
# Compiles the REAL shipped sources (no mocks, no re-implementations, no
# synthetic FPS models) and runs them on the host. The classifier and the
# policy tables are pure CPU code, so they build and run anywhere; every
# assertion below is the shipped code executing.
#
# What is NOT tested here, on purpose: absolute FPS. No host can measure how a
# game runs on a phone GPU. The on-device recipe (README.md) is the only
# honest way to get that number: the perf overlay's FPS counter (fed by the
# real frame counter) plus the new T500/T501/T502 badge (fed by the real
# classifier lock) show, on the device, whether the turbo engaged and what it
# did to the frame rate.

set -euo pipefail
cd "$(dirname "$0")"

CXX="${CXX:-g++}"
CORE="$(cd ../.. && pwd)/core/src"
CXXFLAGS="-std=c++20 -O2 -Wall -Wextra -I${CORE}"

echo "== building (real shipped sources) =="
"$CXX" $CXXFLAGS -o classifier_test classifier_test.cpp "${CORE}/common/contentProfile.cpp"
"$CXX" $CXXFLAGS -o policy_test policy_test.cpp "${CORE}/common/contentProfile.cpp"

echo
echo "== classifier_test =="
./classifier_test
echo
echo "== policy_test =="
./policy_test

echo
echo "ALL REAL-CODE TESTS PASSED"
echo "reminder: absolute FPS is a device measurement - see README.md for the recipe"
