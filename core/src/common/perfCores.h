// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

// Best-effort performance-cluster detection for Android big.LITTLE SoCs (v0.0.6proplus).
// v0.0.9proQ4 round 2: full topology classification (prime / performance / efficiency).

#pragma once

#if defined(__ANDROID__)

#include "common/debugFlags.h"

#include <android/log.h>
#include <sched.h>

#include <cstdint>
#include <cstdio>

namespace Common {

// ------------------------------------------------------------------ classification
//
// Modern flagship SoCs expose three tiers (Snapdragon 8 Gen 3: 1x Cortex-X4 prime,
// 3x A720 performance, 4x A520 efficiency; Dimensity 9300: 4x X4 + 4x A720). The
// Q2/Q3 code lumped everything within 90% of the top frequency into one
// "performance" set and pinned EVERY hot thread class onto it:
//
//   emulator thread + GPU thread + 4-5 pipeline workers + the present thread
//
// which is 7-8 threads fighting over 4 cores while the efficiency cluster sits
// idle. Round 2 classifies the tiers explicitly and gives each thread class its
// own place:
//
//   emulator + GPU threads  -> prime + performance (as in Q2 - proven)
//   pipeline workers        -> performance MINUS prime (the prime core stays
//                              exclusive to the two most critical threads)
//   present thread          -> not pinned at all (as in Q2): it sleeps ~98% of
//                              the time and its per-vblank burst is tiny, and
//                              pinning it onto the busy cluster (the Q3 change)
//                              only added preemption + cache pollution there.
//
// Everything is best effort: a single-cluster SoC, locked-down sysfs or an
// unexpected topology simply falls back to the previous behaviour.

struct CoreTopology {
	cpu_set_t prime;       // cores of the top-frequency cluster (usually one Cortex-X)
	cpu_set_t performance; // every core within 90% of the top frequency, minus prime
	cpu_set_t combined;    // prime | performance - the Q2 "performance" set
	int       detected = 0;  // CPUs found in sysfs (online at classification time)
	bool      split = false; // prime and performance are genuinely distinct tiers
};

// Reads each CPU's cluster id and top frequency and fills the three sets.
// Result is cached (sysfs walks are not free); safe to call from any thread.
inline const CoreTopology& ClassifyCores() {
	static CoreTopology topology = [] {
		CoreTopology result;
		CPU_ZERO(&result.prime);
		CPU_ZERO(&result.performance);
		CPU_ZERO(&result.combined);

		constexpr int kMaxCpus = 64;
		uint64_t      freq[kMaxCpus] = {};
		uint32_t      cluster[kMaxCpus] = {};
		bool          have_cluster = false;
		int           cpus = 0;
		uint64_t      best = 0;

		for (int cpu = 0; cpu < kMaxCpus; ++cpu) {
			char path[128];
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
			// cluster_id appeared in kernel 5.10-ish; missing on some vendors.
			std::snprintf(path, sizeof(path),
			              "/sys/devices/system/cpu/cpu%d/topology/cluster_id", cpu);
			file = std::fopen(path, "re");
			if (file != nullptr) {
				unsigned long long id = 0;
				if (std::fscanf(file, "%llu", &id) == 1) {
					cluster[cpu] = static_cast<uint32_t>(id);
					have_cluster = true;
				}
				std::fclose(file);
			}
		}
		result.detected = cpus;
		if (best == 0 || cpus == 0) {
			return result; // nothing detected: all sets stay empty, callers fall back
		}

		// The "within 90% of the top frequency" rule of the Q2 detector.
		bool fast[kMaxCpus] = {};
		int   fast_count = 0;
		for (int cpu = 0; cpu < kMaxCpus; ++cpu) {
			if (freq[cpu] >= best * 9 / 10) {
				fast[cpu] = true;
				++fast_count;
			}
		}

		// The prime tier: the cluster that owns the single fastest core, when it is
		// small (1-2 cores) AND other fast cores exist outside it (a distinct
		// performance tier). Otherwise there is no meaningful prime/performance
		// split and everything fast is one tier.
		int  fastest_cpu = -1;
		for (int cpu = 0; cpu < kMaxCpus; ++cpu) {
			if (freq[cpu] == best) {
				fastest_cpu = cpu;
				break;
			}
		}
		int prime_cluster_count = 0;
		int fast_outside = 0;
		if (fastest_cpu >= 0) {
			for (int cpu = 0; cpu < kMaxCpus; ++cpu) {
				if (freq[cpu] == 0) {
					continue;
				}
				const bool same_cluster =
				    have_cluster && cluster[cpu] == cluster[fastest_cpu];
				const bool same_freq_bucket = freq[cpu] == best;
				if (same_cluster || same_freq_bucket) {
					++prime_cluster_count;
				} else if (fast[cpu]) {
					++fast_outside;
				}
			}
		}
		const bool prime_is_distinct =
		    fastest_cpu >= 0 && prime_cluster_count > 0 && prime_cluster_count <= 2 && fast_outside > 0;

		for (int cpu = 0; cpu < kMaxCpus; ++cpu) {
			if (freq[cpu] == 0) {
				continue;
			}
			const bool in_prime_tier =
			    prime_is_distinct &&
			    ((have_cluster && cluster[cpu] == cluster[fastest_cpu]) || freq[cpu] == best);
			if (in_prime_tier) {
				CPU_SET(cpu, &result.prime);
			} else if (fast[cpu]) {
				CPU_SET(cpu, &result.performance);
			}
		}
		result.combined = result.prime;
		CPU_OR(&result.combined, &result.combined, &result.performance);
		result.split = CPU_COUNT(&result.prime) > 0 && CPU_COUNT(&result.performance) > 0;
		return result;
	}();
	return topology;
}

// ------------------------------------------------------------------ placement API

// The Q2 detector, unchanged in behaviour: everything within 90% of the top
// frequency (prime + performance). Used by the emulator and GPU threads.
inline bool PerformanceCores(cpu_set_t* out) {
	if (out == nullptr) {
		return false;
	}
	const auto& topology = ClassifyCores();
	if (CPU_COUNT(&topology.combined) == 0) {
		return false;
	}
	*out = topology.combined;
	// A single-cluster SoC (every core in the set) has nothing to prefer - the
	// Q2 detector returned false there and callers left the affinity untouched.
	return CPU_COUNT(out) > 0 && CPU_COUNT(out) < topology.detected;
}

// Pins the calling thread to the prime + performance tiers (the Q2 behaviour).
// Toggle: debug.capy.perf_cores (1).
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

// v0.0.9proQ4 round 2: background pipeline workers are pinned to the performance
// tier WITHOUT the prime core(s), so the one or two hottest threads (emulation
// and GPU command processing) keep the fastest core to themselves while a worker
// storm saturates the rest. Falls back to the combined tier on single-tier SoCs.
// Toggle: debug.capy.worker_cores (1); debug.capy.worker_cores 0 = Q2 behaviour.
inline void PinToWorkerCores(const char* who) {
	static DebugFlags::CachedValue enabled("debug.capy.worker_cores", 1);
	if (enabled.Get() == 0) {
		PinToPerformanceCores(who);
		return;
	}
	const auto& topology = ClassifyCores();
	cpu_set_t   cores;
	if (topology.split && CPU_COUNT(&topology.performance) > 0) {
		cores = topology.performance;
	} else if (CPU_COUNT(&topology.combined) > 0) {
		cores = topology.combined;
	} else {
		return; // nothing detected: leave the affinity untouched
	}
	if (sched_setaffinity(0, sizeof(cores), &cores) == 0) {
		__android_log_print(ANDROID_LOG_INFO, "CapyPS5-Core",
		                    "%s: pinned to %d worker core(s) (prime reserved)",
		                    who, CPU_COUNT(&cores));
	}
}

// v0.0.9proQ4 round 2: the present thread must NOT be pinned onto the busy
// cluster (the Q3 change - it added preemption and cache pollution on the cores
// the emulation threads need, for a thread that sleeps ~98% of the time). The
// Q2 behaviour (floating placement) is restored. Kept as an explicit query so
// the intent is documented at the call site.
inline bool PresentThreadShouldPin() {
	static DebugFlags::CachedValue pin("debug.capy.pin_present", 0);
	return pin.Get() != 0;
}

} // namespace Common

#endif
