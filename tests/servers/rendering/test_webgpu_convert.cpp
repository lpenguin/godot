/**************************************************************************/
/*  test_webgpu_convert.cpp                                               */
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

TEST_FORCE_LINK(test_webgpu_convert)

#ifdef WEBGPU_ENABLED

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "drivers/webgpu/rendering_shader_container_webgpu.h"

// Measures how many of the SPIR-V modules dumped by a real engine run (GODOT_DUMP_SPIRV_DIR) survive the WebGPU shader
// container, which strips write-only buffers, flips the vertex Y axis and converts to WGSL with Tint.
//   WEBGPU_SPIRV_DIR=<dump dir> GODOT_TINT_PATH=<tint> godot --test --test-case="*Convert*"
namespace TestWebGPUConvert {

using RDC = RenderingDeviceCommons;

TEST_CASE("[WebGPU][Convert] Dumped engine shaders convert to WGSL") {
	const String directory = OS::get_singleton()->get_environment("WEBGPU_SPIRV_DIR");
	if (directory.is_empty() || OS::get_singleton()->get_environment("GODOT_TINT_PATH").is_empty()) {
		WARN("WEBGPU_SPIRV_DIR or GODOT_TINT_PATH is not set, skipping.");
		return;
	}

	// Group the stage files of one shader: <base>.<stage>.spv.
	HashMap<String, Vector<RDC::ShaderStageSPIRVData>> shaders;
	Vector<String> order;
	{
		Ref<DirAccess> dir = DirAccess::open(directory);
		REQUIRE(dir.is_valid());
		dir->list_dir_begin();
		for (String file = dir->get_next(); !file.is_empty(); file = dir->get_next()) {
			if (!file.ends_with(".spv")) {
				continue;
			}
			const String without_extension = file.get_basename(); // <base>.<stage>
			const String stage_name = without_extension.get_extension();
			const String base = without_extension.get_basename();
			RDC::ShaderStageSPIRVData stage;
			if (stage_name == "vert") {
				stage.shader_stage = RDC::SHADER_STAGE_VERTEX;
			} else if (stage_name == "frag") {
				stage.shader_stage = RDC::SHADER_STAGE_FRAGMENT;
			} else if (stage_name == "comp") {
				stage.shader_stage = RDC::SHADER_STAGE_COMPUTE;
			} else {
				continue; // Tessellation is not supported by WebGPU.
			}
			stage.spirv = FileAccess::get_file_as_bytes(directory.path_join(file));
			if (!shaders.has(base)) {
				order.push_back(base);
			}
			shaders[base].push_back(stage);
		}
		dir->list_dir_end();
	}
	order.sort();

	RenderingShaderContainerFormatWebGPU format;
	int converted = 0;
	int failed = 0;
	Vector<String> failures;
	for (const String &base : order) {
		Ref<RenderingShaderContainer> container = format.create_container();
		if (container->set_code_from_spirv(base, shaders[base])) {
			converted++;
		} else {
			failed++;
			failures.push_back(base);
		}
	}
	for (const String &name : failures) {
		print_line("CONVERT_FAIL " + name);
	}
	print_line(vformat("CONVERT: %d shaders, %d converted, %d failed", order.size(), converted, failed));
	CHECK(order.size() > 0);
}

} // namespace TestWebGPUConvert

#endif // WEBGPU_ENABLED
