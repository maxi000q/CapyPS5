// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

// Best-effort performance-cluster detection for Android big.LITTLE SoCs (v0.0.6proplus).
// v0.0.9proQ4 round 2: full topology classification (prime / performance / efficiency).

#pragma once

#if defined(__ANDROID__)

#include "common/contentProfile.h"
#include "common/debugFlags.h"

#include <android/log.h>
#include <sched.h>

#include <atomic>
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
	cpu_set_t all;         // round 4: every core found at classification time
	cpu_set_t efficiency;  // round 4: every detected core outside the combined set
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
		// Round 4: the full detected set and its complement against the fast
		// set. The CPU-bound workload family steps the compile workers down
		// onto exactly this complement (see PinToWorkerCoresForClass).
		CPU_ZERO(&result.all);
		CPU_ZERO(&result.efficiency);
		for (int cpu = 0; cpu < kMaxCpus; ++cpu) {
			if (freq[cpu] == 0) {
				continue;
			}
			CPU_SET(cpu, &result.all);
			if (!CPU_ISSET(cpu, &result.combined)) {
				CPU_SET(cpu, &result.efficiency);
			}
		}
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

// ------------------------------------------------------------------ round 4
// v0.0.9proQ4 round 4: placement follows the runtime workload class
// (common/contentProfile.h). The classes describe the shape of the
// engine's own work, and the two families want opposite placements:
//
//   throughput family (500): compile workers keep the round-2
//   performance-minus-prime tier and the pool may grow by one on
//   backlog - the warm-up compile phase IS the critical path there.
//
//   CPU-bound compositing family (501): the content's own threads are
//   the frame bottleneck, so the compile workers step DOWN onto the
//   efficiency tier (freeing the whole performance tier for the
//   content's threads) and guest threads that start while the family is
//   locked may take a bounded number of placement slots there.

enum class WorkerPlacement : uint32_t {
	PerformanceMinusPrime = 0, // the round-2 default
	Efficiency            = 1, // the CPU-bound family vacates the big cores
};

// Pure decision logic (no scheduler calls) so tests can execute it
// directly.
[[nodiscard]] inline WorkerPlacement WorkerSetForClass(bool cpu_bound_class) {
	return cpu_bound_class ? WorkerPlacement::Efficiency
	                       : WorkerPlacement::PerformanceMinusPrime;
}

// Pins the calling (worker) thread per the current class. Falls back to
// the round-2 placement whenever the class does not ask for the
// step-down or the efficiency tier is empty (single-cluster SoC,
// locked-down sysfs). Toggles: debug.capy.worker_cores (0 = the Q2
// behaviour) and the class kill-switches debug.capy.profile500 /
// debug.capy.profile501.
inline void PinToWorkerCoresForClass(const char* who) {
	static DebugFlags::CachedValue enabled("debug.capy.worker_cores", 1);
	if (enabled.Get() == 0) {
		PinToPerformanceCores(who);
		return;
	}
	const auto& topology = ClassifyCores();
	const auto  placement =
	    WorkerSetForClass(Common::ContentProfile::Profile501Enabled());
	cpu_set_t   cores;
	if (placement == WorkerPlacement::Efficiency && CPU_COUNT(&topology.efficiency) > 0) {
		cores = topology.efficiency;
	} else if (topology.split && CPU_COUNT(&topology.performance) > 0) {
		cores = topology.performance;
	} else if (CPU_COUNT(&topology.combined) > 0) {
		cores = topology.combined;
	} else {
		return; // nothing detected: leave the affinity untouched
	}
	if (sched_setaffinity(0, sizeof(cores), &cores) == 0) {
		__android_log_print(ANDROID_LOG_INFO, "CapyPS5-Core",
		                    "%s: pinned to %d core(s) (class placement)",
		                    who, CPU_COUNT(&cores));
	}
}

// Guest-thread placement under the CPU-bound family: the content's own
// threads get the tier the compile workers just vacated. Slots bound
// the takeover so a thread-storming title cannot pile every thread onto
// the big cores; threads beyond the slots (and threads that started
// before the family locked) keep the scheduler's own balancing -
// documented best effort.
inline constexpr int kGuestPlacementSlots = 4;

inline std::atomic<int>& GuestPlacementSlotsFree() {
	static std::atomic<int> free_slots {kGuestPlacementSlots};
	return free_slots;
}

thread_local inline bool      t_guest_placement_taken = false;
thread_local inline cpu_set_t t_guest_placement_saved {};

// Pure decision logic for tests: does the current class want guest
// threads placed at all?
[[nodiscard]] inline bool GuestPlacementWanted(bool cpu_bound_class) {
	return cpu_bound_class;
}

// Called once at guest-thread start (kernel/pthread.cpp RunThread).
// Returns whether this thread took a slot; ReleaseGuestThreadPlacement
// (from the thread cleanup) gives it back. The original affinity mask is
// saved and restored on release, so a spawning thread's own placement
// never leaks into its children.
inline bool AcquireGuestThreadPlacement(const char* who) {
	// One placement per thread, ever: a second acquire from the same
	// thread is already placed (and its slot is already held) - returning
	// true keeps the call idempotent instead of leaking a slot.
	if (t_guest_placement_taken) {
		return true;
	}
	if (!GuestPlacementWanted(Common::ContentProfile::Profile501Enabled())) {
		return false;
	}
	auto& slots    = GuestPlacementSlotsFree();
	int   expected = slots.load(std::memory_order_relaxed);
	while (expected > 0 &&
	       !slots.compare_exchange_weak(expected, expected - 1,
	                                    std::memory_order_relaxed)) {
	}
	if (expected <= 0) {
		return false; // slots full: stay with the scheduler's balancing
	}
	const auto& topology = ClassifyCores();
	cpu_set_t   cores;
	if (topology.split && CPU_COUNT(&topology.performance) > 0) {
		cores = topology.performance;
	} else if (CPU_COUNT(&topology.combined) > 0 &&
	           CPU_COUNT(&topology.combined) < topology.detected) {
		cores = topology.combined;
	} else {
		// no distinct tier to place onto: give the slot back
		slots.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	cpu_set_t current;
	if (sched_getaffinity(0, sizeof(current), &current) == 0) {
		t_guest_placement_saved = current;
	}
	if (sched_setaffinity(0, sizeof(cores), &cores) == 0) {
		t_guest_placement_taken = true;
		__android_log_print(ANDROID_LOG_INFO, "CapyPS5-Core",
		                    "%s: guest thread placed onto %d big core(s)",
		                    who, CPU_COUNT(&cores));
		return true;
	}
	slots.fetch_add(1, std::memory_order_relaxed); // the pin failed
	return false;
}

// Called from the guest-thread cleanup handler; a no-op for threads that
// never took a placement slot.
inline void ReleaseGuestThreadPlacement() {
	if (!t_guest_placement_taken) {
		return;
	}
	t_guest_placement_taken = false;
	GuestPlacementSlotsFree().fetch_add(1, std::memory_order_relaxed);
	sched_setaffinity(0, sizeof(cpu_set_t), &t_guest_placement_saved);
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
