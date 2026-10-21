/**************************************************************************/
/*  test_webgpu_compute.cpp                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_webgpu_compute)

#ifdef WEBGPU_ENABLED

#include "core/io/file_access.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "drivers/webgpu/rendering_context_driver_webgpu.h"
#include "drivers/webgpu/rendering_device_driver_webgpu.h"
#include "drivers/webgpu/rendering_shader_container_webgpu.h"
#include "servers/rendering/rendering_device.h"

namespace TestWebGPUCompute {

using RDC = RenderingDeviceCommons;

static void collect_independent_readback(const PackedByteArray &p_data, Dictionary p_state, int p_index) {
	Array results = p_state["results"];
	results[p_index] = p_data;
	p_state["count"] = int(p_state["count"]) + 1;
}

// Brings up a standalone (windowless) RenderingDevice on the WebGPU driver.
struct WebGPUFixture {
	RenderingContextDriverWebGPU context;
	RenderingDevice *rd = nullptr;

	bool init() {
		if (context.initialize() != OK) {
			return false;
		}
		rd = memnew(RenderingDevice);
		if (rd->initialize(&context) != OK) {
			memdelete(rd);
			rd = nullptr;
			return false;
		}
		return true;
	}

	~WebGPUFixture() {
		if (rd) {
			memdelete(rd);
		}
	}

	RID create_shader(const String &p_source, const String &p_name) {
		String error;
		Vector<uint8_t> spirv = rd->shader_compile_spirv_from_source(RDC::SHADER_STAGE_COMPUTE, p_source, RDC::SHADER_LANGUAGE_GLSL, &error, false);
		INFO(error.utf8().get_data());
		REQUIRE_MESSAGE(!spirv.is_empty(), "GLSL compilation failed.");
		Vector<RDC::ShaderStageSPIRVData> stages;
		RDC::ShaderStageSPIRVData stage;
		stage.shader_stage = RDC::SHADER_STAGE_COMPUTE;
		stage.spirv = spirv;
		stages.push_back(stage);
		return rd->shader_create_from_spirv(stages, p_name);
	}

	void dispatch(RID p_shader, RID p_uniform_set, uint32_t p_x, uint32_t p_y, uint32_t p_z) {
		RID pipeline = rd->compute_pipeline_create(p_shader);
		REQUIRE(pipeline.is_valid());
		RenderingDevice::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, pipeline);
		rd->compute_list_bind_uniform_set(list, p_uniform_set, 0);
		rd->compute_list_dispatch(list, p_x, p_y, p_z);
		rd->compute_list_end();
		rd->submit();
		rd->sync();
	}
};

TEST_CASE("[WebGPU][Compute] Combined sampler preserves anonymous storage bindings") {
	if (OS::get_singleton()->get_environment("GODOT_TINT_PATH").is_empty()) {
		WARN("GODOT_TINT_PATH is required for the WGSL binding regression.");
		return;
	}
	WebGPUFixture fx;
	REQUIRE(fx.init());
	String error;
	Vector<RDC::ShaderStageSPIRVData> stages;
	RDC::ShaderStageSPIRVData stage;
	stage.shader_stage = RDC::SHADER_STAGE_COMPUTE;
	stage.spirv = fx.rd->shader_compile_spirv_from_source(RDC::SHADER_STAGE_COMPUTE, R"(
#version 450
layout(local_size_x=1) in;
layout(set=0,binding=0,std140) uniform Params { uint count; } params;
layout(set=0,binding=1) uniform sampler3D field;
layout(set=0,binding=2,std430) readonly buffer Candidates { uvec4 cells[]; };
layout(set=0,binding=3,std430) writeonly buffer Results { uint needed[]; };
void main() {
    uint id=gl_GlobalInvocationID.x;
    if (id<params.count) needed[id]=cells[id].x+uint(texelFetch(field,ivec3(0),0).r);
}
)",
			RDC::SHADER_LANGUAGE_GLSL, &error, false);
	REQUIRE_MESSAGE(!stage.spirv.is_empty(), error.utf8().get_data());
	stages.push_back(stage);
	RenderingShaderContainerFormatWebGPU format;
	Ref<RenderingShaderContainer> container = format.create_container();
	REQUIRE(container->set_code_from_spirv("anonymous_classification_buffers", stages));
	const RDC::ShaderReflection reflection = container->get_shader_reflection();
	REQUIRE(reflection.uniform_sets.size() == 1);
	REQUIRE(reflection.uniform_sets[0].size() == 4);
	CHECK_FALSE(reflection.uniform_sets[0][2].writable);
	CHECK(reflection.uniform_sets[0][3].writable);
	const PackedByteArray &bytes = container->shaders[0].code_compressed_bytes;
	const String wgsl = String::utf8((const char *)bytes.ptr(), bytes.size());
	INFO(wgsl);
	CHECK(wgsl.contains("@binding(2u) var<storage, read>"));
	CHECK(wgsl.contains("@binding(3u) var<storage, read_write>"));
	CHECK(wgsl.contains("@binding(501u) var field_sampler : sampler;"));

	RenderingDevice *rd = fx.rd;
	RID shader = rd->shader_create_from_spirv(stages, "anonymous_classification_dispatch");
	REQUIRE(shader.is_valid());
	const uint32_t count = 4;
	Vector<uint8_t> parameters;
	parameters.resize(16);
	memset(parameters.ptrw(), 0, 16);
	memcpy(parameters.ptrw(), &count, sizeof(count));
	RID params = rd->uniform_buffer_create(16, parameters);
	Vector<uint8_t> inputs;
	inputs.resize(count * 16);
	memset(inputs.ptrw(), 0, inputs.size());
	for (uint32_t index = 0; index < count; index++) {
		const uint32_t value = index * 7;
		memcpy(inputs.ptrw() + index * 16, &value, sizeof(value));
	}
	RID candidates = rd->storage_buffer_create(inputs.size(), inputs);
	RID results = rd->storage_buffer_create(count * 4);
	RenderingDevice::TextureFormat texture_format;
	texture_format.texture_type = RDC::TEXTURE_TYPE_3D;
	texture_format.format = RDC::DATA_FORMAT_R8G8B8A8_UNORM;
	texture_format.width = texture_format.height = texture_format.depth = 1;
	texture_format.usage_bits = RDC::TEXTURE_USAGE_SAMPLING_BIT;
	Vector<Vector<uint8_t>> layers;
	Vector<uint8_t> pixel;
	pixel.resize(4);
	memset(pixel.ptrw(), 255, 4);
	layers.push_back(pixel);
	RID texture = rd->texture_create(texture_format, RenderingDevice::TextureView(), layers);
	RID sampler = rd->sampler_create(RenderingDevice::SamplerState());
	Vector<RenderingDevice::Uniform> uniforms;
	for (uint32_t binding = 0; binding < 4; binding++) {
		RenderingDevice::Uniform uniform;
		uniform.binding = binding;
		if (binding == 0) {
			uniform.uniform_type = RDC::UNIFORM_TYPE_UNIFORM_BUFFER;
			uniform.append_id(params);
		} else if (binding == 1) {
			uniform.uniform_type = RDC::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
			uniform.append_id(sampler);
			uniform.append_id(texture);
		} else {
			uniform.uniform_type = RDC::UNIFORM_TYPE_STORAGE_BUFFER;
			uniform.append_id(binding == 2 ? candidates : results);
		}
		uniforms.push_back(uniform);
	}
	RID uniform_set = rd->uniform_set_create(uniforms, shader, 0);
	REQUIRE(uniform_set.is_valid());
	fx.dispatch(shader, uniform_set, count, 1, 1);
	Vector<uint8_t> output = rd->buffer_get_data(results);
	REQUIRE(output.size() == int(count * 4));
	for (uint32_t index = 0; index < count; index++) {
		uint32_t value;
		memcpy(&value, output.ptr() + index * 4, sizeof(value));
		CHECK(value == index * 7 + 1);
	}
	rd->free_rid(uniform_set);
	rd->free_rid(sampler);
	rd->free_rid(texture);
	rd->free_rid(results);
	rd->free_rid(candidates);
	rd->free_rid(params);
	rd->free_rid(shader);
}

TEST_CASE("[WebGPU][Compute] Storage and uniform buffers") {
	WebGPUFixture fx;
	if (!fx.init()) {
		WARN("No WebGPU adapter available, skipping.");
		return;
	}
	RenderingDevice *rd = fx.rd;

	const char *source = R"(
#version 450
layout(local_size_x = 64) in;
layout(set = 0, binding = 0, std140) uniform Params { uint count; float scale; } params;
layout(set = 0, binding = 1, std430) buffer Data { float values[]; };
void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i < params.count) {
		values[i] = values[i] * params.scale + float(i);
	}
}
)";
	RID shader = fx.create_shader(source, "buffers");
	REQUIRE(shader.is_valid());

	const uint32_t count = 1000;
	Vector<float> input;
	input.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		input.write[i] = float(i) * 0.5f;
	}
	RID data_buffer = rd->storage_buffer_create(count * sizeof(float), Span<uint8_t>((const uint8_t *)input.ptr(), count * sizeof(float)));

	struct Params {
		uint32_t count;
		float scale;
		float pad[2];
	} params = { count, 3.0f, { 0, 0 } };
	RID params_buffer = rd->uniform_buffer_create(sizeof(Params), Span<uint8_t>((const uint8_t *)&params, sizeof(Params)));

	Vector<RenderingDevice::Uniform> uniforms;
	RenderingDevice::Uniform params_uniform;
	params_uniform.uniform_type = RDC::UNIFORM_TYPE_UNIFORM_BUFFER;
	params_uniform.binding = 0;
	params_uniform.append_id(params_buffer);
	uniforms.push_back(params_uniform);
	RenderingDevice::Uniform data_uniform;
	data_uniform.uniform_type = RDC::UNIFORM_TYPE_STORAGE_BUFFER;
	data_uniform.binding = 1;
	data_uniform.append_id(data_buffer);
	uniforms.push_back(data_uniform);
	RID uniform_set = rd->uniform_set_create(uniforms, shader, 0);
	REQUIRE(uniform_set.is_valid());

	fx.dispatch(shader, uniform_set, (count + 63) / 64, 1, 1);

	Vector<uint8_t> output_bytes = rd->buffer_get_data(data_buffer);
	REQUIRE(output_bytes.size() == (int)(count * sizeof(float)));
	const float *output = (const float *)output_bytes.ptr();
	for (uint32_t i = 0; i < count; i++) {
		const float expected = float(i) * 0.5f * 3.0f + float(i);
		CHECK_MESSAGE(output[i] == doctest::Approx(expected), "Mismatch at index ", i);
		if (output[i] != doctest::Approx(expected)) {
			break;
		}
	}
}

TEST_CASE("[WebGPU][Compute] Mid-frame buffer readback observes pending work and resumes recording") {
	WebGPUFixture fx;
	REQUIRE_MESSAGE(fx.init(), "A WebGPU adapter is required for the readback regression test.");
	RenderingDevice *rd = fx.rd;

	RID shader = fx.create_shader(R"(
#version 450
layout(local_size_x = 64) in;
layout(set = 0, binding = 0, std430) buffer Data { uint values[]; };
void main() {
	uint i = gl_GlobalInvocationID.x;
	values[i] = values[i] * 3u + i;
}
)",
			"mid_frame_readback");
	REQUIRE(shader.is_valid());
	RID pipeline = rd->compute_pipeline_create(shader);
	REQUIRE(pipeline.is_valid());
	const uint32_t count = 64;
	Vector<uint32_t> input;
	input.resize(count);
	input.fill(0);
	RID buffer = rd->storage_buffer_create(count * sizeof(uint32_t), Span<uint8_t>((const uint8_t *)input.ptr(), count * sizeof(uint32_t)));
	REQUIRE(buffer.is_valid());
	RenderingDevice::Uniform uniform;
	uniform.uniform_type = RDC::UNIFORM_TYPE_STORAGE_BUFFER;
	uniform.binding = 0;
	uniform.append_id(buffer);
	Vector<RenderingDevice::Uniform> uniforms;
	uniforms.push_back(uniform);
	RID uniform_set = rd->uniform_set_create(uniforms, shader, 0);
	REQUIRE(uniform_set.is_valid());

	for (uint32_t iteration = 0; iteration < 6; iteration++) {
		for (uint32_t i = 0; i < count; i++) {
			input.write[i] = iteration * 1000 + i;
		}
		REQUIRE(rd->buffer_update(buffer, 0, count * sizeof(uint32_t), input.ptr()) == OK);
		RenderingDevice::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, pipeline);
		rd->compute_list_bind_uniform_set(list, uniform_set, 0);
		rd->compute_list_dispatch(list, 1, 1, 1);
		rd->compute_list_end();

		// No submit()/sync(): readback must submit the upload and compute as well as its copy.
		Vector<uint8_t> partial = rd->buffer_get_data(buffer, 7 * sizeof(uint32_t), 13 * sizeof(uint32_t));
		REQUIRE(partial.size() == 13 * sizeof(uint32_t));
		const uint32_t *partial_values = (const uint32_t *)partial.ptr();
		for (uint32_t i = 0; i < 13; i++) {
			CHECK(partial_values[i] == input[i + 7] * 3 + i + 7);
		}
		Vector<uint8_t> full = rd->buffer_get_data(buffer);
		REQUIRE(full.size() == count * sizeof(uint32_t));
		const uint32_t *full_values = (const uint32_t *)full.ptr();
		for (uint32_t i = 0; i < count; i++) {
			CHECK(full_values[i] == input[i] * 3 + i);
		}

		// Exercise immutable mesh-style readbacks and deferred frees in the same frame.
		RID vertex = rd->vertex_buffer_create(full.size(), full);
		REQUIRE(vertex.is_valid());
		CHECK(rd->buffer_get_data(vertex) == full);
		rd->free_rid(vertex);
	}
	// The normal frame fence must still protect submissions after the mid-frame splits.
	rd->submit();
	rd->sync();
	rd->free_rid(uniform_set);
	rd->free_rid(buffer);
	rd->free_rid(pipeline);
	rd->free_rid(shader);
}

TEST_CASE("[WebGPU][Compute] Independent readbacks survive frame reuse and source disposal") {
	WebGPUFixture fx;
	REQUIRE(fx.init());
	RenderingDevice *rd = fx.rd;
	const int iterations = 48;
	Array results;
	results.resize(iterations);
	Dictionary state;
	state["results"] = results;
	state["count"] = 0;
	Vector<uint32_t> values;
	values.resize(64);
	values.fill(0);
	RID buffer = rd->storage_buffer_create(values.size() * sizeof(uint32_t));
	REQUIRE(buffer.is_valid());
	for (int iteration = 0; iteration < iterations; iteration++) {
		for (int i = 0; i < values.size(); i++) {
			values.write[i] = iteration * 1000 + i;
		}
		REQUIRE(rd->buffer_update(buffer, 0, values.size() * sizeof(uint32_t), values.ptr()) == OK);
		REQUIRE(rd->buffer_get_data_async(buffer, callable_mp_static(&collect_independent_readback).bind(state, iteration), 7 * sizeof(uint32_t), 13 * sizeof(uint32_t)) == OK);
		if (iteration == 0) {
			CHECK(int(state["count"]) == 0); // Registration does not invoke or wait for the callback.
		}
		// Exercise the regular frame path, not submit()/sync() (which intentionally waits).
		rd->swap_buffers(false);
	}
	rd->free_rid(buffer);
	for (int i = 0; i < 1000 && int(state["count"]) != iterations; i++) {
		rd->swap_buffers(false);
		OS::get_singleton()->delay_usec(1000);
	}
	REQUIRE(int(state["count"]) == iterations);
	for (int iteration = 0; iteration < iterations; iteration++) {
		PackedByteArray data = results[iteration];
		REQUIRE(data.size() == 13 * sizeof(uint32_t));
		const uint32_t *read = (const uint32_t *)data.ptr();
		for (uint32_t i = 0; i < 13; i++) {
			CHECK(read[i] == uint32_t(iteration * 1000) + i + 7);
		}
	}
}

struct UploadCountingDriver : RenderingDeviceDriverWebGPU {
	int fence_wait_calls = 0;

	UploadCountingDriver(RenderingContextDriverWebGPU *p_context) :
			RenderingDeviceDriverWebGPU(p_context) {}

	Error fence_wait(FenceID p_fence) override {
		fence_wait_calls++;
		return RenderingDeviceDriverWebGPU::fence_wait(p_fence);
	}
};

struct UploadCountingContext : RenderingContextDriverWebGPU {
	UploadCountingDriver *driver = nullptr;

	RenderingDeviceDriver *driver_create() override {
		driver = memnew(UploadCountingDriver(this));
		return driver;
	}
};

TEST_CASE("[WebGPU][Compute] Geometry pages preserve copies clears and neighboring buffers") {
	WebGPUFixture fx;
	REQUIRE(fx.init());
	RenderingDevice *rd = fx.rd;
	Vector<RID> buffers;
	Vector<PackedByteArray> expected;
	for (int i = 0; i < 512; i++) {
		PackedByteArray data;
		data.resize(128);
		for (int j = 0; j < data.size(); j++) {
			data.ptrw()[j] = (i * 31 + j) & 255;
		}
		RID buffer = i % 2 ? rd->vertex_buffer_create(data.size(), data) : rd->index_buffer_create(64, RDC::INDEX_BUFFER_FORMAT_UINT16, data);
		REQUIRE(buffer.is_valid());
		buffers.push_back(buffer);
		expected.push_back(data);
	}
	// Same-page copies require a scratch resource: WebGPU forbids copying a
	// GPUBuffer to itself even when logical source/destination ranges differ.
	REQUIRE(rd->buffer_copy(buffers[1], buffers[3], 12, 24, 32) == OK);
	memcpy(expected.write[3].ptrw() + 24, expected[1].ptr() + 12, 32);
	REQUIRE(rd->buffer_copy(buffers[0], buffers[2], 8, 16, 48) == OK);
	memcpy(expected.write[2].ptrw() + 16, expected[0].ptr() + 8, 48);
	REQUIRE(rd->buffer_clear(buffers[5], 20, 28) == OK);
	memset(expected.write[5].ptrw() + 20, 0, 28);
	for (int i = 0; i < buffers.size(); i++) {
		CHECK(rd->buffer_get_data(buffers[i]) == expected[i]);
		rd->free_rid(buffers[i]);
	}
}

TEST_CASE("[WebGPU][Compute] Geometry page ranges wait for actual submission completion") {
	RenderingContextDriverWebGPU context;
	REQUIRE(context.initialize() == OK);
	RenderingDeviceDriverWebGPU driver(&context);
	REQUIRE(driver.initialize(0, 2) == OK);
	using Driver = RenderingDeviceDriverWebGPU;
	const auto usage = Driver::BUFFER_USAGE_VERTEX_BIT | Driver::BUFFER_USAGE_TRANSFER_TO_BIT | Driver::BUFFER_USAGE_TRANSFER_FROM_BIT;
	const uint64_t before = driver.get_total_memory_used();
	auto prefix = driver.buffer_create(12, usage, Driver::MEMORY_ALLOCATION_TYPE_GPU, 0);
	REQUIRE(bool(prefix));
	auto first = driver.buffer_create(64, usage, Driver::MEMORY_ALLOCATION_TYPE_GPU, 0);
	auto second = driver.buffer_create(64, usage, Driver::MEMORY_ALLOCATION_TYPE_GPU, 0);
	REQUIRE(bool(first));
	REQUIRE(bool(second));
	auto *first_info = (Driver::BufferInfo *)first.id;
	auto *second_info = (Driver::BufferInfo *)second.id;
	REQUIRE(first_info->geometry_page != nullptr);
	CHECK(first_info->buffer == second_info->buffer);
	CHECK(first_info->offset != second_info->offset);
	CHECK(first_info->offset % 16 == 0);
	CHECK(driver.get_total_memory_used() - before == 4 * 1024 * 1024);
	const uint64_t first_offset = first_info->offset;
	uint8_t pattern[64];
	memset(pattern, 0xa5, sizeof(pattern));
	WGPUQueue queue = wgpuDeviceGetQueue(driver.device_get());
	wgpuQueueWriteBuffer(queue, first_info->buffer, first_offset, pattern, sizeof(pattern));
	wgpuQueueRelease(queue);
	// RGBA32F has a sixteen-byte texel block. A preceding twelve-byte logical
	// buffer must not misalign this texture transfer's physical source offset.
	Driver::TextureFormat texture_format;
	texture_format.format = RDC::DATA_FORMAT_R32G32B32A32_SFLOAT;
	texture_format.texture_type = RDC::TEXTURE_TYPE_2D;
	texture_format.width = 4;
	texture_format.height = 1;
	auto texture = driver.texture_create(texture_format, Driver::TextureView());
	REQUIRE(bool(texture));
	auto texture_download = driver.buffer_create(64, Driver::BUFFER_USAGE_TRANSFER_TO_BIT, Driver::MEMORY_ALLOCATION_TYPE_CPU, 0);
	REQUIRE(bool(texture_download));
	Driver::BufferTextureCopyRegion texture_region;
	texture_region.row_pitch = 256;
	texture_region.texture_region_size = Vector3i(4, 1, 1);
	const auto command = driver.command_buffer_create({});
	REQUIRE(driver.command_buffer_begin(command));
	Driver::BufferCopyRegion region = { 0, 0, 64 };
	driver.command_copy_buffer(command, first, second, region);
	driver.command_copy_buffer_to_texture(command, first, texture, Driver::TEXTURE_LAYOUT_COPY_DST_OPTIMAL, texture_region);
	driver.command_copy_texture_to_buffer(command, texture, Driver::TEXTURE_LAYOUT_COPY_SRC_OPTIMAL, texture_download, texture_region);
	driver.command_buffer_end(command);
	REQUIRE(driver.command_queue_execute_and_present(Driver::CommandQueueID(1), {}, command, {}, {}, {}) == OK);
	driver.buffer_free(first);
	auto third = driver.buffer_create(64, usage, Driver::MEMORY_ALLOCATION_TYPE_GPU, 0);
	REQUIRE(bool(third));
	CHECK(((Driver::BufferInfo *)third.id)->offset != first_offset);
	// Existing bounded submission tracking obtains real completion; the pool
	// neither adds an eager fence nor treats submission as GPU completion.
	for (int i = 0; i < 64; i++) {
		REQUIRE(driver.command_buffer_begin(command));
		driver.command_buffer_end(command);
		REQUIRE(driver.command_queue_execute_and_present(Driver::CommandQueueID(1), {}, command, {}, {}, {}) == OK);
	}
	auto fourth = driver.buffer_create(64, usage, Driver::MEMORY_ALLOCATION_TYPE_GPU, 0);
	REQUIRE(bool(fourth));
	CHECK(((Driver::BufferInfo *)fourth.id)->offset == first_offset);
	const uint8_t *texture_data = driver.buffer_map(texture_download);
	REQUIRE(texture_data != nullptr);
	CHECK(memcmp(texture_data, pattern, sizeof(pattern)) == 0);
	driver.buffer_unmap(texture_download);
	driver.buffer_free(texture_download);
	driver.texture_free(texture);
	// Recycled space must not expose the previous logical buffer's contents.
	auto download = driver.buffer_create(64, Driver::BUFFER_USAGE_TRANSFER_TO_BIT, Driver::MEMORY_ALLOCATION_TYPE_CPU, 0);
	REQUIRE(bool(download));
	REQUIRE(driver.command_buffer_begin(command));
	driver.command_copy_buffer(command, fourth, download, region);
	driver.command_buffer_end(command);
	const auto fence = driver.fence_create();
	REQUIRE(driver.command_queue_execute_and_present(Driver::CommandQueueID(1), {}, command, {}, fence, {}) == OK);
	REQUIRE(driver.fence_wait(fence) == OK);
	const uint8_t *mapped = driver.buffer_map(download);
	REQUIRE(mapped != nullptr);
	for (int i = 0; i < 64; i++) {
		CHECK(mapped[i] == 0);
	}
	driver.buffer_unmap(download);
	driver.buffer_free(download);
	driver.fence_free(fence);
	driver.buffer_free(second);
	driver.buffer_free(third);
	driver.buffer_free(fourth);
	driver.buffer_free(prefix);
	CHECK(driver.get_total_memory_used() == before);
	memdelete((Driver::CommandBufferInfo *)command.id);
}

TEST_CASE("[WebGPU][Compute] Upload worker reuse preserves queued snapshots without fence waits") {
	UploadCountingContext context;
	REQUIRE(context.initialize() == OK);
	RenderingDevice rd;
	REQUIRE(rd.initialize(&context) == OK);
	const int initial_waits = context.driver->fence_wait_calls;
	const int iterations = 48;
	Array results;
	results.resize(iterations * 3);
	Dictionary state;
	state["results"] = results;
	state["count"] = 0;
	Vector<PackedByteArray> expected;
	for (int iteration = 0; iteration < iterations; iteration++) {
		for (int batch = 0; batch < 3; batch++) {
			// A and B share unsubmitted staging storage. C forces submission and
			// growth of the initial 16 KiB staging buffer, then later frames reuse it.
			const int bytes = batch < 2 ? 64 + batch * 32 : 32768 + (iteration % 3) * 4096;
			PackedByteArray input;
			input.resize(bytes);
			uint32_t *words = (uint32_t *)input.ptrw();
			for (int i = 0; i < bytes / 4; i++) {
				words[i] = iteration * 100000 + batch * 10000 + i;
			}
			expected.push_back(input);
			RID buffer = rd.vertex_buffer_create(bytes, input);
			REQUIRE(buffer.is_valid());
			const int index = iteration * 3 + batch;
			REQUIRE(rd.buffer_get_data_async(buffer, callable_mp_static(&collect_independent_readback).bind(state, index)) == OK);
			input.fill(0); // The original caller's data must already have been captured.
			rd.free_rid(buffer);
		}
		rd.swap_buffers(false);
	}
	for (int i = 0; i < 1000 && int(state["count"]) != iterations * 3; i++) {
		rd.swap_buffers(false);
		OS::get_singleton()->delay_usec(1000);
	}
	REQUIRE(int(state["count"]) == iterations * 3);
	CHECK(context.driver->fence_wait_calls == initial_waits);
	for (int i = 0; i < expected.size(); i++) {
		PackedByteArray data = results[i];
		CHECK(data == expected[i]);
	}
	// A real explicit completion still exists independently of upload recycling.
	rd.submit();
	rd.sync();
	CHECK(context.driver->fence_wait_calls > initial_waits);
}

TEST_CASE("[WebGPU][Compute] Initial texture uploads reuse staging without waiting") {
	UploadCountingContext context;
	REQUIRE(context.initialize() == OK);
	RenderingDevice rd;
	REQUIRE(rd.initialize(&context) == OK);
	const int initial_waits = context.driver->fence_wait_calls;
	Vector<RID> textures;
	Vector<Vector<uint8_t>> expected;
	for (int iteration = 0; iteration < 24; iteration++) {
		RDC::TextureFormat format;
		format.format = RDC::DATA_FORMAT_R8G8B8A8_UNORM;
		format.texture_type = RDC::TEXTURE_TYPE_3D;
		format.width = 13 + (iteration % 3) * 17;
		format.height = 7 + (iteration % 4) * 5;
		format.depth = 3;
		format.usage_bits = RDC::TEXTURE_USAGE_SAMPLING_BIT | RDC::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		Vector<uint8_t> data;
		data.resize(format.width * format.height * format.depth * 4);
		for (int i = 0; i < data.size(); i++) {
			data.write[i] = uint8_t(iteration * 37 + i * 13);
		}
		expected.push_back(data);
		Vector<Vector<uint8_t>> layers;
		layers.push_back(data);
		RID texture = rd.texture_create(format, RenderingDevice::TextureView(), layers);
		REQUIRE(texture.is_valid());
		textures.push_back(texture);
	}
	CHECK(context.driver->fence_wait_calls == initial_waits);
	for (int i = 0; i < textures.size(); i++) {
		CHECK(rd.texture_get_data(textures[i], 0) == expected[i]);
		rd.free_rid(textures[i]);
	}
	CHECK(context.driver->fence_wait_calls > initial_waits);
}

#ifdef THREADS_ENABLED
struct ConcurrentSubmission {
	RenderingDeviceDriverWebGPU *driver = nullptr;
	RenderingDeviceDriver::CommandBufferID command;
	SafeFlag *start = nullptr;
	Error error = OK;
	int submitted = 0;
};

static void submit_concurrently(void *p_data) {
	ConcurrentSubmission &state = *(ConcurrentSubmission *)p_data;
	while (!state.start->is_set()) {
		OS::get_singleton()->delay_usec(100);
	}
	for (int i = 0; i < 96; i++) {
		if (!state.driver->command_buffer_begin(state.command)) {
			state.error = ERR_CANT_CREATE;
			return;
		}
		state.driver->command_buffer_end(state.command);
		state.error = state.driver->command_queue_execute_and_present(RenderingDeviceDriver::CommandQueueID(1), {}, state.command, {}, {}, {});
		if (state.error != OK) {
			return;
		}
		state.submitted++;
	}
}

TEST_CASE("[WebGPU][Compute] Concurrent queue submissions preserve completion bookkeeping") {
	RenderingContextDriverWebGPU context;
	REQUIRE(context.initialize() == OK);
	RenderingDeviceDriverWebGPU driver(&context);
	REQUIRE(driver.initialize(0, 2) == OK);
	SafeFlag start;
	ConcurrentSubmission states[2];
	Thread threads[2];
	for (int i = 0; i < 2; i++) {
		states[i].driver = &driver;
		states[i].start = &start;
		states[i].command = driver.command_buffer_create({});
		threads[i].start(submit_concurrently, &states[i]);
	}
	start.set();
	for (Thread &thread : threads) {
		thread.wait_to_finish();
	}
	for (ConcurrentSubmission &state : states) {
		CHECK(state.error == OK);
		CHECK(state.submitted == 96);
		// WebGPU's placeholder command pools do not own their command bookkeeping.
		memdelete((RenderingDeviceDriverWebGPU::CommandBufferInfo *)state.command.id);
	}
	RenderingDeviceDriver::FenceID fence = driver.fence_create();
	REQUIRE(driver.command_queue_execute_and_present(RenderingDeviceDriver::CommandQueueID(1), {}, {}, {}, fence, {}) == OK);
	driver.fence_wait(fence);
	CHECK(((RenderingDeviceDriverWebGPU::FenceInfo *)fence.id)->done.is_set());
	driver.fence_free(fence);
}
#endif

TEST_CASE("[WebGPU][Compute] Write-only 3D storage image") {
	WebGPUFixture fx;
	if (!fx.init()) {
		WARN("No WebGPU adapter available, skipping.");
		return;
	}
	RenderingDevice *rd = fx.rd;

	const char *source = R"(
#version 450
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;
layout(set = 0, binding = 0, r32f) uniform writeonly image3D img;
void main() {
	ivec3 p = ivec3(gl_GlobalInvocationID);
	imageStore(img, p, vec4(float(p.x + p.y * 10 + p.z * 100), 0.0, 0.0, 0.0));
}
)";
	RID shader = fx.create_shader(source, "image3d");
	REQUIRE(shader.is_valid());

	RDC::TextureFormat format;
	format.format = RDC::DATA_FORMAT_R32_SFLOAT;
	format.width = 8;
	format.height = 8;
	format.depth = 8;
	format.texture_type = RDC::TEXTURE_TYPE_3D;
	format.usage_bits = RDC::TEXTURE_USAGE_STORAGE_BIT | RDC::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	RID texture = rd->texture_create(format, RenderingDevice::TextureView());
	REQUIRE(texture.is_valid());

	Vector<RenderingDevice::Uniform> uniforms;
	RenderingDevice::Uniform image_uniform;
	image_uniform.uniform_type = RDC::UNIFORM_TYPE_IMAGE;
	image_uniform.binding = 0;
	image_uniform.append_id(texture);
	uniforms.push_back(image_uniform);
	RID uniform_set = rd->uniform_set_create(uniforms, shader, 0);
	REQUIRE(uniform_set.is_valid());

	fx.dispatch(shader, uniform_set, 2, 2, 2);

	Vector<uint8_t> bytes = rd->texture_get_data(texture, 0);
	REQUIRE(bytes.size() == 8 * 8 * 8 * (int)sizeof(float));
	const float *texels = (const float *)bytes.ptr();
	bool all_match = true;
	for (int z = 0; z < 8; z++) {
		for (int y = 0; y < 8; y++) {
			for (int x = 0; x < 8; x++) {
				all_match = all_match && texels[(z * 8 + y) * 8 + x] == float(x + y * 10 + z * 100);
			}
		}
	}
	CHECK(all_match);
}

// Compiles and builds pipelines for the real WoodWorks compute shaders. Set WOODWORKS_SHADERS_DIR to <repo>/godot/shaders.
TEST_CASE("[WebGPU][Compute] WoodWorks shaders create pipelines") {
	String shaders_dir = OS::get_singleton()->get_environment("WOODWORKS_SHADERS_DIR");
	if (shaders_dir.is_empty()) {
		WARN("WOODWORKS_SHADERS_DIR is not set, skipping.");
		return;
	}
	WebGPUFixture fx;
	if (!fx.init()) {
		WARN("No WebGPU adapter available, skipping.");
		return;
	}

	const char *files[] = { "carve.glsl", "shaving_compute.glsl", "surface_probe.glsl", "initial_classify.glsl", "initial_fill.glsl" };
	for (const char *file : files) {
		String source = FileAccess::get_file_as_string(shaders_dir.path_join(file));
		REQUIRE_MESSAGE(!source.is_empty(), "Cannot read ", file);
		// Drop the RDShaderFile `#[compute]` section marker.
		source = source.replace("#[compute]", "");

		CAPTURE(file);
		RID shader = fx.create_shader(source, file);
		CHECK_MESSAGE(shader.is_valid(), "Shader creation failed for ", file);
		if (shader.is_valid()) {
			RID pipeline = fx.rd->compute_pipeline_create(shader);
			CHECK_MESSAGE(pipeline.is_valid(), "Pipeline creation failed for ", file);
		}
	}
}

} // namespace TestWebGPUCompute

#endif // WEBGPU_ENABLED
