/**************************************************************************/
/*  test_webgpu_triangle.cpp                                              */
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

TEST_FORCE_LINK(test_webgpu_triangle)

#ifdef WEBGPU_ENABLED

#include "core/io/file_access.h"
#include "core/os/os.h"
#include "drivers/webgpu/rendering_context_driver_webgpu.h"
#include "servers/rendering/rendering_device.h"

#ifdef VULKAN_ENABLED
#ifdef WINDOWS_ENABLED
#include "platform/windows/rendering_context_driver_vulkan_windows.h"
using TestVulkanContext = RenderingContextDriverVulkanWindows;
#else
#include "platform/linuxbsd/x11/rendering_context_driver_vulkan_x11.h"
using TestVulkanContext = RenderingContextDriverVulkanX11;
#endif
#endif

// Renders a colored triangle into an offscreen RGBA8 texture through RenderingDevice (render pass, framebuffer,
// vertex format and buffers, render pipeline, draw list) and reads the pixels back.
namespace TestWebGPUTriangle {

using RDC = RenderingDeviceCommons;
using RD = RenderingDevice;

constexpr int SIZE = 64;
const Color CLEAR_COLOR(0.1f, 0.2f, 0.3f, 1.0f);

const char *VERTEX_SOURCE = R"(
#version 450
layout(location = 0) in vec2 position;
layout(location = 1) in vec3 color;
layout(location = 0) out vec3 vertex_color;
void main() {
	gl_Position = vec4(position, 0.0, 1.0);
	vertex_color = color;
}
)";

const char *FRAGMENT_SOURCE = R"(
#version 450
layout(location = 0) in vec3 vertex_color;
layout(location = 0) out vec4 frag_color;
void main() {
	frag_color = vec4(vertex_color, 1.0);
}
)";

// Corners in clip space (Vulkan convention, Y down) with their colors.
const float TRIANGLE[3][5] = {
	{ -0.8f, -0.8f, 1.0f, 0.0f, 0.0f },
	{ 0.8f, -0.8f, 0.0f, 1.0f, 0.0f },
	{ 0.0f, 0.8f, 0.0f, 0.0f, 1.0f },
};

Vector<uint8_t> render_triangle(RD *p_rd, const String &p_baked_dir, const String &p_bake_dir) {
	RD *rd = p_rd;
	Vector<RID> owned;
	auto track = [&](RID p_rid) {
		owned.push_back(p_rid);
		return p_rid;
	};

	RID shader;
	if (!p_baked_dir.is_empty()) {
		const Vector<uint8_t> container = FileAccess::get_file_as_bytes(p_baked_dir.path_join("triangle.bin"));
		if (container.is_empty()) {
			ERR_PRINT("Cannot read the baked triangle shader.");
			return Vector<uint8_t>();
		}
		shader = track(rd->shader_create_from_bytecode(container));
	} else {
		Vector<RDC::ShaderStageSPIRVData> stages;
		const struct {
			RDC::ShaderStage stage;
			const char *source;
		} sources[] = { { RDC::SHADER_STAGE_VERTEX, VERTEX_SOURCE }, { RDC::SHADER_STAGE_FRAGMENT, FRAGMENT_SOURCE } };
		for (const auto &entry : sources) {
			String error;
			RDC::ShaderStageSPIRVData stage;
			stage.shader_stage = entry.stage;
			stage.spirv = rd->shader_compile_spirv_from_source(entry.stage, entry.source, RDC::SHADER_LANGUAGE_GLSL, &error, false);
			if (stage.spirv.is_empty()) {
				ERR_PRINT("Cannot compile the triangle shader: " + error);
				return Vector<uint8_t>();
			}
			stages.push_back(stage);
		}
		if (!p_bake_dir.is_empty()) {
			const Vector<uint8_t> container = rd->shader_compile_binary_from_spirv(stages, "triangle");
			Ref<FileAccess> out = FileAccess::open(p_bake_dir.path_join("triangle.bin"), FileAccess::WRITE);
			if (out.is_valid() && !container.is_empty()) {
				out->store_buffer(container.ptr(), container.size());
			} else {
				ERR_PRINT("Cannot bake the triangle shader.");
			}
		}
		shader = track(rd->shader_create_from_spirv(stages, "triangle"));
	}
	if (!shader.is_valid()) {
		return Vector<uint8_t>();
	}

	RDC::TextureFormat format;
	format.format = RDC::DATA_FORMAT_R8G8B8A8_UNORM;
	format.width = SIZE;
	format.height = SIZE;
	format.usage_bits = RDC::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RDC::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	RID texture = track(rd->texture_create(format, RD::TextureView()));
	Vector<RID> attachments;
	attachments.push_back(texture);
	RID framebuffer = track(rd->framebuffer_create(attachments));

	Vector<uint8_t> vertex_bytes;
	vertex_bytes.resize(sizeof(TRIANGLE));
	memcpy(vertex_bytes.ptrw(), TRIANGLE, sizeof(TRIANGLE));
	RID vertex_buffer = track(rd->vertex_buffer_create(vertex_bytes.size(), Span<uint8_t>(vertex_bytes.ptr(), vertex_bytes.size())));

	Vector<RD::VertexAttribute> attributes;
	RD::VertexAttribute position;
	position.binding = 0;
	position.location = 0;
	position.offset = 0;
	position.stride = 5 * sizeof(float);
	position.format = RDC::DATA_FORMAT_R32G32_SFLOAT;
	attributes.push_back(position);
	RD::VertexAttribute color;
	color.binding = 0;
	color.location = 1;
	color.offset = 2 * sizeof(float);
	color.stride = 5 * sizeof(float);
	color.format = RDC::DATA_FORMAT_R32G32B32_SFLOAT;
	attributes.push_back(color);
	const RD::VertexFormatID vertex_format = rd->vertex_format_create(attributes);
	Vector<RID> buffers;
	buffers.push_back(vertex_buffer);
	RID vertex_array = track(rd->vertex_array_create(3, vertex_format, buffers));

	RID pipeline = track(rd->render_pipeline_create(shader, rd->framebuffer_get_format(framebuffer), vertex_format, RDC::RENDER_PRIMITIVE_TRIANGLES, RDC::PipelineRasterizationState(), RDC::PipelineMultisampleState(), RDC::PipelineDepthStencilState(), RDC::PipelineColorBlendState::create_disabled(1)));
	if (!pipeline.is_valid()) {
		return Vector<uint8_t>();
	}

	RD::DrawListID list = rd->draw_list_begin(framebuffer, RD::DRAW_CLEAR_COLOR_0, VectorView<Color>(CLEAR_COLOR));
	rd->draw_list_bind_render_pipeline(list, pipeline);
	rd->draw_list_bind_vertex_array(list, vertex_array);
	rd->draw_list_draw(list, false, 1);
	rd->draw_list_end();
	rd->submit();
	rd->sync();

	Vector<uint8_t> pixels = rd->texture_get_data(texture, 0);
	for (int i = owned.size() - 1; i >= 0; i--) {
		if (owned[i].is_valid()) {
			rd->free_rid(owned[i]);
		}
	}
	return pixels;
}

// Checks every pixel that is clearly inside or outside of the triangle against the analytic result.
// Returns the number of wrong pixels and reports how many were checked.
int check_pixels(const Vector<uint8_t> &p_pixels, int &r_inside, int &r_outside) {
	int wrong = 0;
	r_inside = 0;
	r_outside = 0;
	if (p_pixels.size() != SIZE * SIZE * 4) {
		return -1;
	}
	const Vector2 a(TRIANGLE[0][0], TRIANGLE[0][1]);
	const Vector2 b(TRIANGLE[1][0], TRIANGLE[1][1]);
	const Vector2 c(TRIANGLE[2][0], TRIANGLE[2][1]);
	const float area = (b - a).cross(c - a);
	for (int y = 0; y < SIZE; y++) {
		for (int x = 0; x < SIZE; x++) {
			// Vulkan convention: clip space Y points down, so the image row index grows with clip Y.
			const Vector2 point((x + 0.5f) / SIZE * 2.0f - 1.0f, (y + 0.5f) / SIZE * 2.0f - 1.0f);
			const float wa = (b - point).cross(c - point) / area;
			const float wb = (c - point).cross(a - point) / area;
			const float wc = 1.0f - wa - wb;
			const float margin = MIN(wa, MIN(wb, wc));
			const uint8_t *pixel = p_pixels.ptr() + (y * SIZE + x) * 4;
			if (margin > 0.04f) {
				r_inside++;
				const float expected[3] = { wa * 255.0f, wb * 255.0f, wc * 255.0f };
				bool ok = pixel[3] == 255;
				for (int channel = 0; channel < 3; channel++) {
					ok = ok && Math::abs(pixel[channel] - expected[channel]) <= 3.0f;
				}
				wrong += !ok;
			} else if (margin < -0.04f) {
				r_outside++;
				const bool ok = Math::abs(pixel[0] - CLEAR_COLOR.r * 255.0f) <= 1.5f && Math::abs(pixel[1] - CLEAR_COLOR.g * 255.0f) <= 1.5f && Math::abs(pixel[2] - CLEAR_COLOR.b * 255.0f) <= 1.5f && pixel[3] == 255;
				wrong += !ok;
			}
		}
	}
	return wrong;
}

int count_different(const Vector<uint8_t> &p_a, const Vector<uint8_t> &p_b, int p_tolerance) {
	int different = 0;
	for (int i = 0; i < SIZE * SIZE; i++) {
		bool differs = false;
		for (int channel = 0; channel < 4; channel++) {
			differs = differs || Math::abs((int)p_a[i * 4 + channel] - (int)p_b[i * 4 + channel]) > p_tolerance;
		}
		different += differs;
	}
	return different;
}

String baked_directory() {
#ifdef WEB_ENABLED
	return "/webgpu";
#else
	return OS::get_singleton()->get_environment("WEBGPU_BAKED_DIR");
#endif
}

TEST_CASE("[WebGPU][Triangle] Triangle rendered to a texture matches the analytic result and Vulkan") {
	RenderingContextDriverWebGPU webgpu_context;
	if (webgpu_context.initialize() != OK) {
		WARN("No WebGPU adapter available, skipping.");
		return;
	}
	RD *webgpu_rd = memnew(RD);
	REQUIRE(webgpu_rd->initialize(&webgpu_context) == OK);
	const Vector<uint8_t> web = render_triangle(webgpu_rd, String(), String());
	memdelete(webgpu_rd);
	REQUIRE_MESSAGE(!web.is_empty(), "WebGPU rendering failed.");

	int inside = 0;
	int outside = 0;
	const int wrong = check_pixels(web, inside, outside);
	print_line(vformat("WebGPU triangle: %d interior and %d exterior pixels checked, %d wrong", inside, outside, wrong));
	CHECK(inside > 500);
	CHECK(outside > 500);
	CHECK(wrong == 0);

#ifdef VULKAN_ENABLED
	TestVulkanContext vulkan_context;
	if (vulkan_context.initialize() != OK) {
		WARN("Vulkan is unavailable, skipping the comparison.");
		return;
	}
	RD *vulkan_rd = memnew(RD);
	REQUIRE(vulkan_rd->initialize(&vulkan_context) == OK);
	const Vector<uint8_t> vulkan = render_triangle(vulkan_rd, String(), String());
	memdelete(vulkan_rd);
	REQUIRE(!vulkan.is_empty());
	const int different = count_different(web, vulkan, 2);
	print_line(vformat("WebGPU vs Vulkan: %d of %d pixels differ by more than 2", different, SIZE * SIZE));
	CHECK(different <= 12); // Only rasterization differences along the triangle edges are acceptable.
#endif
}

// Desktop only: bakes the triangle shader and stores the Vulkan image as the reference for browser runs.
TEST_CASE("[WebGPU][Bake] Bake the triangle shader and the Vulkan reference image") {
	const String bake_dir = OS::get_singleton()->get_environment("WEBGPU_BAKE_DIR");
	if (bake_dir.is_empty()) {
		WARN("WEBGPU_BAKE_DIR is not set, skipping.");
		return;
	}
#ifdef VULKAN_ENABLED
	RenderingContextDriverWebGPU webgpu_context;
	REQUIRE(webgpu_context.initialize() == OK);
	RD *webgpu_rd = memnew(RD);
	REQUIRE(webgpu_rd->initialize(&webgpu_context) == OK);
	const Vector<uint8_t> baked_run = render_triangle(webgpu_rd, String(), bake_dir);
	memdelete(webgpu_rd);
	REQUIRE(!baked_run.is_empty());

	TestVulkanContext vulkan_context;
	REQUIRE(vulkan_context.initialize() == OK);
	RD *vulkan_rd = memnew(RD);
	REQUIRE(vulkan_rd->initialize(&vulkan_context) == OK);
	const Vector<uint8_t> reference = render_triangle(vulkan_rd, String(), String());
	memdelete(vulkan_rd);
	REQUIRE(!reference.is_empty());
	Ref<FileAccess> out = FileAccess::open(bake_dir.path_join("triangle_reference.bin"), FileAccess::WRITE);
	REQUIRE(out.is_valid());
	out->store_buffer(reference.ptr(), reference.size());
#else
	WARN("Vulkan is required to produce the reference image, skipping.");
#endif
}

// Replays the baked triangle shader on WebGPU (natively or in the browser) and checks it against the analytic result
// and the Vulkan reference image.
TEST_CASE("[WebGPU][Browser] Baked triangle shader renders the same image as Vulkan") {
	const String baked = baked_directory();
	if (baked.is_empty()) {
		WARN("WEBGPU_BAKED_DIR is not set, skipping.");
		return;
	}
	const Vector<uint8_t> reference = FileAccess::get_file_as_bytes(baked.path_join("triangle_reference.bin"));
	REQUIRE_MESSAGE(reference.size() == SIZE * SIZE * 4, "Cannot read triangle_reference.bin");

	RenderingContextDriverWebGPU context;
	REQUIRE_MESSAGE(context.initialize() == OK, "No WebGPU adapter.");
	RD *rd = memnew(RD);
	REQUIRE(rd->initialize(&context) == OK);
	const Vector<uint8_t> pixels = render_triangle(rd, baked, String());
	memdelete(rd);
	REQUIRE_MESSAGE(!pixels.is_empty(), "Rendering failed.");

	int inside = 0;
	int outside = 0;
	const int wrong = check_pixels(pixels, inside, outside);
	const int different = count_different(pixels, reference, 2);
	print_line(vformat("triangle: %d interior and %d exterior pixels checked, %d wrong; %d of %d pixels differ from the Vulkan reference", inside, outside, wrong, different, SIZE * SIZE));
	CHECK(wrong == 0);
	CHECK(different <= 12);
}

} // namespace TestWebGPUTriangle

#endif // WEBGPU_ENABLED
