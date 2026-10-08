/**************************************************************************/
/*  test_webgpu_parity.cpp                                                */
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

TEST_FORCE_LINK(test_webgpu_parity)

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

// Runs the WoodWorks sparse-SDF compute pipeline (classify, fill, carve, shaving, surface probe) on a
// RenderingDevice and collects every output, so that the WebGPU and Vulkan drivers can be compared.
namespace TestWebGPUParity {

using RDC = RenderingDeviceCommons;
using RD = RenderingDevice;

const Vector3i GRID(14, 13, 12);
const Vector3i COARSE(7, 6, 6);
const float ORIGIN = -1.5f;
const Vector3 HALF(0.65f, 0.9f, 0.7f);
const float SPACING = 0.25f;
const float EPSILON = 0.0625f;
const int PHYSICAL = 5;
const int ATLAS_GRID = 7;
const int SIDE = PHYSICAL * ATLAS_GRID;

struct Outputs {
	Vector<uint8_t> states;
	Vector<uint8_t> pages;
	Vector<uint8_t> atlas_initial;
	Vector<uint8_t> carve_atlas;
	Vector<uint8_t> carve_pages;
	Vector<uint8_t> carve_page_image;
	Vector<uint8_t> carve_target_atlas;
	Vector<uint8_t> carve_target_pages;
	Vector<uint8_t> shaving_target;
	Vector<uint8_t> shaving_result;
	Vector<uint8_t> probe_down;
	Vector<uint8_t> probe_ray;
	Vector<float> expected_atlas; // CPU reference for the initial fill.
	Vector<uint32_t> expected_states;
	bool ok = false;
};

struct ParamWriter {
	Vector<uint8_t> bytes;
	void f(float p_value) {
		const uint8_t *raw = (const uint8_t *)&p_value;
		for (int i = 0; i < 4; i++) {
			bytes.push_back(raw[i]);
		}
	}
	void u(uint32_t p_value) {
		const uint8_t *raw = (const uint8_t *)&p_value;
		for (int i = 0; i < 4; i++) {
			bytes.push_back(raw[i]);
		}
	}
	void v4(float p_x, float p_y, float p_z, float p_w) {
		f(p_x);
		f(p_y);
		f(p_z);
		f(p_w);
	}
	void u4(uint32_t p_x, uint32_t p_y, uint32_t p_z, uint32_t p_w) {
		u(p_x);
		u(p_y);
		u(p_z);
		u(p_w);
	}
};

float box_distance(const Vector3i &p_cell) {
	const Vector3i clamped(CLAMP(p_cell.x, 0, GRID.x - 1), CLAMP(p_cell.y, 0, GRID.y - 1), CLAMP(p_cell.z, 0, GRID.z - 1));
	const Vector3 point(ORIGIN + clamped.x * SPACING, ORIGIN + clamped.y * SPACING, ORIGIN + clamped.z * SPACING);
	const Vector3 q = point.abs() - HALF;
	const Vector3 positive(MAX(q.x, 0.0f), MAX(q.y, 0.0f), MAX(q.z, 0.0f));
	return positive.length() + MIN(MAX(q.x, MAX(q.y, q.z)), 0.0f);
}

struct Item {
	RDC::UniformType type;
	RID rid;
};

struct Runner {
	RD *rd = nullptr;
	Vector<RID> owned;
	// When set, shaders are loaded from baked containers in this directory instead of being compiled from GLSL.
	String baked_dir;
	// When set, every compiled shader is also written here as a container (<name>.bin).
	String bake_dir;

	RID track(RID p_rid) {
		owned.push_back(p_rid);
		return p_rid;
	}

	RID shader(const String &p_path, RID &r_pipeline) {
		if (!baked_dir.is_empty()) {
			const Vector<uint8_t> container = FileAccess::get_file_as_bytes(baked_dir.path_join(p_path.get_file().get_basename() + ".bin"));
			if (container.is_empty()) {
				ERR_PRINT("Cannot read the baked shader for " + p_path);
				return RID();
			}
			RID baked_shader = track(rd->shader_create_from_bytecode(container));
			r_pipeline = track(rd->compute_pipeline_create(baked_shader));
			return baked_shader;
		}
		String source = FileAccess::get_file_as_string(p_path).replace("#[compute]", "");
		String error;
		Vector<uint8_t> spirv = rd->shader_compile_spirv_from_source(RDC::SHADER_STAGE_COMPUTE, source, RDC::SHADER_LANGUAGE_GLSL, &error, false);
		if (spirv.is_empty()) {
			ERR_PRINT(vformat("Cannot compile %s: %s", p_path, error));
			return RID();
		}
		Vector<RDC::ShaderStageSPIRVData> stages;
		RDC::ShaderStageSPIRVData stage;
		stage.shader_stage = RDC::SHADER_STAGE_COMPUTE;
		stage.spirv = spirv;
		stages.push_back(stage);
		if (!bake_dir.is_empty()) {
			const Vector<uint8_t> container = rd->shader_compile_binary_from_spirv(stages, p_path);
			Ref<FileAccess> out = FileAccess::open(bake_dir.path_join(p_path.get_file().get_basename() + ".bin"), FileAccess::WRITE);
			if (out.is_valid() && !container.is_empty()) {
				out->store_buffer(container.ptr(), container.size());
			} else {
				ERR_PRINT("Cannot bake " + p_path);
			}
		}
		RID shader_rid = track(rd->shader_create_from_spirv(stages, p_path));
		r_pipeline = track(rd->compute_pipeline_create(shader_rid));
		return shader_rid;
	}

	RID storage(const Vector<uint8_t> &p_data) {
		return track(rd->storage_buffer_create(p_data.size(), Span<uint8_t>(p_data.ptr(), p_data.size())));
	}

	RID uniform_buffer(const Vector<uint8_t> &p_data) {
		return track(rd->uniform_buffer_create(p_data.size(), Span<uint8_t>(p_data.ptr(), p_data.size())));
	}

	RID texture(RDC::DataFormat p_format, RDC::TextureType p_type, uint32_t p_w, uint32_t p_h, uint32_t p_d, const Vector<uint8_t> &p_data, bool p_sampling = false) {
		RDC::TextureFormat format;
		format.format = p_format;
		format.texture_type = p_type;
		format.width = p_w;
		format.height = p_h;
		format.depth = p_d;
		format.usage_bits = RDC::TEXTURE_USAGE_STORAGE_BIT | RDC::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RDC::TEXTURE_USAGE_CAN_UPDATE_BIT;
		if (p_sampling) {
			format.usage_bits |= RDC::TEXTURE_USAGE_SAMPLING_BIT;
		}
		Vector<Vector<uint8_t>> data;
		if (!p_data.is_empty()) {
			data.push_back(p_data);
		}
		return track(rd->texture_create(format, RD::TextureView(), data));
	}

	RID set(RID p_shader, std::initializer_list<Item> p_items) {
		Vector<RD::Uniform> uniforms;
		uint32_t binding = 0;
		for (const Item &item : p_items) {
			RD::Uniform uniform;
			uniform.uniform_type = item.type;
			uniform.binding = binding++;
			uniform.append_id(item.rid);
			uniforms.push_back(uniform);
		}
		return track(rd->uniform_set_create(uniforms, p_shader, 0));
	}

	void dispatch(RID p_pipeline, RID p_set, Vector3i p_groups) {
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, p_pipeline);
		rd->compute_list_bind_uniform_set(list, p_set, 0);
		rd->compute_list_dispatch(list, p_groups.x, p_groups.y, p_groups.z);
		rd->compute_list_end();
		rd->submit();
		rd->sync();
	}

	Vector<uint8_t> tex_data(RID p_texture) { return rd->texture_get_data(p_texture, 0); }
	Vector<uint8_t> buf_data(RID p_buffer) { return rd->buffer_get_data(p_buffer); }

	~Runner() {
		for (int i = owned.size() - 1; i >= 0; i--) {
			if (owned[i].is_valid()) {
				rd->free_rid(owned[i]);
			}
		}
	}
};

Vector<uint8_t> float_bytes(const Vector<float> &p_values) {
	Vector<uint8_t> out;
	out.resize(p_values.size() * sizeof(float));
	memcpy(out.ptrw(), p_values.ptr(), out.size());
	return out;
}

void cutter_params(ParamWriter &w) {
	w.v4(0.0f, 0.05f, 0.0f, 0.6f); // Center, half length.
	w.v4(1.0f, 0.0f, 0.0f, 0.5f); // Direction, half width.
	w.v4(0.0f, 1.0f, 0.0f, 0.35f); // Side, half height.
	w.v4(0.0f, 0.0f, 1.0f, 0.25f); // Up, bevel length.
}

Outputs run_scenario(RD *p_rd, const String &p_shaders, const String &p_baked_dir = String(), const String &p_bake_dir = String()) {
	Outputs out;
	Runner r;
	r.rd = p_rd;
	r.baked_dir = p_baked_dir;
	r.bake_dir = p_bake_dir;

	const int coarse_count = COARSE.x * COARSE.y * COARSE.z;

	// ---- Initial classification ----
	RID classify_pipeline;
	RID classify_shader = r.shader(p_shaders.path_join("initial_classify.glsl"), classify_pipeline);
	RID fill_pipeline;
	RID fill_shader = r.shader(p_shaders.path_join("initial_fill.glsl"), fill_pipeline);
	if (!classify_shader.is_valid() || !fill_shader.is_valid()) {
		return out;
	}

	ParamWriter classify_params;
	classify_params.v4(ORIGIN, ORIGIN, ORIGIN, SPACING);
	classify_params.v4(0, 0, 0, EPSILON);
	classify_params.v4(HALF.x, HALF.y, HALF.z, 0);
	classify_params.u4(GRID.x, GRID.y, GRID.z, 2);
	classify_params.u4(COARSE.x, COARSE.y, COARSE.z, 0);

	Vector<uint8_t> zero_states;
	zero_states.resize(coarse_count * 4);
	zero_states.fill(0);
	RID states_buffer = r.storage(zero_states);
	RID classify_params_buffer = r.uniform_buffer(classify_params.bytes);
	RID classify_set = r.set(classify_shader, { { RDC::UNIFORM_TYPE_UNIFORM_BUFFER, classify_params_buffer }, { RDC::UNIFORM_TYPE_STORAGE_BUFFER, states_buffer } });
	r.dispatch(classify_pipeline, classify_set, COARSE);
	out.states = r.buf_data(states_buffer);

	// CPU reference for the classification and page table.
	Vector<uint8_t> pages_bytes;
	pages_bytes.resize(coarse_count * 4);
	Vector<float> expected;
	expected.resize(SIDE * SIDE * SIDE);
	expected.fill(99.0f);
	int slots = 0;
	for (int z = 0; z < COARSE.z; z++) {
		for (int y = 0; y < COARSE.y; y++) {
			for (int x = 0; x < COARSE.x; x++) {
				const Vector3i cell(x, y, z);
				const int index = x + COARSE.x * (y + COARSE.y * z);
				float minimum = INFINITY;
				float maximum = -INFINITY;
				for (int sz = 0; sz < 3; sz++) {
					for (int sy = 0; sy < 3; sy++) {
						for (int sx = 0; sx < 3; sx++) {
							const float value = box_distance(cell * 2 + Vector3i(sx, sy, sz));
							minimum = MIN(minimum, value);
							maximum = MAX(maximum, value);
						}
					}
				}
				const uint32_t state = minimum > EPSILON ? 0 : (maximum < -EPSILON ? 1 : 2);
				out.expected_states.push_back(state);
				const uint32_t entry = state == 0 ? 0 : (uint32_t(slots) << 2) | state;
				memcpy(pages_bytes.ptrw() + index * 4, &entry, 4);
				if (state == 2) {
					const Vector3i start(slots % ATLAS_GRID * PHYSICAL, (slots / ATLAS_GRID) % ATLAS_GRID * PHYSICAL, slots / (ATLAS_GRID * ATLAS_GRID) * PHYSICAL);
					for (int sz = 0; sz < PHYSICAL; sz++) {
						for (int sy = 0; sy < PHYSICAL; sy++) {
							for (int sx = 0; sx < PHYSICAL; sx++) {
								const Vector3i position = start + Vector3i(sx, sy, sz);
								expected.write[position.x + SIDE * (position.y + SIDE * position.z)] = box_distance(cell * 2 - Vector3i(1, 1, 1) + Vector3i(sx, sy, sz));
							}
						}
					}
				}
				if (state != 0) {
					slots++;
				}
			}
		}
	}
	out.expected_atlas = expected;
	out.pages = pages_bytes;

	// ---- Initial fill ----
	Vector<float> initial_fill;
	initial_fill.resize(SIDE * SIDE * SIDE);
	initial_fill.fill(99.0f);
	RID pages_buffer = r.storage(pages_bytes);
	RID atlas = r.texture(RDC::DATA_FORMAT_R32_SFLOAT, RDC::TEXTURE_TYPE_3D, SIDE, SIDE, SIDE, float_bytes(initial_fill));
	ParamWriter fill_params;
	fill_params.v4(ORIGIN, ORIGIN, ORIGIN, SPACING);
	fill_params.v4(0, 0, 0, EPSILON);
	fill_params.v4(HALF.x, HALF.y, HALF.z, 0);
	fill_params.u4(GRID.x, GRID.y, GRID.z, 2);
	fill_params.u4(COARSE.x, COARSE.y, COARSE.z, 1);
	fill_params.u4(ATLAS_GRID, PHYSICAL, 0, 0);
	RID fill_params_buffer = r.uniform_buffer(fill_params.bytes);
	RID fill_set = r.set(fill_shader, { { RDC::UNIFORM_TYPE_UNIFORM_BUFFER, fill_params_buffer }, { RDC::UNIFORM_TYPE_STORAGE_BUFFER, pages_buffer }, { RDC::UNIFORM_TYPE_IMAGE, atlas } });
	r.dispatch(fill_pipeline, fill_set, COARSE);
	out.atlas_initial = r.tex_data(atlas);

	// ---- Carve (no target, then with target protection) ----
	RID carve_pipeline;
	RID carve_shader = r.shader(p_shaders.path_join("carve.glsl"), carve_pipeline);
	if (!carve_shader.is_valid()) {
		return out;
	}
	Vector<uint8_t> page_image_zero;
	page_image_zero.resize(COARSE.x * COARSE.y * COARSE.z * 4);
	page_image_zero.fill(0);

	ParamWriter target_params;
	target_params.v4(ORIGIN, ORIGIN, ORIGIN, SPACING);
	target_params.u4(GRID.x - 1, GRID.y - 1, GRID.z - 1, 2);
	target_params.u4(COARSE.x, COARSE.y, COARSE.z, 1);
	target_params.u4(ATLAS_GRID, PHYSICAL, 0, 0);
	RID target_params_buffer = r.uniform_buffer(target_params.bytes);

	for (int variant = 0; variant < 2; variant++) {
		ParamWriter p;
		cutter_params(p);
		p.v4(0.3f, 0, 0, 0); // lathe0: top extension.
		p.v4(0, 0, 0, EPSILON); // lathe1: mode 0 (prism), empty threshold.
		p.v4(ORIGIN, ORIGIN, ORIGIN, SPACING);
		p.v4(GRID.x - 1, GRID.y - 1, GRID.z - 1, 2);
		p.v4(COARSE.x, COARSE.y, COARSE.z, 1);
		p.v4(PHYSICAL, ATLAS_GRID, variant == 1 ? 1.0f : 0.0f, 0);
		p.v4(0, 0, 0, 0); // dirty_min.
		p.v4(COARSE.x, COARSE.y, COARSE.z, 0); // dirty_size.

		RID live_atlas = r.texture(RDC::DATA_FORMAT_R32_SFLOAT, RDC::TEXTURE_TYPE_3D, SIDE, SIDE, SIDE, out.atlas_initial);
		RID live_pages = r.storage(pages_bytes);
		RID target_atlas = r.texture(RDC::DATA_FORMAT_R32_SFLOAT, RDC::TEXTURE_TYPE_3D, SIDE, SIDE, SIDE, out.atlas_initial);
		RID target_pages = r.storage(pages_bytes);
		RID page_image = r.texture(RDC::DATA_FORMAT_R8G8B8A8_UNORM, RDC::TEXTURE_TYPE_2D, COARSE.x, COARSE.y * COARSE.z, 1, page_image_zero);
		RID carve_params_buffer = r.uniform_buffer(p.bytes);
		RID carve_set = r.set(carve_shader, { { RDC::UNIFORM_TYPE_UNIFORM_BUFFER, carve_params_buffer }, { RDC::UNIFORM_TYPE_STORAGE_BUFFER, live_pages }, { RDC::UNIFORM_TYPE_IMAGE, live_atlas }, { RDC::UNIFORM_TYPE_IMAGE, page_image }, { RDC::UNIFORM_TYPE_STORAGE_BUFFER, target_pages }, { RDC::UNIFORM_TYPE_IMAGE, target_atlas }, { RDC::UNIFORM_TYPE_UNIFORM_BUFFER, target_params_buffer } });
		r.dispatch(carve_pipeline, carve_set, COARSE);
		if (variant == 0) {
			out.carve_atlas = r.tex_data(live_atlas);
			out.carve_pages = r.buf_data(live_pages);
			out.carve_page_image = r.tex_data(page_image);
		} else {
			out.carve_target_atlas = r.tex_data(live_atlas);
			out.carve_target_pages = r.buf_data(live_pages);
		}
	}

	// ---- Shaving: source = initial wood, live = carved wood ----
	RID shaving_pipeline;
	RID shaving_shader = r.shader(p_shaders.path_join("shaving_compute.glsl"), shaving_pipeline);
	if (!shaving_shader.is_valid()) {
		return out;
	}
	const int shaving_length = 14;
	const int shaving_width = 16;
	const int shaving_thickness = 12;
	{
		ParamWriter p;
		cutter_params(p);
		p.v4(0.3f, 0, 0, 0); // Top extension.
		p.v4(0, 0, 0, 0); // Threshold (unused).
		p.v4(ORIGIN, ORIGIN, ORIGIN, SPACING);
		p.v4(GRID.x - 1, GRID.y - 1, GRID.z - 1, 2);
		p.v4(COARSE.x, COARSE.y, COARSE.z, 1);
		p.v4(PHYSICAL, ATLAS_GRID, 0, 0);
		p.v4(0, 0, 0, 0);
		p.v4(COARSE.x, COARSE.y, COARSE.z, 0);
		p.v4(-0.6f, -0.5f, -0.4f, 0.1f); // Shaving origin, voxel.
		p.v4(1, 0, 0, 0.4f); // Axis T, width offset.
		p.v4(0, 1, 0, 0.3f); // Axis B, thickness offset.
		p.v4(0, 0, 1, 0.0001f); // Axis N, wood threshold.
		p.v4(0, shaving_width, shaving_thickness, 0); // Write range.

		RID source_atlas = r.texture(RDC::DATA_FORMAT_R32_SFLOAT, RDC::TEXTURE_TYPE_3D, SIDE, SIDE, SIDE, out.atlas_initial);
		RID source_pages = r.storage(pages_bytes);
		RID live_atlas = r.texture(RDC::DATA_FORMAT_R32_SFLOAT, RDC::TEXTURE_TYPE_3D, SIDE, SIDE, SIDE, out.carve_atlas);
		RID live_pages = r.storage(out.carve_pages);
		Vector<uint8_t> shaving_zero;
		shaving_zero.resize(shaving_length * shaving_width * shaving_thickness * 8);
		shaving_zero.fill(0);
		RID target = r.texture(RDC::DATA_FORMAT_R32G32_SFLOAT, RDC::TEXTURE_TYPE_3D, shaving_length, shaving_width, shaving_thickness, shaving_zero, true);
		ParamWriter result_init;
		result_init.u(0);
		result_init.u(0x7FFFFFFF);
		RID result = r.storage(result_init.bytes);
		RID params_buffer = r.uniform_buffer(p.bytes);
		RID set = r.set(shaving_shader, { { RDC::UNIFORM_TYPE_UNIFORM_BUFFER, params_buffer }, { RDC::UNIFORM_TYPE_STORAGE_BUFFER, source_pages }, { RDC::UNIFORM_TYPE_IMAGE, source_atlas }, { RDC::UNIFORM_TYPE_IMAGE, target }, { RDC::UNIFORM_TYPE_STORAGE_BUFFER, result }, { RDC::UNIFORM_TYPE_STORAGE_BUFFER, live_pages }, { RDC::UNIFORM_TYPE_IMAGE, live_atlas } });
		r.dispatch(shaving_pipeline, set, Vector3i(shaving_length, 1, 1));
		out.shaving_target = r.tex_data(target);
		out.shaving_result = r.buf_data(result);
	}

	// ---- Surface probe ----
	RID probe_pipeline;
	RID probe_shader = r.shader(p_shaders.path_join("surface_probe.glsl"), probe_pipeline);
	if (!probe_shader.is_valid()) {
		return out;
	}
	{
		ParamWriter sparse;
		sparse.v4(ORIGIN, ORIGIN, ORIGIN, SPACING);
		sparse.v4(COARSE.x, COARSE.y, COARSE.z, 2);
		sparse.v4(PHYSICAL, ATLAS_GRID, 1, 0);
		sparse.v4(-1.5f, -1.5f, -1.5f, 0.5f);
		sparse.v4(2.0f, 2.0f, 2.0f, 0.5f);
		sparse.v4(GRID.x - 1, GRID.y - 1, GRID.z - 1, 300);
		RID sparse_buffer = r.uniform_buffer(sparse.bytes);
		RID probe_atlas = r.texture(RDC::DATA_FORMAT_R32_SFLOAT, RDC::TEXTURE_TYPE_3D, SIDE, SIDE, SIDE, out.carve_atlas);
		RID probe_pages = r.storage(out.carve_pages);
		Vector<uint8_t> zero16;
		zero16.resize(16);
		zero16.fill(0);
		for (int mode = 0; mode < 2; mode++) {
			ParamWriter query;
			if (mode == 0) {
				query.v4(0.1f, 0.0f, 0.05f, 0.0f);
				query.v4(0, -1, 0, 0);
			} else {
				query.v4(-1.2f, 0.9f, 0.3f, 0.0f);
				query.v4(1.0f, -0.3f, -0.1f, 1.0f);
			}
			RID query_buffer = r.uniform_buffer(query.bytes);
			RID result = r.storage(zero16);
			RID set = r.set(probe_shader, { { RDC::UNIFORM_TYPE_UNIFORM_BUFFER, query_buffer }, { RDC::UNIFORM_TYPE_STORAGE_BUFFER, result }, { RDC::UNIFORM_TYPE_UNIFORM_BUFFER, sparse_buffer }, { RDC::UNIFORM_TYPE_STORAGE_BUFFER, probe_pages }, { RDC::UNIFORM_TYPE_IMAGE, probe_atlas } });
			r.dispatch(probe_pipeline, set, Vector3i(1, 1, 1));
			(mode == 0 ? out.probe_down : out.probe_ray) = r.buf_data(result);
		}
	}

	out.ok = true;
	return out;
}

struct FloatDiff {
	float max_error = 0.0f;
	int mismatches = 0;
	int count = 0;
};

FloatDiff compare_floats(const Vector<uint8_t> &p_a, const Vector<uint8_t> &p_b, float p_tolerance) {
	FloatDiff diff;
	const int count = MIN(p_a.size(), p_b.size()) / 4;
	diff.count = count;
	const float *a = (const float *)p_a.ptr();
	const float *b = (const float *)p_b.ptr();
	for (int i = 0; i < count; i++) {
		const float error = Math::abs(a[i] - b[i]);
		diff.max_error = MAX(diff.max_error, error);
		if (!(error <= p_tolerance)) {
			diff.mismatches++;
		}
	}
	return diff;
}

String describe(const String &p_name, const FloatDiff &p_diff) {
	return p_name + ": " + itos(p_diff.count) + " values, max error " + String::num(p_diff.max_error, 9) + ", " + itos(p_diff.mismatches) + " beyond tolerance";
}

int count_changed(const Vector<uint8_t> &p_a, const Vector<uint8_t> &p_b) {
	int changed = 0;
	const int count = MIN(p_a.size(), p_b.size()) / 4;
	for (int i = 0; i < count; i++) {
		changed += memcmp(p_a.ptr() + i * 4, p_b.ptr() + i * 4, 4) != 0;
	}
	return changed;
}

String floats_to_string(const Vector<uint8_t> &p_bytes) {
	String text;
	const float *values = (const float *)p_bytes.ptr();
	for (int i = 0; i < p_bytes.size() / 4; i++) {
		text += String::num(values[i], 5) + " ";
	}
	return text;
}

TEST_CASE("[WebGPU][Parity] WoodWorks sparse SDF pipeline matches the CPU reference and Vulkan") {
	String shaders_dir = OS::get_singleton()->get_environment("WOODWORKS_SHADERS_DIR");
	if (shaders_dir.is_empty()) {
		WARN("WOODWORKS_SHADERS_DIR is not set, skipping.");
		return;
	}

	RenderingContextDriverWebGPU webgpu_context;
	if (webgpu_context.initialize() != OK) {
		WARN("No WebGPU adapter available, skipping.");
		return;
	}
	RD *webgpu_rd = memnew(RD);
	REQUIRE(webgpu_rd->initialize(&webgpu_context) == OK);
	print_line("WebGPU device: " + webgpu_context.device_get(0).name);
	Outputs web = run_scenario(webgpu_rd, shaders_dir);
	memdelete(webgpu_rd);
	REQUIRE(web.ok);

	// Against the analytic reference (same checks as probe/initial.gd).
	{
		const uint32_t *states = (const uint32_t *)web.states.ptr();
		int wrong_states = 0;
		for (int i = 0; i < web.expected_states.size(); i++) {
			wrong_states += states[i] != web.expected_states[i];
		}
		CHECK_MESSAGE(wrong_states == 0, "classification states differ from the CPU reference: ", wrong_states);
		const float *atlas = (const float *)web.atlas_initial.ptr();
		float max_error = 0.0f;
		for (int i = 0; i < web.expected_atlas.size(); i++) {
			max_error = MAX(max_error, Math::abs(atlas[i] - web.expected_atlas[i]));
		}
		print_line("initial fill max error vs CPU: " + String::num(max_error, 9));
		CHECK(max_error < 0.000001f);
	}

#ifdef VULKAN_ENABLED
	{
		TestVulkanContext vulkan_context;
		if (vulkan_context.initialize() != OK) {
			WARN("Vulkan is unavailable, skipping the comparison.");
			return;
		}
		RD *vulkan_rd = memnew(RD);
		REQUIRE(vulkan_rd->initialize(&vulkan_context) == OK);
		print_line("Vulkan device: " + vulkan_context.device_get(0).name);
		Outputs vk = run_scenario(vulkan_rd, shaders_dir);
		memdelete(vulkan_rd);
		REQUIRE(vk.ok);

		const uint32_t *result_words = (const uint32_t *)web.shaving_result.ptr();
		print_line("carve changed texels (vs initial): " + itos(count_changed(web.atlas_initial, web.carve_atlas)) + ", with target: " + itos(count_changed(web.atlas_initial, web.carve_target_atlas)));
		print_line("carve pages changed: " + itos(count_changed(web.pages, web.carve_pages)));
		print_line("shaving wood_count=" + itos(result_words[0]) + " min_fixed=" + itos((int32_t)result_words[1]));
		print_line("probe down (web): " + floats_to_string(web.probe_down) + "| (vulkan): " + floats_to_string(vk.probe_down));
		print_line("probe ray (web): " + floats_to_string(web.probe_ray) + "| (vulkan): " + floats_to_string(vk.probe_ray));

		CHECK(web.states == vk.states);
		CHECK(web.atlas_initial == vk.atlas_initial);
		CHECK(web.carve_pages == vk.carve_pages);
		CHECK(web.carve_page_image == vk.carve_page_image);
		CHECK(web.carve_target_pages == vk.carve_target_pages);
		CHECK(web.shaving_result == vk.shaving_result);

		const FloatDiff carve = compare_floats(web.carve_atlas, vk.carve_atlas, 1e-5f);
		const FloatDiff carve_target = compare_floats(web.carve_target_atlas, vk.carve_target_atlas, 1e-5f);
		const FloatDiff shaving = compare_floats(web.shaving_target, vk.shaving_target, 1e-5f);
		const FloatDiff probe_down = compare_floats(web.probe_down, vk.probe_down, 1e-5f);
		const FloatDiff probe_ray = compare_floats(web.probe_ray, vk.probe_ray, 1e-5f);
		print_line(describe("carve atlas", carve));
		print_line(describe("carve atlas with target", carve_target));
		print_line(describe("shaving texture", shaving));
		print_line(describe("surface probe (down)", probe_down));
		print_line(describe("surface probe (ray)", probe_ray));
		CHECK(carve.mismatches == 0);
		CHECK(carve_target.mismatches == 0);
		CHECK(shaving.mismatches == 0);
		CHECK(probe_down.mismatches == 0);
		CHECK(probe_ray.mismatches == 0);
	}
#endif
}

// ---- Reference outputs, so the browser can compare against a desktop run ----

Vector<Vector<uint8_t> *> output_fields(Outputs &p_outputs) {
	Vector<Vector<uint8_t> *> fields;
	fields.push_back(&p_outputs.states);
	fields.push_back(&p_outputs.pages);
	fields.push_back(&p_outputs.atlas_initial);
	fields.push_back(&p_outputs.carve_atlas);
	fields.push_back(&p_outputs.carve_pages);
	fields.push_back(&p_outputs.carve_page_image);
	fields.push_back(&p_outputs.carve_target_atlas);
	fields.push_back(&p_outputs.carve_target_pages);
	fields.push_back(&p_outputs.shaving_target);
	fields.push_back(&p_outputs.shaving_result);
	fields.push_back(&p_outputs.probe_down);
	fields.push_back(&p_outputs.probe_ray);
	return fields;
}

const char *OUTPUT_NAMES[] = { "states", "pages", "atlas_initial", "carve_atlas", "carve_pages", "carve_page_image", "carve_target_atlas", "carve_target_pages", "shaving_target", "shaving_result", "probe_down", "probe_ray" };

bool save_outputs(const String &p_path, Outputs &p_outputs) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return false;
	}
	for (Vector<uint8_t> *field : output_fields(p_outputs)) {
		file->store_32(field->size());
		file->store_buffer(field->ptr(), field->size());
	}
	return true;
}

bool load_outputs(const String &p_path, Outputs &r_outputs) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		return false;
	}
	for (Vector<uint8_t> *field : output_fields(r_outputs)) {
		const uint32_t size = file->get_32();
		field->resize(size);
		if (file->get_buffer(field->ptrw(), size) != size) {
			return false;
		}
	}
	return true;
}

String baked_directory() {
#ifdef WEB_ENABLED
	return "/webgpu";
#else
	return OS::get_singleton()->get_environment("WEBGPU_BAKED_DIR");
#endif
}

// Desktop only: bakes the shaders into WebGPU containers (WGSL when GODOT_TINT_PATH points to Tint) and stores the Vulkan
// results as reference data in WEBGPU_BAKE_DIR. The browser test then replays the baked shaders against that reference.
TEST_CASE("[WebGPU][Bake] Bake shader containers and Vulkan reference outputs") {
	const String bake_dir = OS::get_singleton()->get_environment("WEBGPU_BAKE_DIR");
	const String shaders_dir = OS::get_singleton()->get_environment("WOODWORKS_SHADERS_DIR");
	if (bake_dir.is_empty() || shaders_dir.is_empty()) {
		WARN("WEBGPU_BAKE_DIR or WOODWORKS_SHADERS_DIR is not set, skipping.");
		return;
	}
#ifdef VULKAN_ENABLED
	RenderingContextDriverWebGPU webgpu_context;
	REQUIRE(webgpu_context.initialize() == OK);
	RD *webgpu_rd = memnew(RD);
	REQUIRE(webgpu_rd->initialize(&webgpu_context) == OK);
	Outputs baked_run = run_scenario(webgpu_rd, shaders_dir, String(), bake_dir);
	memdelete(webgpu_rd);
	REQUIRE(baked_run.ok);

	TestVulkanContext vulkan_context;
	REQUIRE(vulkan_context.initialize() == OK);
	RD *vulkan_rd = memnew(RD);
	REQUIRE(vulkan_rd->initialize(&vulkan_context) == OK);
	Outputs reference = run_scenario(vulkan_rd, shaders_dir);
	memdelete(vulkan_rd);
	REQUIRE(reference.ok);
	CHECK(save_outputs(bake_dir.path_join("reference.bin"), reference));
	print_line("Baked containers and reference outputs to " + bake_dir);
#else
	WARN("Vulkan is required to produce the reference outputs, skipping.");
#endif
}

// Runs the baked shaders on WebGPU (natively, or in the browser where the files are preloaded under /webgpu) and compares
// every output with the Vulkan reference produced by the bake test.
TEST_CASE("[WebGPU][Browser] Baked shaders reproduce the Vulkan reference outputs") {
	const String baked = baked_directory();
	if (baked.is_empty()) {
		WARN("WEBGPU_BAKED_DIR is not set, skipping.");
		return;
	}
	Outputs reference;
	REQUIRE_MESSAGE(load_outputs(baked.path_join("reference.bin"), reference), "Cannot read reference.bin");

	RenderingContextDriverWebGPU context;
	REQUIRE_MESSAGE(context.initialize() == OK, "No WebGPU adapter.");
	RD *rd = memnew(RD);
	REQUIRE(rd->initialize(&context) == OK);
	print_line("WebGPU device: " + context.device_get(0).name);
	Outputs result = run_scenario(rd, "shaders", baked);
	memdelete(rd);
	REQUIRE(result.ok);

	// Integer data must match exactly; float data within 1e-5.
	const bool exact[] = { true, true, false, false, true, true, false, true, false, true, false, false };
	Vector<Vector<uint8_t> *> got = output_fields(result);
	Vector<Vector<uint8_t> *> want = output_fields(reference);
	int failures = 0;
	for (int i = 0; i < got.size(); i++) {
		bool ok;
		String detail;
		if (exact[i]) {
			ok = *got[i] == *want[i];
			detail = "exact";
		} else {
			const FloatDiff diff = compare_floats(*got[i], *want[i], 1e-5f);
			ok = diff.mismatches == 0 && got[i]->size() == want[i]->size();
			detail = describe("float", diff);
		}
		print_line(vformat("%s: %s (%s)", OUTPUT_NAMES[i], ok ? "OK" : "MISMATCH", detail));
		failures += !ok;
	}
	CHECK_MESSAGE(failures == 0, "outputs differ from the Vulkan reference");
}

} // namespace TestWebGPUParity

#endif // WEBGPU_ENABLED
