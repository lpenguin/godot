/**************************************************************************/
/*  test_webgpu_push_constants.cpp                                              */
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

TEST_FORCE_LINK(test_webgpu_push_constants)

#ifdef WEBGPU_ENABLED

#include "core/os/os.h"
#include "drivers/webgpu/rendering_context_driver_webgpu.h"
#include "servers/rendering/rendering_device.h"

// Push constants of WGSL shaders are emulated with a uniform buffer and a dynamic offset (browsers have no immediates).
// Needs GODOT_TINT_PATH so that the shaders are baked to WGSL; otherwise the native immediates are used.
namespace TestWebGPUPushConstants {

using RDC = RenderingDeviceCommons;
using RD = RenderingDevice;

constexpr int SIZE = 64;

const char *VERTEX_SOURCE = R"(
#version 450
layout(push_constant, std430) uniform Params {
	vec4 color;
	vec2 offset;
	vec2 scale;
} pc;
layout(location = 0) in vec2 position;
layout(location = 0) out vec4 vertex_color;
void main() {
	gl_Position = vec4(position * pc.scale + pc.offset, 0.0, 1.0);
	vertex_color = pc.color;
}
)";

const char *FRAGMENT_SOURCE = R"(
#version 450
layout(location = 0) in vec4 vertex_color;
layout(location = 0) out vec4 frag_color;
void main() {
	frag_color = vertex_color;
}
)";

const char *COMPUTE_SOURCE = R"(
#version 450
layout(local_size_x = 1) in;
layout(push_constant, std430) uniform Params {
	uint index;
	uint value;
} pc;
layout(set = 0, binding = 0, std430) buffer restrict Data {
	uint data[];
} data_buffer;
void main() {
	data_buffer.data[pc.index] = pc.value;
}
)";

RID make_shader(RD *p_rd, const Vector<RDC::ShaderStage> &p_stages, const Vector<const char *> &p_sources, const char *p_name) {
	Vector<RDC::ShaderStageSPIRVData> stages;
	for (int i = 0; i < p_stages.size(); i++) {
		String error;
		RDC::ShaderStageSPIRVData stage;
		stage.shader_stage = p_stages[i];
		stage.spirv = p_rd->shader_compile_spirv_from_source(p_stages[i], p_sources[i], RDC::SHADER_LANGUAGE_GLSL, &error, false);
		REQUIRE_MESSAGE(!stage.spirv.is_empty(), error.utf8().get_data());
		stages.push_back(stage);
	}
	return p_rd->shader_create_from_spirv(stages, p_name);
}

TEST_CASE("[WebGPU][PushConstants] Every draw and dispatch sees its own push constants") {
	RenderingContextDriverWebGPU context;
	if (context.initialize() != OK) {
		WARN("No WebGPU adapter available, skipping.");
		return;
	}
	if (OS::get_singleton()->get_environment("GODOT_TINT_PATH").is_empty()) {
		WARN("GODOT_TINT_PATH is not set: the shaders would not be WGSL, skipping.");
		return;
	}
	RD *rd = memnew(RD);
	REQUIRE(rd->initialize(&context) == OK);

	// Compute: two dispatches in one pass write different cells.
	{
		RID shader = make_shader(rd, { RDC::SHADER_STAGE_COMPUTE }, { COMPUTE_SOURCE }, "push_constants_compute");
		REQUIRE(shader.is_valid());
		Vector<uint8_t> zeros;
		zeros.resize(8 * sizeof(uint32_t));
		zeros.fill(0);
		RID buffer = rd->storage_buffer_create(zeros.size(), zeros);
		RD::Uniform uniform;
		uniform.uniform_type = RDC::UNIFORM_TYPE_STORAGE_BUFFER;
		uniform.binding = 0;
		uniform.append_id(buffer);
		Vector<RD::Uniform> uniforms;
		uniforms.push_back(uniform);
		RID uniform_set = rd->uniform_set_create(uniforms, shader, 0);
		RID pipeline = rd->compute_pipeline_create(shader);
		REQUIRE(pipeline.is_valid());

		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, pipeline);
		rd->compute_list_bind_uniform_set(list, uniform_set, 0);
		const uint32_t first[2] = { 3, 111 };
		rd->compute_list_set_push_constant(list, first, sizeof(first));
		rd->compute_list_dispatch(list, 1, 1, 1);
		const uint32_t second[2] = { 5, 222 };
		rd->compute_list_set_push_constant(list, second, sizeof(second));
		rd->compute_list_dispatch(list, 1, 1, 1);
		rd->compute_list_end();
		rd->submit();
		rd->sync();

		const Vector<uint8_t> bytes = rd->buffer_get_data(buffer);
		REQUIRE(bytes.size() == 8 * sizeof(uint32_t));
		const uint32_t *values = (const uint32_t *)bytes.ptr();
		CHECK(values[3] == 111);
		CHECK(values[5] == 222);
		CHECK(values[0] == 0);
		CHECK(values[4] == 0);
		rd->free_rid(pipeline);
		rd->free_rid(uniform_set);
		rd->free_rid(buffer);
		rd->free_rid(shader);
	}

	// Render: two quads drawn with different colors and offsets.
	{
		RID shader = make_shader(rd, { RDC::SHADER_STAGE_VERTEX, RDC::SHADER_STAGE_FRAGMENT }, { VERTEX_SOURCE, FRAGMENT_SOURCE }, "push_constants_render");
		REQUIRE(shader.is_valid());
		RDC::TextureFormat format;
		format.format = RDC::DATA_FORMAT_R8G8B8A8_UNORM;
		format.width = SIZE;
		format.height = SIZE;
		format.usage_bits = RDC::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RDC::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		RID texture = rd->texture_create(format, RD::TextureView());
		Vector<RID> attachments;
		attachments.push_back(texture);
		RID framebuffer = rd->framebuffer_create(attachments);

		const float quad[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
		Vector<uint8_t> vertex_bytes;
		vertex_bytes.resize(sizeof(quad));
		memcpy(vertex_bytes.ptrw(), quad, sizeof(quad));
		RID vertex_buffer = rd->vertex_buffer_create(vertex_bytes.size(), vertex_bytes);
		RD::VertexAttribute attribute;
		attribute.location = 0;
		attribute.offset = 0;
		attribute.stride = 2 * sizeof(float);
		attribute.format = RDC::DATA_FORMAT_R32G32_SFLOAT;
		Vector<RD::VertexAttribute> attributes;
		attributes.push_back(attribute);
		const RD::VertexFormatID vertex_format = rd->vertex_format_create(attributes);
		Vector<RID> buffers;
		buffers.push_back(vertex_buffer);
		RID vertex_array = rd->vertex_array_create(6, vertex_format, buffers);
		RID pipeline = rd->render_pipeline_create(shader, rd->framebuffer_get_format(framebuffer), vertex_format, RDC::RENDER_PRIMITIVE_TRIANGLES, RDC::PipelineRasterizationState(), RDC::PipelineMultisampleState(), RDC::PipelineDepthStencilState(), RDC::PipelineColorBlendState::create_disabled(1));
		REQUIRE(pipeline.is_valid());

		struct Params {
			float color[4];
			float offset[2];
			float scale[2];
		};
		const Params red = { { 1, 0, 0, 1 }, { -0.5f, -0.5f }, { 0.4f, 0.4f } };
		const Params green = { { 0, 1, 0, 1 }, { 0.5f, 0.5f }, { 0.4f, 0.4f } };
		RD::DrawListID list = rd->draw_list_begin(framebuffer, RD::DRAW_CLEAR_COLOR_0, VectorView<Color>(Color(0, 0, 0, 1)));
		rd->draw_list_bind_render_pipeline(list, pipeline);
		rd->draw_list_bind_vertex_array(list, vertex_array);
		rd->draw_list_set_push_constant(list, &red, sizeof(red));
		rd->draw_list_draw(list, false, 1);
		rd->draw_list_set_push_constant(list, &green, sizeof(green));
		rd->draw_list_draw(list, false, 1);
		rd->draw_list_end();
		rd->submit();
		rd->sync();

		const Vector<uint8_t> pixels = rd->texture_get_data(texture, 0);
		REQUIRE(pixels.size() == SIZE * SIZE * 4);
		auto pixel = [&](int p_x, int p_y, int p_channel) { return (int)pixels[(p_y * SIZE + p_x) * 4 + p_channel]; };
		// Vulkan convention: clip Y points down, so negative offsets land in the upper left of the image.
		CHECK(pixel(16, 16, 0) == 255);
		CHECK(pixel(16, 16, 1) == 0);
		CHECK(pixel(48, 48, 0) == 0);
		CHECK(pixel(48, 48, 1) == 255);
		CHECK(pixel(32, 32, 0) == 0);
		CHECK(pixel(32, 32, 1) == 0);
		CHECK(pixel(16, 48, 0) == 0);
		CHECK(pixel(16, 48, 1) == 0);
		rd->free_rid(pipeline);
		rd->free_rid(vertex_array);
		rd->free_rid(vertex_buffer);
		rd->free_rid(framebuffer);
		rd->free_rid(texture);
		rd->free_rid(shader);
	}
	memdelete(rd);
}

} // namespace TestWebGPUPushConstants

#endif // WEBGPU_ENABLED
