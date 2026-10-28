// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_SERIALIZE_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_SERIALIZE_H_

#include "common/assert.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include <algorithm>
#include <bit>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

// Shader cache records. Io(io, value) appends value to a Writer or reads it back from a Reader
// field by field, so padding never reaches the bytes; instructions travel as plan-local indices.
namespace Libs::Graphics::ShaderRecompiler::IR {

struct Writer {
	std::vector<uint8_t>                      bytes;
	std::unordered_map<const Inst*, uint32_t> index; // Instructions that values may refer to.

	void Raw(const void* data, size_t size) {
		const auto* begin = static_cast<const uint8_t*>(data);
		bytes.insert(bytes.end(), begin, begin + size);
	}
};

struct Reader {
	std::span<const uint8_t> bytes;
	std::vector<Inst*>       insts;     // Instructions by index.
	bool                     ok = true; // False once a read ran past the end or was invalid.

	void Raw(void* data, size_t size) {
		ok = ok && size <= bytes.size();
		if (!ok) return;
		std::copy_n(bytes.data(), size, static_cast<uint8_t*>(data));
		bytes = bytes.subspan(size);
	}
	// Every element takes at least one byte, which bounds counts read from damaged records.
	uint32_t Count(uint32_t count) {
		ok = ok && count <= bytes.size();
		return ok ? count : 0u;
	}
};

template <class S> constexpr bool Reading = std::is_same_v<S, Reader>;
template <class T, template <class...> class Of> constexpr bool IsA = false;
template <template <class...> class Of, class... A> constexpr bool IsA<Of<A...>, Of> = true;
template <class T, class U> concept Is = std::is_same_v<std::remove_const_t<T>, U>;

template <class S, class T> void Io(S& io, T& value);

template <class S, class... T> void IoAll(S& io, T&&... values) {
	(Io(io, values), ...);
}

// The value's Type, Opaque for an instruction, then the instruction index or the zero-extended
// immediate.
inline void IoValue(Writer& writer, const Value& value) {
	auto     type = Type::Opaque;
	uint64_t bits = 0;
	if (const auto* inst = value.TryInstruction()) {
		bits = writer.index.at(inst);
	} else {
		type = value.GetType();
		switch (type) {
			case Type::Void: break;
			case Type::ScalarReg: bits = RegIndex(value.ScalarRegister()); break;
			case Type::VectorReg: bits = RegIndex(value.VectorRegister()); break;
			case Type::U1: bits = value.U1(); break;
			case Type::U8: bits = value.U8(); break;
			case Type::U16: bits = value.U16(); break;
			case Type::U32: bits = value.U32(); break;
			case Type::U64: bits = value.U64(); break;
			case Type::F16: bits = value.F16Bits(); break;
			case Type::F32: bits = std::bit_cast<uint32_t>(value.F32Value()); break;
			default: EXIT("value type %u has no cache encoding\n", static_cast<uint32_t>(type));
		}
	}
	IoAll(writer, type, bits);
}

inline void IoValue(Reader& reader, Value& value) {
	auto     type = Type::Void;
	uint64_t bits = 0;
	IoAll(reader, type, bits);
	switch (type) {
		case Type::Void: value = {}; break;
		case Type::Opaque:
			reader.ok = reader.ok && bits < reader.insts.size();
			if (reader.ok) value = Value(reader.insts[bits]);
			break;
		case Type::ScalarReg: value = Value(static_cast<ScalarReg>(bits)); break;
		case Type::VectorReg: value = Value(static_cast<VectorReg>(bits)); break;
		case Type::U1: value = Value(bits != 0u); break;
		case Type::U8: value = Value(static_cast<uint8_t>(bits)); break;
		case Type::U16: value = Value(static_cast<uint16_t>(bits)); break;
		case Type::U32: value = Value(static_cast<uint32_t>(bits)); break;
		case Type::U64: value = Value(bits); break;
		case Type::F16: value = Value::F16(static_cast<uint16_t>(bits)); break;
		case Type::F32: value = Value::F32(std::bit_cast<float>(static_cast<uint32_t>(bits))); break;
		default: reader.ok = false; break;
	}
}

// Scalars and standard containers; structs have their own overloads below.
template <class S, class T> void Io(S& io, T& value) {
	using U = std::remove_const_t<T>;
	static_assert(!std::is_pointer_v<U>, "pointers do not persist");
	if constexpr (std::is_arithmetic_v<U> || std::is_enum_v<U>) {
		io.Raw(&value, sizeof(U));
	} else if constexpr (std::is_same_v<U, Value>) {
		IoValue(io, value);
	} else if constexpr (IsA<U, std::vector> || IsA<U, std::basic_string>) {
		auto count = static_cast<uint32_t>(value.size());
		Io(io, count);
		if constexpr (Reading<S>) value.resize(io.Count(count));
		using E = typename U::value_type;
		if constexpr (std::is_arithmetic_v<E> || std::is_enum_v<E>) {
			io.Raw(value.data(), value.size() * sizeof(E));
		} else {
			for (auto& element: value) Io(io, element);
		}
	} else if constexpr (IsA<U, std::optional>) {
		bool has = value.has_value();
		Io(io, has);
		if constexpr (Reading<S>) {
			if (has) value = typename U::value_type {}; // Not emplace(): clang rejects nested aggregates.
			else value.reset();
		}
		if (has) Io(io, *value);
	} else if constexpr (IsA<U, std::pair>) {
		IoAll(io, value.first, value.second);
	} else {
		static_assert(requires { std::tuple_size<U>::value; }, "struct without an Io overload");
		std::apply([&](auto&... elements) { IoAll(io, elements...); }, value); // std::array
	}
}

// Binds every field, so that a field added later fails to compile until it is listed.
#define KYTY_IO_FIELDS(Struct, ...)                                                                \
	template <class S, Is<Struct> T> void Io(S& io, T& object) {                                   \
		auto& [__VA_ARGS__] = object;                                                              \
		IoAll(io, __VA_ARGS__);                                                                    \
	}
KYTY_IO_FIELDS(MemoryInfo, kind, resource, sampler, offset, secondary_offset, dmask, data_dwords,
               data_bits, component_index, component_count, data_format, number_format,
               image_sample_flags, image_dimension, image_address_components, address_is_full,
               data_signed, typed, formatted, image_has_mip, image_r128, idxen, offen, coherent,
               planning_only)
KYTY_IO_FIELDS(BufferResource, source, descriptor_index, first_use_pc, max_byte_extent,
               write_extent, packed_stride, descriptor_format, descriptor_swizzle, image_alias,
               read, written, atomic, formatted, scalar, live, indirect_root,
               indirect_mapping_offset, indirect_search_iterations, indirect_resources)
KYTY_IO_FIELDS(ImageResource, source, descriptor_index, first_use_pc, resource_class,
               numeric_class, dimension, mip_mode, mip_count, conversion_format, shader_swizzle,
               read, written, atomic, atomic64, unfiltered_access_only, depth_compare, cube, r128,
               heap, indirect_root, indirect_mapping_offset, indirect_search_iterations,
               indirect_resources)
KYTY_IO_FIELDS(SamplerResource, source, first_use_pc, snapshot_index, indirect_root,
               indirect_mapping_offset, indirect_search_iterations, indirect_resources,
               force_point_filtering, depth_compare, integer_border, gather_lod)
KYTY_IO_FIELDS(SampledResourcePair, image, sampler, first_use_pc)
KYTY_IO_FIELDS(StageInput, kind, location, component_count, debug_name, per_vertex)
KYTY_IO_FIELDS(StageOutput, kind, index, location, debug_name)
KYTY_IO_FIELDS(ShaderInfo, buffers, images, samplers, sampled_pairs, inputs, outputs,
               user_data_registers, vertex_fetch_components, vertex_offset_sgpr,
               instance_offset_sgpr, has_bitwise_xor, uses_dma, uses_swizzle, uses_lds, uses_gds,
               uses_flattened_srt, uses_image_heap, uses_dispatch_threads, bvh, subgroup_ballot,
               subgroup_vote, subgroup_barrier, subgroup_shuffle, subgroup_arithmetic,
               subgroup_local_invocation_id, compute_derivatives, image_gather_extended,
               function_lds, function_lds_dwords, function_scratch, pixel_valid_mask,
               buffer_int64_atomics, buffer_u8, buffer_u16, shared_int64_atomics,
               coherent_buffers, float64)
KYTY_IO_FIELDS(DescriptorSource::IndirectDescriptor::SelectorRead::CountedRows, source, stride)
KYTY_IO_FIELDS(DescriptorSource::IndirectDescriptor::SelectorRead, source, stride, offset,
               maximum, bit_offset, bit_mask, global, bitset, rows, byte_ranges)
KYTY_IO_FIELDS(DescriptorSource::IndirectDescriptor::PointerTable, offset, stride, count,
               indexed)
KYTY_IO_FIELDS(DescriptorSource::IndirectDescriptor, selector, table_maximum, pointer_table,
               table_source, table_offset, table_immediate, table_stride, table_record_bytes,
               table_scalar, indexed, signed_count, workgroup_axis, key_count, selector_first,
               selector_mask, sources)
KYTY_IO_FIELDS(DescriptorSource, dwords, dword_count, indirect_descriptor)
KYTY_IO_FIELDS(SrtRead, value, flat_offset)
KYTY_IO_FIELDS(ResourceBlock, condition, successors, sources, srt_reads)
KYTY_IO_FIELDS(UniformFill, kind, resource, group_stride, words, value, bounded_elements)
KYTY_IO_FIELDS(UniformFillPlan, fill, values, pattern_size, element_count)
KYTY_IO_FIELDS(ResourceSpecialization::Buffer, packed_stride, descriptor_format,
               descriptor_swizzle, zero_stride_oob, scalar_empty, indirect_root,
               indirect_mapping_offset, indirect_search_iterations)
KYTY_IO_FIELDS(ResourceSpecialization::Image, numeric_class, dimension, mip_count,
               conversion_format, shader_swizzle, indirect_root, indirect_mapping_offset,
               indirect_search_iterations, cube, fmask, heap)
KYTY_IO_FIELDS(ResourceSpecialization::Sampler, indirect_root, indirect_mapping_offset,
               indirect_search_iterations)
KYTY_IO_FIELDS(ResourceSpecialization, buffers, images, samplers)
KYTY_IO_FIELDS(Bindings, descriptor_counts, descriptor_mask, push_data_start_dword,
               dispatch_thread_dword, group_base_dword, memory_offset_dword)
KYTY_IO_FIELDS(CompiledShaderInfo, stage, shader_hash, wave_size, user_data_base,
               user_data_count, scratch_dwords, param_export_mask, has_address_writes,
               mesh_compute, info, bindings)
#undef KYTY_IO_FIELDS

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif // EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_SERIALIZE_H_
