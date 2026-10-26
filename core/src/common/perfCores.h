// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

// Best-effort performance-cluster detection for Android big.LITTLE SoCs (v0.0.6proplus).

#pragma once

#if defined(__ANDROID__)

#include "common/debugFlags.h"

#include <android/log.h>
#include <sched.h>

#include <cstdint>
#include <cstdio>

namespace Common {

// Reads each CPU's top frequency and fills `out` with the fastest cluster (every core
// within 90% of the maximum). Returns false for single-cluster SoCs, locked-down sysfs
// or unexpected topologies - callers then leave the affinity mask untouched.
inline bool PerformanceCores(cpu_set_t* out) {
	if (out == nullptr) {
		return false;
	}
	CPU_ZERO(out);

	uint64_t best = 0;
	int      cpus = 0;
	uint64_t freq[64] {};
	for (int cpu = 0; cpu < 64; ++cpu) {
		char path[96];
		std::snprintf(path, sizeof(path),
		              "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
		FILE* file = std::fopen(path, "re");
		if (file == nullptr) {
			continue; // a hole in the numbering ends the scan
		}
		unsigned long long value = 0;
		const int          read  = std::fscanf(file, "%llu", &value);
		std::fclose(file);
		if (read != 1 || value == 0) {
			continue;
		}
		freq[cpu] = value;
		++cpus;
		if (value > best) {
			best = value;
		}
	}
	if (best == 0 || cpus == 0) {
		return false;
	}

	int pinned = 0;
	for (int cpu = 0; cpu < 64; ++cpu) {
		if (freq[cpu] >= best * 9 / 10) {
			CPU_SET(cpu, out);
			++pinned;
		}
	}
	return pinned > 0 && pinned < cpus; // one cluster only: nothing to prefer
}

// Pins the calling thread to the performance cluster. Toggle: debug.capy.perf_cores (1).
inline void PinToPerformanceCores(const char* who) {
	static DebugFlags::CachedValue enabled("debug.capy.perf_cores", 1);
	if (enabled.Get() == 0) {
		return;
	}
	cpu_set_t cores;
	if (!PerformanceCores(&cores)) {
		return;
	}
	if (sched_setaffinity(0, sizeof(cores), &cores) == 0) {
		__android_log_print(ANDROID_LOG_INFO, "CapyPS5-Core",
		                    "%s: pinned to %d performance core(s)", who, CPU_COUNT(&cores));
	}
}

} // namespace Common

#endif
