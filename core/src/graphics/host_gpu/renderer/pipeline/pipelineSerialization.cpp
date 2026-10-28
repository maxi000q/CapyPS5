// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

#include "common/file.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/uniqueFunction.h"
#include "kytyGitVersion.h"
#include "graphics/shader/recompiler/ir/Serialize.h"
#include "common/stringUtils.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/host_gpu/graphicContext.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fmt/format.h>
#include <ranges>
#include <xxhash.h>
#include <zstd.h>

// The shader store: a header, then zstd-compressed records of everything a run created first.
// Records are only appended, so a store cut short by a crash keeps every complete record.
namespace Libs::Graphics {

namespace {

// Bump when a record's encoding changes on purpose.
constexpr uint32_t ShaderStoreFormat = 1;

} // namespace

// What every record depends on besides its own fields.
std::vector<uint8_t> ShaderStoreHeader(const GraphicContext& graphics) {
	const auto& limits = graphics.GetPhysicalDeviceProperties().limits;
	ShaderRecompiler::IR::Writer header;
	const uint64_t revision = XXH3_64bits(KYTY_GIT_REVISION,
	                                   std::strlen(KYTY_GIT_REVISION));
	ShaderRecompiler::IR::IoAll(header, ShaderStoreFormat, revision,
	                             limits.maxComputeSharedMemorySize);
	return std::move(header.bytes);
}

void ShaderStoreWriter::Open(const std::filesystem::path& path, std::span<const uint8_t> header,
                             uint64_t kept) {
	Common::File::CreateDirectories(path.parent_path());
	if (kept == 0) {
		if (m_file.Create(path)) {
			m_file.Write(header.data(), static_cast<uint32_t>(header.size()));
		}
	} else if (m_file.Open(path, Common::File::Mode::ReadWrite)) {
		m_file.Truncate(kept);
		m_file.Seek(kept);
	}
	if (m_file.IsInvalid()) {
		return;
	}
	m_thread = std::jthread([this](std::stop_token stop) {
		std::unique_lock lock(m_mutex);
		// Once stop is requested the wait returns false only when the queue is empty.
		while (m_ready.wait(lock, stop, [this] { return !m_jobs.empty(); })) {
			const auto job = std::move(m_jobs.front());
			m_jobs.pop_front();
			lock.unlock();
			job();
			lock.lock();
		}
	});
}

void ShaderStoreWriter::Append(std::vector<uint8_t>&& record) {
	Post([this, record = std::move(record)] {
		std::vector<uint8_t> bytes(sizeof(ShaderStoreFrame) + ZSTD_compressBound(record.size()));
		const auto size = ZSTD_compress(bytes.data() + sizeof(ShaderStoreFrame), bytes.size() - sizeof(ShaderStoreFrame),
		                                record.data(), record.size(), 3);
		EXIT_IF(ZSTD_isError(size));
		const ShaderStoreFrame frame {.compressed_size = static_cast<uint32_t>(size),
		                   .raw_size        = static_cast<uint32_t>(record.size()),
		                   .checksum        = XXH3_64bits(bytes.data() + sizeof(ShaderStoreFrame), size)};
		std::memcpy(bytes.data(), &frame, sizeof(frame));
		// A failed write leaves a partial record; the next run drops it and everything after it.
		m_file.Write(bytes.data(), static_cast<uint32_t>(sizeof(ShaderStoreFrame) + size));
	});
}

void ShaderStoreWriter::Post(Common::UniqueFunction<void>&& job) {
	{
		std::scoped_lock lock(m_mutex);
		m_jobs.push_back(std::move(job));
	}
	m_ready.notify_one();
}

ShaderStoreWriter::~ShaderStoreWriter() {
	if (m_thread.joinable()) {
		m_thread.request_stop();
		m_ready.notify_all();
		m_thread.join();
	}
}

} // namespace Libs::Graphics
