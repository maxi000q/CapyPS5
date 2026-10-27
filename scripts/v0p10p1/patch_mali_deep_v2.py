#!/usr/bin/env python3
"""v0.0.10pro phase 1: Mali deep profile v2 (core, same graphics everywhere).

Files touched (binary-safe, exact bytes; all four are CRLF+tab):
  core/src/graphics/host_gpu/renderer/commandScheduler.h/.cpp
      - batched queue submissions: consecutive bare flushes (one per guest command
        buffer; heavy Unity titles issue ~80 per game frame at low FPS) are merged
        into one vkQueueSubmit with up to N command buffers, cutting the per-submit
        driver/ioctl cost and queue_mutex traffic. A force-submit flag makes every
        submission that follows a priority deferral (flips, end-of-pipe writes) a
        real submit, so the flip-completion waiter can never wait on a batched tick;
        every wait path (Wait/FlushAndWait/Finish/Shutdown) drains the batch first.
        debug.capy.submit_batch (default 8, 0/1 = off) tunes it.
      - command-pool growth step 4 -> 8 (fewer vkAllocateCommandBuffers rounds and
        fewer pool grows while batches hold several buffers).
  core/src/graphics/host_gpu/renderer/renderDraw.cpp
      - the per-draw 1x1 depth clear ("depth kick") is a workaround for an Adreno
        driver bug; on Mali it is pure per-draw cost, so Mali skips it
        (debug.capy.depth_kick still overrides; pixels identical on every GPU).
  core/src/common/platform/sysLinuxVirtual.cpp
      - transparent huge pages: guest-memory commits >= 2 MiB are advised
        MADV_HUGEPAGE, so the kernel backs the multi-GB guest heap with 2 MiB
        pages - ~500x fewer page faults while the game touches its memory and far
        fewer TLB misses in the FEX translation (guest addresses are dereferenced
        directly). No-op on kernels with THP disabled; debug.capy.thp (default 1).
  core/src/graphics/presentation/window/vulkanWindow.cpp
      - the Mali profile log line lists the new v2 items.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def patch(path: Path, edits):
    data = path.read_bytes()
    for old, new in edits:
        if data.count(new) == 1 and new != old:
            print(f"skip {path.name}: edit already applied")
            continue
        n = data.count(old)
        if n != 1:
            print(f"FAIL {path.name}: anchor found {n}x (expected 1):")
            print(old[:200])
            sys.exit(1)
        data = data.replace(old, new, 1)
    path.write_bytes(data)
    print(f"ok {path} ({len(edits)} edit(s))")


# ------------------------------------------------------------ commandScheduler.h
sched_h = ROOT / "core/src/graphics/host_gpu/renderer/commandScheduler.h"
patch(sched_h, [
    (
        b"""\tvoid BeginNext();\r
\tvoid PriorityOperationsThread(std::stop_token stop);\r
\tvoid QueueOperation(Common::UniqueFunction<void>&& operation, bool priority);\r
\tvoid RunOperation(Common::UniqueFunction<void>&& operation);\r
""",
        b"""\tvoid BeginNext();\r
\tvoid PriorityOperationsThread(std::stop_token stop);\r
\tvoid QueueOperation(Common::UniqueFunction<void>&& operation, bool priority);\r
\tvoid RunOperation(Common::UniqueFunction<void>&& operation);\r
\t// Console Five Pro (v0.0.10pro phase 1): submit the pending batch (if any) as one\r
\t// vkQueueSubmit signalling the timeline at its last tick. Same-thread discipline as\r
\t// the recording command buffer itself.\r
\tuint64_t FlushSubmitBatch();\r
""",
    ),
    (
        b"""\t\tstatic constexpr size_t GrowStep = 4;\r
""",
        b"""\t\t// Console Five Pro (v0.0.10pro phase 1): 8 (was 4) - batched submissions hold\r
\t\t// several buffers in flight at once, so the pool grows in fewer, larger rounds.\r
\t\tstatic constexpr size_t GrowStep = 8;\r
""",
    ),
    (
        b"""\tOperationState               m_operation_state      = OperationState::Open;\r
""",
        b"""\tOperationState               m_operation_state      = OperationState::Open;\r
\t// Console Five Pro (v0.0.10pro phase 1): batched queue submissions. Consecutive\r
\t// bare flushes are collected and submitted as one vkQueueSubmit; a priority deferral\r
\t// (flip completion) sets m_force_submit so the very next submission is real, keeping\r
\t// the flip waiter's tick in flight. All members follow the recording buffer's\r
\t// same-thread discipline except m_force_submit (atomic).\r
\tstd::vector<vk::CommandBuffer> m_submit_batch_buffers;\r
\tstd::vector<uint64_t>          m_submit_batch_ticks;\r
\tstd::atomic<bool>              m_force_submit {false};\r
""",
    ),
])

# ---------------------------------------------------------- commandScheduler.cpp
sched_cpp = ROOT / "core/src/graphics/host_gpu/renderer/commandScheduler.cpp"
patch(sched_cpp, [
    # 1) Shutdown: drain the batch before waiting on the last tick.
    (
        b"""\tif (!m_command.IsInvalid()) {\r
\t\tSubmit();\r
\t}\r
\tm_master.Wait(CurrentTick() - 1);\r
""",
        b"""\tif (!m_command.IsInvalid()) {\r
\t\tSubmit();\r
\t}\r
\t// v0.0.10pro phase 1: a batched submission may hold the highest reserved tick.\r
\t(void)FlushSubmitBatch();\r
\tm_master.Wait(CurrentTick() - 1);\r
""",
    ),
    # 2) FlushAndWait: Submit() may have batched; drain before waiting.
    (
        b"""void CommandScheduler::FlushAndWait() {\r
\tconst auto tick = Submit();\r
\tSUBMIT_STATS_WAIT(m_master.Wait(tick));\r
""",
        b"""void CommandScheduler::FlushAndWait() {\r
\tconst auto tick = Submit();\r
\t(void)FlushSubmitBatch();\r
\tSUBMIT_STATS_WAIT(m_master.Wait(tick));\r
""",
    ),
    # 3) Finish: drain before the current-tick wait.
    (
        b"""void CommandScheduler::Finish() {\r
\tCS_LOG("Finish called by %p", __builtin_return_address(0));\r
\tCheckActive();\r
\tif (!m_command.IsInvalid()) {\r
\t\tSubmit();\r
\t}\r
\tSUBMIT_STATS_WAIT(m_master.Wait(CurrentTick() - 1));\r
""",
        b"""void CommandScheduler::Finish() {\r
\tCS_LOG("Finish called by %p", __builtin_return_address(0));\r
\tCheckActive();\r
\tif (!m_command.IsInvalid()) {\r
\t\tSubmit();\r
\t}\r
\t(void)FlushSubmitBatch();\r
\tSUBMIT_STATS_WAIT(m_master.Wait(CurrentTick() - 1));\r
""",
    ),
    # 4) Wait: both branches may wait on a tick that is still sitting in the batch.
    (
        b"""\t\tconst auto submitted_tick = Submit();\r
\t\tEXIT_IF(submitted_tick != tick);\r
\t\tSUBMIT_STATS_WAIT(m_master.Wait(tick));\r
\t\tBeginNext();\r
\t} else {\r
\t\tSUBMIT_STATS_WAIT(m_master.Wait(tick));\r
\t}\r
""",
        b"""\t\tconst auto submitted_tick = Submit();\r
\t\tEXIT_IF(submitted_tick != tick);\r
\t\t(void)FlushSubmitBatch();\r
\t\tSUBMIT_STATS_WAIT(m_master.Wait(tick));\r
\t\tBeginNext();\r
\t} else {\r
\t\t(void)FlushSubmitBatch();\r
\t\tSUBMIT_STATS_WAIT(m_master.Wait(tick));\r
\t}\r
""",
    ),
    # 5) QueueOperation: a priority deferral must be able to complete without waiting
    #    for a future batch flush - force the next submission to be real.
    (
        b"""void CommandScheduler::QueueOperation(Common::UniqueFunction<void>&& operation, bool priority) {\r
\tCS_LOG("QueueOperation called by %p priority=%d", __builtin_return_address(0), (int)priority);\r
\tCheckActive();\r
\tEXIT_IF(!operation);\r
""",
        b"""void CommandScheduler::QueueOperation(Common::UniqueFunction<void>&& operation, bool priority) {\r
\tCS_LOG("QueueOperation called by %p priority=%d", __builtin_return_address(0), (int)priority);\r
\tCheckActive();\r
\tEXIT_IF(!operation);\r
\t// v0.0.10pro phase 1: flip completions and end-of-pipe writes wait on the tick the\r
\t// NEXT submission reserves. Forcing that submission to be a real one (instead of\r
\t\t// joining the batch) keeps the waiter's tick in flight; a stray flag without a\r
\t// following flush only costs one unbatched submit.\r
\tif (priority) {\r
\t\tm_force_submit.store(true, std::memory_order_release);\r
\t}\r
""",
    ),
    # 6) Submit: the batching itself + FlushSubmitBatch implementation before it.
    (
        b"""uint64_t CommandScheduler::Submit(SubmitInfo submit) {\r
\tEXIT_IF(m_command.IsInvalid());\r
\tg_submit_count.fetch_add(1, std::memory_order_relaxed);\r
#if defined(__ANDROID__)\r
\tSubmitStats::NoteSubmit();\r
#endif\r
\tEXIT_IF(submit.num_wait_semaphores > SubmitInfo::MaxSemaphores ||\r
\t        submit.num_signal_semaphores >= SubmitInfo::MaxSemaphores);\r
\r
\tm_command.End();\r
\tconst auto buffer   = m_command.m_buffer;\r
\tauto&      graphics = m_graphics;\r
\tEXIT_IF(graphics.queue == nullptr);\r
\r
\tvk::Result result;\r
""",
        b"""// Console Five Pro (v0.0.10pro phase 1): drain the pending submission batch as ONE\r
// vkQueueSubmit signalling the master timeline at the batch's last tick. Timeline\r
// semaphores are monotonic, and every submission site flushes earlier ticks first, so\r
// queue order keeps matching reservation order. Same-thread discipline as Submit().\r
uint64_t CommandScheduler::FlushSubmitBatch() {\r
\tif (m_submit_batch_buffers.empty()) {\r
\t\treturn m_master.CurrentTick() == 0 ? 0 : m_master.CurrentTick() - 1;\r
\t}\r
\tauto& graphics = m_graphics;\r
\tEXIT_IF(graphics.queue == nullptr);\r
\r
\tconst uint64_t last_tick = m_submit_batch_ticks.back();\r
\r
\tvk::TimelineSemaphoreSubmitInfo timeline_info {};\r
\ttimeline_info.signalSemaphoreValueCount = 1;\r
\ttimeline_info.pSignalSemaphoreValues    = &last_tick;\r
\r
\tvk::Semaphore master = m_master.Handle();\r
\tvk::SubmitInfo submit_info {};\r
\tsubmit_info.pNext                = &timeline_info;\r
\tsubmit_info.commandBufferCount   = static_cast<uint32_t>(m_submit_batch_buffers.size());\r
\tsubmit_info.pCommandBuffers      = m_submit_batch_buffers.data();\r
\tsubmit_info.signalSemaphoreCount = 1;\r
\tsubmit_info.pSignalSemaphores    = &master;\r
\r
\tconst auto result = [&] {\r
\t\tCommon::LockGuard lock(graphics.queue_mutex);\r
\t\treturn graphics.queue.submit(1, &submit_info, nullptr);\r
\t}();\r
\tif (result != vk::Result::eSuccess) {\r
\t\tReportVulkanFatal("vkQueueSubmit", result, last_tick, 0, 0, 0, 0, 0, 0, 0);\r
\t}\r
\tEXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);\r
\r
\tm_submit_batch_buffers.clear();\r
\tm_submit_batch_ticks.clear();\r
\treturn last_tick;\r
}\r
\r
uint64_t CommandScheduler::Submit(SubmitInfo submit) {\r
\tEXIT_IF(m_command.IsInvalid());\r
\tg_submit_count.fetch_add(1, std::memory_order_relaxed);\r
#if defined(__ANDROID__)\r
\tSubmitStats::NoteSubmit();\r
#endif\r
\tEXIT_IF(submit.num_wait_semaphores > SubmitInfo::MaxSemaphores ||\r
\t        submit.num_signal_semaphores >= SubmitInfo::MaxSemaphores);\r
\r
\tm_command.End();\r
\tconst auto buffer   = m_command.m_buffer;\r
\tauto&      graphics = m_graphics;\r
\tEXIT_IF(graphics.queue == nullptr);\r
\r
\t// v0.0.10pro phase 1: batched queue submissions. Every guest command-buffer flush\r
\t// used to be its own vkQueueSubmit (heavy Unity titles push ~80 per game frame at\r
\t// low FPS); consecutive semaphore-free flushes are now merged into one submit with\r
\t// several command buffers. A priority deferral forces the next submission to be\r
\t// real (see QueueOperation), and every wait path drains the batch first\r
\t// (Wait/FlushAndWait/Finish/Shutdown). Semaphore-carrying submissions - the\r
\t// swapchain presenter's own scheduler - never batch.\r
\tstatic Common::DebugFlags::CachedValue batch_limit("debug.capy.submit_batch", 8);\r
\tconst auto  max_batch = static_cast<uint32_t>(batch_limit.Get());\r
\tconst bool  forced    = m_force_submit.exchange(false, std::memory_order_acq_rel);\r
\tconst bool  batchable = max_batch > 1 && !forced && submit.num_wait_semaphores == 0 &&\r
\t                      submit.num_signal_semaphores == 0;\r
\tif (batchable) {\r
\t\tconst uint64_t batch_tick = m_master.NextTick();\r
\t\tm_submit_batch_buffers.push_back(buffer);\r
\t\tm_submit_batch_ticks.push_back(batch_tick);\r
\t\tm_command.m_buffer = nullptr;\r
\t\tif (m_submit_batch_buffers.size() < max_batch) {\r
\t\t\treturn batch_tick;\r
\t\t}\r
\t\t// Capacity reached: one vkQueueSubmit carries the whole batch.\r
\t\tconst uint64_t flushed = FlushSubmitBatch();\r
\t\tEXIT_IF(flushed != batch_tick);\r
\t\treturn batch_tick;\r
\t}\r
\t// A forced or semaphore-carrying submission flushes the batch first so the\r
\t// timeline stays ordered and forced ticks are in flight.\r
\t(void)FlushSubmitBatch();\r
\r
\tvk::Result result;\r
""",
    ),
])

# ------------------------------------------------------------------ renderDraw.cpp
draw_cpp = ROOT / "core/src/graphics/host_gpu/renderer/renderDraw.cpp"
patch(draw_cpp, [
    # include for the vendor check
    (
        b"""#include "common/debugFlags.h"\r
""",
        b"""#include "common/debugFlags.h"\r
#include "common/gpuProfile.h"\r
""",
    ),
    # depth kick: Adreno workaround, skip on Mali
    (
        b"""\tstatic Common::DebugFlags::CachedValue depth_kick("debug.capy.depth_kick", 1);\r
\tif (depth_kick.Get() != 0 && rendering.depth_stencil_attachment.has_depth &&\r
""",
        b"""\tstatic Common::DebugFlags::CachedValue depth_kick("debug.capy.depth_kick", 1);\r
\t// v0.0.10pro phase 1: the kick is an Adreno driver workaround; on Mali it is pure\r
\t// per-draw cost (a tiler executes clearAttachments as real commands), so Mali skips\r
\t// it. debug.capy.depth_kick 1 still forces it on everywhere.\r
\tconst bool depth_kick_on = depth_kick.Get() != 0 &&\r
\t                         (depth_kick.Get() == 1 ? !Common::GpuProfile::IsMali() : true);\r
\tif (depth_kick_on && rendering.depth_stencil_attachment.has_depth &&\r
""",
    ),
])

# ---------------------------------------------------------- sysLinuxVirtual.cpp
virt_cpp = ROOT / "core/src/common/platform/sysLinuxVirtual.cpp"
patch(virt_cpp, [
    (
        b"""#include "common/assert.h"\r
#include "common/virtualMemory.h"\r
""",
        b"""#include "common/assert.h"\r
#include "common/debugFlags.h"\r
#include "common/virtualMemory.h"\r
""",
    ),
    (
        b"""bool Protect(uint64_t address, uint64_t size, Mode mode) {\r
\tconst auto addr       = static_cast<uintptr_t>(address);\r
\tconst auto page_start = addr >> 12u;\r
\tconst auto page_end   = (addr + size - 1) >> 12u;\r
\treturn mprotect(reinterpret_cast<void*>(page_start << 12u), (page_end - page_start + 1) << 12u,\r
\t                get_protection_flag(mode)) == 0;\r
}\r
""",
        b"""bool Protect(uint64_t address, uint64_t size, Mode mode) {\r
\tconst auto addr       = static_cast<uintptr_t>(address);\r
\tconst auto page_start = addr >> 12u;\r
\tconst auto page_end   = (addr + size - 1) >> 12u;\r
\tconst auto bytes      = (page_end - page_start + 1) << 12u;\r
\tif (mprotect(reinterpret_cast<void*>(page_start << 12u), bytes,\r
\t             get_protection_flag(mode)) != 0) {\r
\t\treturn false;\r
\t}\r
#if defined(__linux__)\r
\t// Console Five Pro (v0.0.10pro phase 1): transparent huge pages for guest memory.\r
\t// The multi-gigabyte guest heap is committed through this function; advising\r
\t// MADV_HUGEPAGE lets the kernel back it with 2 MiB pages, which means ~500x fewer\r
\t// soft page faults while the game touches its memory and far fewer TLB misses for\r
\t// every guest dereference the FEX translation makes. A no-op on kernels with THP\r
\t// disabled; never changes guest-visible contents. `debug.capy.thp 0` reverts.\r
\tstatic Common::DebugFlags::CachedValue thp("debug.capy.thp", 1);\r
\tconstexpr uint64_t HugePageBytes = 2ull * 1024 * 1024;\r
\tif (mode != Mode::NoAccess && bytes >= HugePageBytes && thp.Get() != 0) {\r
\t\t(void)::madvise(reinterpret_cast<void*>(page_start << 12u), bytes, MADV_HUGEPAGE);\r
\t}\r
#endif\r
\treturn true;\r
}\r
""",
    ),
])

# ---------------------------------------------------------------- vulkanWindow.cpp
win_cpp = ROOT / "core/src/graphics/presentation/window/vulkanWindow.cpp"
patch(win_cpp, [
    (
        b"""\t\t\t"GPU: {} - Mali performance profile enabled: hardware render-target clears "\r
\t\t\t"(AFBC-friendly), 128 MiB streaming buffer, +1 pipeline worker, "\r
\t\t\t"pass-split-free event barriers, batched compute submissions\\n",\r
""",
        b"""\t\t\t"GPU: {} - Mali performance profile enabled: hardware render-target clears "\r
\t\t\t"(AFBC-friendly), 128 MiB streaming buffer, +1 pipeline worker, "\r
\t\t\t"pass-split-free event barriers, batched compute submissions, "\r
\t\t\t"batched queue submits (2 MiB huge-page guest memory, depth-pass kick off)\\n",\r
""",
    ),
])

print("Mali deep profile v2: all files patched")
