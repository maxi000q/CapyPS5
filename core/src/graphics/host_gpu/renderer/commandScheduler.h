// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_

#include "common/common.h"
#include "common/uniqueFunction.h"
#include "graphics/host_gpu/renderer/dynamicState.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"
#include "graphics/host_gpu/renderer/render.h"

#include <condition_variable>
#include <mutex>
#include <queue>
#include <stop_token>
#include <thread>
#include <vector>

#include <atomic>

namespace Libs::Graphics {

// Queue submissions so far (gpu_stats diagnostics).
extern std::atomic<uint64_t> g_submit_count;

class CommandScheduler {
public:
	// v0.0.10pro phase 1: batch_submits enables queue-submit batching for this
	// scheduler - only RenderContext's own scheduler opts in (one bare flush per guest
	// command buffer); the presenter's scheduler never batches (its blank-frame ticks
	// must be in flight immediately).
	explicit CommandScheduler(RenderContext& context, GraphicContext& graphics,
	                        bool batch_submits = false);
	~CommandScheduler();
	KYTY_CLASS_NO_COPY(CommandScheduler);

	void           Begin(HW::Context& registers, HW::UserConfig& user_config, HW::Shader& shaders);
	void           BeginRendering(const RenderState& state);
	void           EndRendering();
	void           Flush();
	void           Flush(SubmitInfo& submit);
	void           FlushAndWait();
	void           Finish();
	// v0.0.9proQ4 round 2: drain a stale submit batch when no more guest work is
	// queued (owner thread only, same discipline as FlushSubmitBatch). Public: the
	// GPU thread calls it at queue-empty so the frame tail reaches the GPU at once.
	void DrainSubmitBatch();
	CommandBuffer& BeginCommand();
	uint64_t       Submit(SubmitInfo submit = {});
	// Deferred callbacks can observe an externally owned drain, but cannot initiate shutdown:
	// the priority runner cannot join itself.
	void                      Shutdown();
	void                      Wait(uint64_t tick);
	void                      PopPendingOperations();
	void                      DrainPriorityOperations();
	void                      WaitPriorityOperations(uint64_t tick);
	// Guest-memory completions use the priority queue; normal callbacks maintain GPU resources.
	void                      DeferOperation(Common::UniqueFunction<void>&& operation);
	void                      DeferPriorityOperation(Common::UniqueFunction<void>&& operation);
	[[nodiscard]] bool        HasPendingPriorityOperations();
	[[nodiscard]] static bool InDeferredOperation() noexcept;

	[[nodiscard]] bool Active() const noexcept { return m_command.m_registers != nullptr; }
	void                           CheckActive() const;
	CommandBuffer&                 Current();
	[[nodiscard]] uint64_t         CurrentTick() const noexcept { return m_master.CurrentTick(); }
	[[nodiscard]] bool             IsFree(uint64_t tick);
	[[nodiscard]] MasterSemaphore& GetMasterSemaphore() noexcept { return m_master; }
	[[nodiscard]] DynamicState&    GetDynamicState() noexcept { return m_dynamic_state; }
	[[nodiscard]] RenderContext&   Context() const noexcept { return m_context; }
	[[nodiscard]] GraphicContext&  Graphics() const noexcept { return m_graphics; }

private:
	class CommandPool {
	public:
		CommandPool(GraphicContext& graphics, MasterSemaphore& master);
		~CommandPool();
		KYTY_CLASS_NO_COPY(CommandPool);

		vk::CommandBuffer Commit();

	private:
		// Console Five Pro (v0.0.10pro phase 1): 8 (was 4) - batched submissions hold
		// several buffers in flight at once, so the pool grows in fewer, larger rounds.
		static constexpr size_t GrowStep = 8;

		size_t Grow();

		GraphicContext&                m_graphics;
		MasterSemaphore&               m_master;
		vk::CommandPool                m_pool = nullptr;
		std::vector<vk::CommandBuffer> m_buffers;
		std::vector<uint64_t>          m_ticks;
		size_t                         m_hint = 0;
	};

	enum class OperationState { Open, Draining, Closed };

	struct PendingOperation {
		Common::UniqueFunction<void> callback;
		uint64_t                     tick = 0;
	};

	void BeginNext();
	void PriorityOperationsThread(std::stop_token stop);
	void QueueOperation(Common::UniqueFunction<void>&& operation, bool priority);
	void RunOperation(Common::UniqueFunction<void>&& operation);
	// Console Five Pro (v0.0.10pro phase 1): submit the pending batch (if any) as one
	// vkQueueSubmit signalling the timeline at its last tick. Same-thread discipline as
	// the recording command buffer itself.
	uint64_t FlushSubmitBatch();

	MasterSemaphore              m_master;
	RenderContext&               m_context;
	GraphicContext&              m_graphics;
	CommandPool                  m_command_pool;
	CommandBuffer                m_command;
	DynamicState                 m_dynamic_state;
	std::queue<PendingOperation> m_pending_operations;
	std::queue<PendingOperation> m_priority_operations;
	std::mutex                   m_operation_mutex;
	std::condition_variable      m_operation_available;
	std::jthread                 m_priority_thread;
	bool                         m_priority_active      = false;
	uint64_t                     m_priority_active_tick = 0;
	OperationState               m_operation_state      = OperationState::Open;
	// Console Five Pro (v0.0.10pro phase 1): batched queue submissions. Consecutive
	// bare flushes are collected and submitted as one vkQueueSubmit; a priority deferral
	// (flip completion) sets m_force_submit so the very next submission is real, keeping
	// the flip waiter's tick in flight. All members follow the recording buffer's
	// same-thread discipline except m_force_submit (atomic).
	bool                         m_batch_submits       = false;
	std::vector<vk::CommandBuffer> m_submit_batch_buffers;
	std::vector<uint64_t>          m_submit_batch_ticks;
	std::atomic<bool>              m_force_submit {false};
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_
