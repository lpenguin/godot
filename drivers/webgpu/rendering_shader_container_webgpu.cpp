/**************************************************************************/
/*  rendering_shader_container_webgpu.cpp                                 */
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

#ifdef WEBGPU_ENABLED

#include "rendering_shader_container_webgpu.h"

#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"
#include "thirdparty/spirv-reflect/spirv_reflect.h"

const uint32_t RenderingShaderContainerWebGPU::FORMAT_VERSION = 1;

namespace {

// WGSL has no write-only storage buffers: storage buffers are either read-only or read-write.
// Strips the NonReadable decoration (GLSL `writeonly`) from buffers so that naga/Tint accept the module.
// Write-only storage textures are valid in WGSL, so NonReadable stays on images.
Vector<uint8_t> _strip_buffer_non_readable(const Vector<uint8_t> &p_spirv) {
	constexpr uint32_t OP_VARIABLE = 59;
	constexpr uint32_t OP_DECORATE = 71;
	constexpr uint32_t OP_MEMBER_DECORATE = 72;
	constexpr uint32_t DECORATION_NON_READABLE = 25;
	constexpr uint32_t STORAGE_CLASS_UNIFORM = 2;
	constexpr uint32_t STORAGE_CLASS_STORAGE_BUFFER = 12;

	const uint32_t word_count = p_spirv.size() / 4;
	const uint32_t *words = (const uint32_t *)p_spirv.ptr();
	if (word_count < 5) {
		return p_spirv;
	}

	// Pass 1: find which ids are buffer variables.
	HashSet<uint32_t> buffer_variables;
	for (uint32_t i = 5; i < word_count;) {
		const uint32_t length = words[i] >> 16;
		const uint32_t opcode = words[i] & 0xFFFF;
		if (length == 0 || i + length > word_count) {
			return p_spirv;
		}
		if (opcode == OP_VARIABLE && length >= 4) {
			const uint32_t storage_class = words[i + 3];
			if (storage_class == STORAGE_CLASS_STORAGE_BUFFER || storage_class == STORAGE_CLASS_UNIFORM) {
				buffer_variables.insert(words[i + 2]);
			}
		}
		i += length;
	}

	// Pass 2: copy everything except the offending decorations.
	Vector<uint8_t> result;
	result.resize(p_spirv.size());
	uint32_t *out = (uint32_t *)result.ptrw();
	memcpy(out, words, 5 * sizeof(uint32_t));
	uint32_t out_count = 5;
	for (uint32_t i = 5; i < word_count;) {
		const uint32_t length = words[i] >> 16;
		const uint32_t opcode = words[i] & 0xFFFF;
		bool skip = false;
		if (opcode == OP_MEMBER_DECORATE && length >= 4 && words[i + 3] == DECORATION_NON_READABLE) {
			skip = true;
		} else if (opcode == OP_DECORATE && length >= 3 && words[i + 2] == DECORATION_NON_READABLE && buffer_variables.has(words[i + 1])) {
			skip = true;
		}
		if (!skip) {
			memcpy(out + out_count, words + i, length * sizeof(uint32_t));
			out_count += length;
		}
		i += length;
	}
	result.resize(out_count * sizeof(uint32_t));
	return result;
}

#ifndef WEB_ENABLED
// Converts SPIR-V to WGSL with the Tint command line tool (GODOT_TINT_PATH). Browsers only accept WGSL, so the
// conversion happens when the container is baked, never in the player.
bool _spirv_to_wgsl(const String &p_tint, const Vector<uint8_t> &p_spirv, const String &p_name, String &r_wgsl) {
	const String directory = OS::get_singleton()->get_temp_path();
	const String stem = directory.path_join("godot_webgpu_" + itos(OS::get_singleton()->get_process_id()) + "_" + p_name.get_file().get_basename().validate_filename());
	const String input = stem + ".spv";
	const String output = stem + ".wgsl";
	{
		Ref<FileAccess> file = FileAccess::open(input, FileAccess::WRITE);
		ERR_FAIL_COND_V_MSG(file.is_null(), false, "WebGPU: cannot write " + input);
		file->store_buffer(p_spirv.ptr(), p_spirv.size());
	}
	List<String> arguments;
	arguments.push_back("--format");
	arguments.push_back("wgsl");
	arguments.push_back("-o");
	arguments.push_back(output);
	arguments.push_back(input);
	String log;
	int exit_code = -1;
	const Error err = OS::get_singleton()->execute(p_tint, arguments, &log, &exit_code, true);
	if (err != OK || exit_code != 0) {
		ERR_PRINT(vformat("WebGPU: Tint failed for '%s' (exit code %d): %s", p_name, exit_code, log));
		return false;
	}
	r_wgsl = FileAccess::get_file_as_string(output);
	return !r_wgsl.is_empty();
}
#endif

} // namespace

RenderingShaderContainerWebGPU::RenderingShaderContainerWebGPU() {
}

uint32_t RenderingShaderContainerWebGPU::_format() const {
	return 0x55504757; // "WGPU".
}

uint32_t RenderingShaderContainerWebGPU::_format_version() const {
	return FORMAT_VERSION;
}

bool RenderingShaderContainerWebGPU::_set_code_from_spirv(const ReflectShader &p_shader) {
	const LocalVector<ReflectShaderStage> &stages = p_shader.shader_stages;

	// Native builds can hand SPIR-V straight to wgpu. Browsers need WGSL, which is produced here, at bake time, when
	// the GODOT_TINT_PATH environment variable points to a Tint executable.
	String tint;
#ifndef WEB_ENABLED
	tint = OS::get_singleton()->get_environment("GODOT_TINT_PATH");
#endif
	shaders.resize(stages.size());
	for (uint32_t i = 0; i < stages.size(); i++) {
		RenderingShaderContainer::Shader &shader = shaders.ptrw()[i];
		shader.shader_stage = stages[i].shader_stage;
		const Vector<uint8_t> spirv = _strip_buffer_non_readable(stages[i].spirv_data());
		shader.code_compression_flags = 0;
		shader.code_decompressed_size = 0;
#ifndef WEB_ENABLED
		if (!tint.is_empty()) {
			String wgsl;
			ERR_FAIL_COND_V(!_spirv_to_wgsl(tint, spirv, String::utf8(shader_name.get_data()), wgsl), false);
			const CharString utf8 = wgsl.utf8();
			shader.code_compressed_bytes.resize(utf8.length());
			memcpy(shader.code_compressed_bytes.ptrw(), utf8.get_data(), utf8.length());
			shader.code_compression_flags = COMPRESSION_FLAG_WGSL;
			continue;
		}
#endif
		shader.code_compressed_bytes = spirv;
	}
	return true;
}

void RenderingShaderContainerWebGPU::_set_from_shader_reflection_post(const ReflectShader &p_shader) {
	binding_extras.clear();
	for (const ReflectDescriptorSet &uniform_set : p_shader.uniform_sets) {
		for (const ReflectUniform &uniform : uniform_set) {
			BindingExtra extra;
			const SpvReflectDescriptorBinding &spv = uniform.get_spv_reflect();
			extra.image_format = uint32_t(spv.image.image_format);
			extra.dim = uint32_t(spv.image.dim);
			extra.arrayed = spv.image.arrayed;
			extra.multisampled = spv.image.ms;
			extra.depth = spv.image.depth;
			extra.readable = (spv.decoration_flags & SPV_REFLECT_DECORATION_NON_READABLE) ? 0 : 1;
			extra.writable = (spv.decoration_flags & SPV_REFLECT_DECORATION_NON_WRITABLE) ? 0 : 1;
			binding_extras.push_back(extra);
		}
	}
}

uint32_t RenderingShaderContainerWebGPU::_from_bytes_reflection_binding_uniform_extra_data_start(const uint8_t *p_bytes) {
	return 0;
}

uint32_t RenderingShaderContainerWebGPU::_from_bytes_reflection_binding_uniform_extra_data(const uint8_t *p_bytes, uint32_t p_index) {
	if (p_index >= (uint32_t)binding_extras.size()) {
		binding_extras.resize(p_index + 1);
	}
	memcpy(&binding_extras.ptrw()[p_index], p_bytes, sizeof(BindingExtra));
	return sizeof(BindingExtra);
}

uint32_t RenderingShaderContainerWebGPU::_to_bytes_reflection_binding_uniform_extra_data(uint8_t *p_bytes, uint32_t p_index) const {
	if (p_bytes) {
		memcpy(p_bytes, &binding_extras[p_index], sizeof(BindingExtra));
	}
	return sizeof(BindingExtra);
}

// RenderingShaderContainerFormatWebGPU

Ref<RenderingShaderContainer> RenderingShaderContainerFormatWebGPU::create_container() const {
	return memnew(RenderingShaderContainerWebGPU);
}

RenderingDeviceCommons::ShaderLanguageVersion RenderingShaderContainerFormatWebGPU::get_shader_language_version() const {
	return SHADER_LANGUAGE_VULKAN_VERSION_1_1;
}

RenderingDeviceCommons::ShaderSpirvVersion RenderingShaderContainerFormatWebGPU::get_shader_spirv_version() const {
	return SHADER_SPIRV_VERSION_1_3;
}

#endif // WEBGPU_ENABLED
