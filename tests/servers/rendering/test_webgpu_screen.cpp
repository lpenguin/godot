/**************************************************************************/
/*  test_webgpu_screen.cpp                                                */
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

TEST_FORCE_LINK(test_webgpu_screen)

#if defined(WEBGPU_ENABLED) && defined(WEB_ENABLED)

#include "core/io/file_access.h"
#include "drivers/webgpu/rendering_context_driver_webgpu.h"
#include "servers/rendering/rendering_device.h"

// Draws the baked triangle shader to the page's <canvas id="canvas"> through RenderingDevice's screen path
// (surface, swap chain, screen draw list, swap_buffers). The page keeps showing the last frame.
namespace TestWebGPUScreen {

using RDC = RenderingDeviceCommons;
using RD = RenderingDevice;

constexpr int CANVAS_SIZE = 384;

const float TRIANGLE[3][5] = {
	{ -0.8f, -0.8f, 1.0f, 0.0f, 0.0f },
	{ 0.8f, -0.8f, 0.0f, 1.0f, 0.0f },
	{ 0.0f, 0.8f, 0.0f, 0.0f, 1.0f },
};

TEST_CASE("[WebGPU][Browser][Screen] The triangle is presented to the canvas") {
	const Vector<uint8_t> container = FileAccess::get_file_as_bytes("/webgpu/triangle.bin");
	REQUIRE_MESSAGE(!container.is_empty(), "Cannot read /webgpu/triangle.bin");

	// The context, device and swap chain are intentionally leaked so that the canvas keeps showing the last frame.
	RenderingContextDriverWebGPU *context = memnew(RenderingContextDriverWebGPU);
	REQUIRE_MESSAGE(context->initialize() == OK, "No WebGPU adapter.");
	RenderingContextDriverWebGPU::WindowPlatformData platform_data;
	platform_data.canvas_selector = "#canvas";
	REQUIRE(context->window_create(DisplayServerEnums::MAIN_WINDOW_ID, &platform_data) == OK);
	context->window_set_size(DisplayServerEnums::MAIN_WINDOW_ID, CANVAS_SIZE, CANVAS_SIZE);

	RD *rd = memnew(RD);
	REQUIRE(rd->initialize(context, DisplayServerEnums::MAIN_WINDOW_ID) == OK);
	REQUIRE(rd->screen_create(DisplayServerEnums::MAIN_WINDOW_ID) == OK);

	RID shader = rd->shader_create_from_bytecode(container);
	REQUIRE(shader.is_valid());

	Vector<uint8_t> vertex_bytes;
	vertex_bytes.resize(sizeof(TRIANGLE));
	memcpy(vertex_bytes.ptrw(), TRIANGLE, sizeof(TRIANGLE));
	RID vertex_buffer = rd->vertex_buffer_create(vertex_bytes.size(), Span<uint8_t>(vertex_bytes.ptr(), vertex_bytes.size()));
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
	RID vertex_array = rd->vertex_array_create(3, vertex_format, buffers);

	RID pipeline = rd->render_pipeline_create(shader, rd->screen_get_framebuffer_format(DisplayServerEnums::MAIN_WINDOW_ID), vertex_format, RDC::RENDER_PRIMITIVE_TRIANGLES, RDC::PipelineRasterizationState(), RDC::PipelineMultisampleState(), RDC::PipelineDepthStencilState(), RDC::PipelineColorBlendState::create_disabled(1));
	REQUIRE(pipeline.is_valid());

	const Color clear_color(0.1f, 0.2f, 0.3f, 1.0f);
	for (int frame = 0; frame < 3; frame++) {
		REQUIRE(rd->screen_prepare_for_drawing(DisplayServerEnums::MAIN_WINDOW_ID) == OK);
		RD::DrawListID list = rd->draw_list_begin_for_screen(DisplayServerEnums::MAIN_WINDOW_ID, clear_color);
		rd->draw_list_bind_render_pipeline(list, pipeline);
		rd->draw_list_bind_vertex_array(list, vertex_array);
		rd->draw_list_draw(list, false, 1);
		rd->draw_list_end();
		rd->swap_buffers(true);
	}
	print_line(vformat("SCREEN: presented 3 frames to a %dx%d canvas", rd->screen_get_width(DisplayServerEnums::MAIN_WINDOW_ID), rd->screen_get_height(DisplayServerEnums::MAIN_WINDOW_ID)));

	rd->free_rid(pipeline);
	rd->free_rid(vertex_array);
	rd->free_rid(vertex_buffer);
	rd->free_rid(shader);
}

} // namespace TestWebGPUScreen

#endif // WEBGPU_ENABLED && WEB_ENABLED
