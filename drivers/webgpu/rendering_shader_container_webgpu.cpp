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


// Vulkan clip space has Y pointing down, WebGPU (like D3D12 and Metal) has it pointing up. Godot's shaders follow the
// Vulkan convention, so, like the D3D12 driver does, negate gl_Position.y before every return of the vertex entry point.
// This is done on the SPIR-V so that it happens at bake time, before any conversion to WGSL.
Vector<uint8_t> _flip_vertex_y(const Vector<uint8_t> &p_spirv) {
	constexpr uint32_t OP_ENTRY_POINT = 15;
	constexpr uint32_t OP_TYPE_INT = 21;
	constexpr uint32_t OP_TYPE_FLOAT = 22;
	constexpr uint32_t OP_TYPE_STRUCT = 30;
	constexpr uint32_t OP_TYPE_POINTER = 32;
	constexpr uint32_t OP_CONSTANT = 43;
	constexpr uint32_t OP_FUNCTION = 54;
	constexpr uint32_t OP_FUNCTION_END = 56;
	constexpr uint32_t OP_VARIABLE = 59;
	constexpr uint32_t OP_LOAD = 61;
	constexpr uint32_t OP_STORE = 62;
	constexpr uint32_t OP_ACCESS_CHAIN = 65;
	constexpr uint32_t OP_MEMBER_DECORATE = 72;
	constexpr uint32_t OP_F_NEGATE = 127;
	constexpr uint32_t OP_RETURN = 253;
	constexpr uint32_t EXECUTION_MODEL_VERTEX = 0;
	constexpr uint32_t DECORATION_BUILTIN = 11;
	constexpr uint32_t BUILTIN_POSITION = 0;
	constexpr uint32_t STORAGE_CLASS_OUTPUT = 3;

	const uint32_t word_count = p_spirv.size() / 4;
	const uint32_t *words = (const uint32_t *)p_spirv.ptr();
	if (word_count < 5) {
		return p_spirv;
	}

	uint32_t entry_function = 0;
	uint32_t position_struct = 0;
	uint32_t position_member = 0;
	uint32_t float_type = 0;
	uint32_t int_type = 0;
	HashMap<uint32_t, uint32_t> pointer_pointee; // Output pointer type id -> pointee id.
	HashMap<uint32_t, uint32_t> pointer_types; // Output pointer type id.
	uint32_t position_variable = 0;
	uint32_t position_variable_type = 0;
	HashMap<uint32_t, uint32_t> int_constants; // value -> id (for int_type).
	uint32_t first_function = 0; // Word index.

	for (uint32_t i = 5; i < word_count;) {
		const uint32_t length = words[i] >> 16;
		const uint32_t opcode = words[i] & 0xFFFF;
		if (length == 0 || i + length > word_count) {
			return p_spirv;
		}
		switch (opcode) {
			case OP_ENTRY_POINT:
				if (words[i + 1] == EXECUTION_MODEL_VERTEX && entry_function == 0) {
					entry_function = words[i + 2];
				}
				break;
			case OP_MEMBER_DECORATE:
				if (length >= 5 && words[i + 3] == DECORATION_BUILTIN && words[i + 4] == BUILTIN_POSITION) {
					position_struct = words[i + 1];
					position_member = words[i + 2];
				}
				break;
			case OP_TYPE_FLOAT:
				if (words[i + 2] == 32 && float_type == 0) {
					float_type = words[i + 1];
				}
				break;
			case OP_TYPE_INT:
				if (words[i + 2] == 32 && int_type == 0) {
					int_type = words[i + 1];
				}
				break;
			case OP_TYPE_POINTER:
				if (words[i + 2] == STORAGE_CLASS_OUTPUT) {
					pointer_pointee[words[i + 1]] = words[i + 3];
				}
				break;
			case OP_CONSTANT:
				if (words[i + 1] == int_type && int_type != 0) {
					int_constants[words[i + 3]] = words[i + 2];
				}
				break;
			case OP_VARIABLE:
				if (words[i + 3] == STORAGE_CLASS_OUTPUT && position_struct != 0 && pointer_pointee.has(words[i + 1]) && pointer_pointee[words[i + 1]] == position_struct) {
					position_variable = words[i + 2];
					position_variable_type = words[i + 1];
				}
				break;
			case OP_FUNCTION:
				if (first_function == 0) {
					first_function = i;
				}
				break;
			default:
				break;
		}
		i += length;
	}
	if (entry_function == 0 || position_variable == 0 || float_type == 0 || first_function == 0) {
		return p_spirv; // Not a vertex shader writing gl_Position through gl_PerVertex.
	}

	uint32_t bound = words[3];
	const uint32_t new_int_type_id = int_type == 0 ? bound++ : 0;
	if (int_type == 0) {
		int_type = new_int_type_id;
	}
	const uint32_t member_const = int_constants.has(position_member) ? int_constants[position_member] : bound++;
	const bool member_const_new = !int_constants.has(position_member);
	const uint32_t y_const = int_constants.has(1) ? int_constants[1] : bound++;
	const bool y_const_new = !int_constants.has(1);

	uint32_t float_output_pointer = 0;
	for (const KeyValue<uint32_t, uint32_t> &pointer : pointer_pointee) {
		if (pointer.value == float_type) {
			float_output_pointer = pointer.key;
			break;
		}
	}
	const bool pointer_new = float_output_pointer == 0;
	if (pointer_new) {
		float_output_pointer = bound++;
	}

	// New global declarations go right before the first function.
	Vector<uint32_t> declarations;
	if (new_int_type_id != 0) {
		declarations.push_back((4u << 16) | OP_TYPE_INT);
		declarations.push_back(new_int_type_id);
		declarations.push_back(32);
		declarations.push_back(1);
	}
	if (member_const_new) {
		declarations.push_back((4u << 16) | OP_CONSTANT);
		declarations.push_back(int_type);
		declarations.push_back(member_const);
		declarations.push_back(position_member);
	}
	if (y_const_new) {
		declarations.push_back((4u << 16) | OP_CONSTANT);
		declarations.push_back(int_type);
		declarations.push_back(y_const);
		declarations.push_back(1);
	}
	if (pointer_new) {
		declarations.push_back((4u << 16) | OP_TYPE_POINTER);
		declarations.push_back(float_output_pointer);
		declarations.push_back(STORAGE_CLASS_OUTPUT);
		declarations.push_back(float_type);
	}

	// Copy the module, inserting the flip before each OpReturn of the entry function.
	Vector<uint32_t> out;
	out.resize(0);
	bool in_entry = false;
	int flips = 0;
	for (uint32_t i = 0; i < word_count;) {
		uint32_t length = 1;
		uint32_t opcode = 0;
		if (i >= 5) {
			length = words[i] >> 16;
			opcode = words[i] & 0xFFFF;
		} else {
			length = 5 - i;
		}
		if (i == first_function) {
			for (uint32_t word : declarations) {
				out.push_back(word);
			}
		}
		if (i >= 5) {
			if (opcode == OP_FUNCTION) {
				in_entry = words[i + 2] == entry_function;
			} else if (opcode == OP_FUNCTION_END) {
				in_entry = false;
			} else if (opcode == OP_RETURN && in_entry) {
				const uint32_t pointer = bound++;
				const uint32_t value = bound++;
				const uint32_t negated = bound++;
				// %pointer = OpAccessChain %float_output_pointer %position_variable %member %y
				out.push_back((6u << 16) | OP_ACCESS_CHAIN);
				out.push_back(float_output_pointer);
				out.push_back(pointer);
				out.push_back(position_variable);
				out.push_back(member_const);
				out.push_back(y_const);
				// %value = OpLoad %float %pointer
				out.push_back((4u << 16) | OP_LOAD);
				out.push_back(float_type);
				out.push_back(value);
				out.push_back(pointer);
				// %negated = OpFNegate %float %value
				out.push_back((4u << 16) | OP_F_NEGATE);
				out.push_back(float_type);
				out.push_back(negated);
				out.push_back(value);
				// OpStore %pointer %negated
				out.push_back((3u << 16) | OP_STORE);
				out.push_back(pointer);
				out.push_back(negated);
				flips++;
			}
		}
		for (uint32_t k = 0; k < length; k++) {
			out.push_back(words[i + k]);
		}
		i += length;
	}
	if (flips == 0) {
		return p_spirv;
	}
	out.write[3] = bound;

	Vector<uint8_t> result;
	result.resize(out.size() * sizeof(uint32_t));
	memcpy(result.ptrw(), out.ptr(), result.size());
	(void)position_variable_type;
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
	// glslang describes a shadow map as a non-depth texture sampled through a depth sampled-image type. Tint converts it
	// itself, but its IR validator rejects the input first. The validation runs on the SPIR-V we feed it, not on the WGSL
	// it writes, and the browser validates that WGSL again.
	arguments.push_back("--disable-ir-validation-asserts");
	arguments.push_back("true");
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
	// No browser supports `var<immediate>` yet: push constants become a uniform buffer in group 0 (see the driver).
	r_wgsl = r_wgsl.replace("var<immediate> ", vformat("@group(0) @binding(%d) var<uniform> ", RenderingShaderContainerWebGPU::PUSH_CONSTANT_BINDING));
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
		Vector<uint8_t> spirv = _strip_buffer_non_readable(stages[i].spirv_data());
		if (stages[i].shader_stage == RDC::SHADER_STAGE_VERTEX) {
			spirv = _flip_vertex_y(spirv);
		}
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
