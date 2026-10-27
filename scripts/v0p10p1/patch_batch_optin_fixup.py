#!/usr/bin/env python3
"""v0.0.10pro phase 1 fixup: submit batching must be opt-in per scheduler.

The presenter's scheduler (swapchain.cpp) also calls bare Submit() for blank
frames (PrepareBlankFrame). Batching there would delay blank-frame ticks until
some other path drains the batch - pointless risk. Make batching a constructor
opt-in used only by RenderContext's own scheduler (the one that receives one
bare flush per guest command buffer).
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


sched_h = ROOT / "core/src/graphics/host_gpu/renderer/commandScheduler.h"
patch(sched_h, [
    (
        b"""\tCommandScheduler(RenderContext& context, GraphicContext& graphics);\r
""",
        b"""\t// v0.0.10pro phase 1: batch_submits enables queue-submit batching for this\r
\t// scheduler - only RenderContext's own scheduler opts in (one bare flush per guest\r
\t// command buffer); the presenter's scheduler never batches (its blank-frame ticks\r
\t// must be in flight immediately).\r
\texplicit CommandScheduler(RenderContext& context, GraphicContext& graphics,\r
\t                        bool batch_submits = false);\r
""",
    ),
    (
        b"""\tstd::vector<vk::CommandBuffer> m_submit_batch_buffers;\r
""",
        b"""\tbool                         m_batch_submits       = false;\r
\tstd::vector<vk::CommandBuffer> m_submit_batch_buffers;\r
""",
    ),
])

sched_cpp = ROOT / "core/src/graphics/host_gpu/renderer/commandScheduler.cpp"
patch(sched_cpp, [
    (
        b"""CommandScheduler::CommandScheduler(RenderContext& context, GraphicContext& graphics)\r
    : m_master(graphics), m_context(context), m_graphics(graphics),\r
""",
        b"""CommandScheduler::CommandScheduler(RenderContext& context, GraphicContext& graphics,\r
                                   bool batch_submits)\r
    : m_master(graphics), m_context(context), m_graphics(graphics),\r
""",
    ),
    (
        b"""\tconst bool  batchable = max_batch > 1 && !forced && submit.num_wait_semaphores == 0 &&\r
""",
        b"""\tconst bool  batchable = m_batch_submits && max_batch > 1 && !forced &&\r
\t                      submit.num_wait_semaphores == 0 &&\r
""",
    ),
])

# hook: renderContext initializes its scheduler in the member-init list
ctx_cpp = ROOT / "core/src/graphics/host_gpu/renderer/renderContext.cpp"
patch(ctx_cpp, [
    (
        b"""    : m_graphics(graphics), m_render_executor(*this), m_command_scheduler(*this, graphics),\r
""",
        b"""    : m_graphics(graphics), m_render_executor(*this),\r
      m_command_scheduler(*this, graphics, true /* v0.0.10pro: batched submits */),\r
""",
    ),
])

print("opt-in batching fixup applied")
