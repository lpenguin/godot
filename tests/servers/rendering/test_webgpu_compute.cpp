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
#include "core/os/os.h"
#include "drivers/webgpu/rendering_context_driver_webgpu.h"
#include "servers/rendering/rendering_device.h"

namespace TestWebGPUCompute {

using RDC = RenderingDeviceCommons;

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
