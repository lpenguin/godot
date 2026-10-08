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

const uint32_t RenderingShaderContainerWebGPU::FORMAT_VERSION = 8;

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


// Tint does not read some instructions that WGSL cannot express:
// * OpMemoryBarrier (GLSL memoryBarrierShared(), groupMemoryBarrier()). Godot always pairs it with barrier(), and the
//   OpControlBarrier that follows already carries the workgroup memory semantics, which becomes workgroupBarrier().
// * OpIsNan and OpIsInf: WGSL has no NaN or infinity. They become a comparison that is always false (same result type).
// SPIR-V 1.4 modules (compiled by drivers that target Vulkan 1.2+) are rewritten to 1.3, which is what Tint validates against.
// Two differences matter: 1.4 entry points list every global variable in their interface, and 1.4 allows OpSelect to pick
// whole vectors with a scalar condition.
Vector<uint8_t> _downgrade_spirv_version(const Vector<uint8_t> &p_spirv) {
	constexpr uint32_t OP_ENTRY_POINT = 15;
	constexpr uint32_t OP_TYPE_BOOL = 20;
	constexpr uint32_t OP_TYPE_VECTOR = 23;
	constexpr uint32_t OP_FUNCTION = 54;
	constexpr uint32_t OP_VARIABLE = 59;
	constexpr uint32_t OP_COMPOSITE_CONSTRUCT = 80;
	constexpr uint32_t OP_SELECT = 169;
	constexpr uint32_t SPIRV_1_3 = 0x00010300;
	const uint32_t word_count = p_spirv.size() / 4;
	const uint32_t *words = (const uint32_t *)p_spirv.ptr();
	if (word_count < 5 || words[1] <= SPIRV_1_3) {
		return p_spirv;
	}
	HashMap<uint32_t, uint32_t> storage_class;
	HashMap<uint32_t, uint32_t> value_type; // For the instructions that can produce a select condition.
	HashMap<uint32_t, uint32_t> vector_size; // Vector type id -> component count.
	HashMap<uint32_t, uint32_t> bool_vector; // Component count -> bool vector type id.
	uint32_t bool_type = 0;
	for (uint32_t i = 5; i < word_count;) {
		const uint32_t length = words[i] >> 16;
		const uint32_t opcode = words[i] & 0xFFFF;
		if (length == 0 || i + length > word_count) {
			return p_spirv;
		}
		if (opcode == OP_VARIABLE && length >= 4) {
			storage_class[words[i + 2]] = words[i + 3];
		} else if (opcode == OP_TYPE_BOOL) {
			bool_type = words[i + 1];
		} else if (opcode == OP_TYPE_VECTOR && length >= 4) {
			vector_size[words[i + 1]] = words[i + 3];
			if (words[i + 2] == bool_type) {
				bool_vector[words[i + 3]] = words[i + 1];
			}
		} else if (length >= 3 && (opcode == 1 || opcode == 12 || opcode == 41 || opcode == 42 || opcode == 48 || opcode == 49 || opcode == 55 || opcode == 57 || opcode == 61 || opcode == 81 || (opcode >= 154 && opcode <= 190) || opcode == 245)) {
			value_type[words[i + 2]] = words[i + 1];
		}
		i += length;
	}

	// Bool vector types needed by selects with a scalar condition.
	uint32_t bound = words[3];
	LocalVector<uint32_t> new_types; // Words of OpTypeVector declarations to emit before the first function.
	uint32_t select_count = 0;
	for (uint32_t i = 5; i < word_count;) {
		const uint32_t length = words[i] >> 16;
		if ((words[i] & 0xFFFF) == OP_SELECT && length == 6) {
			const uint32_t *size = vector_size.getptr(words[i + 1]);
			const uint32_t *cond_type = value_type.getptr(words[i + 3]);
			if (size && cond_type && *cond_type == bool_type) {
				select_count++;
				if (!bool_vector.has(*size)) {
					bool_vector[*size] = bound++;
					new_types.push_back((4u << 16) | OP_TYPE_VECTOR);
					new_types.push_back(bool_vector[*size]);
					new_types.push_back(bool_type);
					new_types.push_back(*size);
				}
			}
		}
		i += length;
	}

	Vector<uint8_t> result;
	result.resize(p_spirv.size() + new_types.size() * sizeof(uint32_t) + select_count * 7 * sizeof(uint32_t) + 64);
	uint32_t *out = (uint32_t *)result.ptrw();
	memcpy(out, words, 5 * sizeof(uint32_t));
	out[1] = SPIRV_1_3;
	uint32_t out_count = 5;
	bool types_emitted = false;
	for (uint32_t i = 5; i < word_count;) {
		const uint32_t length = words[i] >> 16;
		const uint32_t opcode = words[i] & 0xFFFF;
		if (opcode == OP_FUNCTION && !types_emitted) {
			types_emitted = true;
			for (uint32_t w : new_types) {
				out[out_count++] = w;
			}
		}
		if (opcode == OP_ENTRY_POINT) {
			// Header: execution model, function id, name (nul-terminated string words), then interface ids.
			uint32_t name_end = i + 3;
			while (name_end < i + length) {
				const uint32_t w = words[name_end++];
				if (((w >> 24) & 0xFF) == 0 || ((w >> 16) & 0xFF) == 0 || ((w >> 8) & 0xFF) == 0 || (w & 0xFF) == 0) {
					break;
				}
			}
			const uint32_t start = out_count;
			out_count++; // Length is patched below.
			for (uint32_t j = i + 1; j < name_end; j++) {
				out[out_count++] = words[j];
			}
			for (uint32_t j = name_end; j < i + length; j++) {
				const uint32_t *sc = storage_class.getptr(words[j]);
				if (!sc || *sc == 1 || *sc == 3) { // Input and Output only.
					out[out_count++] = words[j];
				}
			}
			out[start] = ((out_count - start) << 16) | OP_ENTRY_POINT;
		} else if (opcode == OP_SELECT && length == 6 && vector_size.has(words[i + 1]) && value_type.has(words[i + 3]) && value_type[words[i + 3]] == bool_type) {
			const uint32_t size = vector_size[words[i + 1]];
			const uint32_t condition = bound++;
			out[out_count++] = ((3u + size) << 16) | OP_COMPOSITE_CONSTRUCT;
			out[out_count++] = bool_vector[size];
			out[out_count++] = condition;
			for (uint32_t c = 0; c < size; c++) {
				out[out_count++] = words[i + 3];
			}
			out[out_count++] = words[i];
			out[out_count++] = words[i + 1];
			out[out_count++] = words[i + 2];
			out[out_count++] = condition;
			out[out_count++] = words[i + 4];
			out[out_count++] = words[i + 5];
		} else {
			memcpy(out + out_count, words + i, length * sizeof(uint32_t));
			out_count += length;
		}
		i += length;
	}
	out[3] = bound;
	result.resize(out_count * sizeof(uint32_t));
	return result;
}

Vector<uint8_t> _lower_unsupported_instructions(const Vector<uint8_t> &p_spirv) {
	constexpr uint32_t OP_IS_NAN = 156;
	constexpr uint32_t OP_IS_INF = 157;
	constexpr uint32_t OP_F_ORD_LESS_THAN = 184;
	constexpr uint32_t OP_MEMORY_BARRIER = 225;

	const uint32_t word_count = p_spirv.size() / 4;
	const uint32_t *words = (const uint32_t *)p_spirv.ptr();
	if (word_count < 5) {
		return p_spirv;
	}
	Vector<uint8_t> result;
	result.resize(p_spirv.size() + p_spirv.size() / 8 + 64); // Room for the comparisons, which are one word longer.
	uint32_t *out = (uint32_t *)result.ptrw();
	memcpy(out, words, 5 * sizeof(uint32_t));
	uint32_t out_count = 5;
	bool changed = false;
	for (uint32_t i = 5; i < word_count;) {
		const uint32_t length = words[i] >> 16;
		const uint32_t opcode = words[i] & 0xFFFF;
		if (length == 0 || i + length > word_count) {
			return p_spirv;
		}
		if (opcode == OP_MEMORY_BARRIER) {
			changed = true;
		} else if ((opcode == OP_IS_NAN || opcode == OP_IS_INF) && length == 4) {
			ERR_FAIL_COND_V((out_count + 5) * sizeof(uint32_t) > (uint32_t)result.size(), p_spirv);
			out[out_count++] = (5u << 16) | OP_F_ORD_LESS_THAN;
			out[out_count++] = words[i + 1]; // Result type.
			out[out_count++] = words[i + 2]; // Result id.
			out[out_count++] = words[i + 3];
			out[out_count++] = words[i + 3];
			changed = true;
		} else {
			ERR_FAIL_COND_V((out_count + length) * sizeof(uint32_t) > (uint32_t)result.size(), p_spirv);
			memcpy(out + out_count, words + i, length * sizeof(uint32_t));
			out_count += length;
		}
		i += length;
	}
	if (!changed) {
		return p_spirv;
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
	// Godot's shaders sample textures with implicit derivatives from non-uniform control flow, like GLSL allows. This adds
	// `diagnostic(off, derivative_uniformity)` to the WGSL, which browsers accept.
	arguments.push_back("--allow-non-uniform-derivatives");
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
	// Narrow 8-bit storage textures are allocated as RGBA8 (see the driver); textureStore always takes a vec4.
	for (const char *dimension : { "2d", "2d_array", "3d" }) {
		for (const char *access : { "write", "read", "read_write" }) {
			for (const char *narrow : { "r8unorm", "rg8unorm" }) {
				r_wgsl = r_wgsl.replace(vformat("texture_storage_%s<%s, %s>", dimension, narrow, access), vformat("texture_storage_%s<rgba8unorm, %s>", dimension, access));
			}
		}
	}
	r_wgsl = r_wgsl.replace("var<immediate> ", vformat("@group(0) @binding(%d) var<uniform> ", RenderingShaderContainerWebGPU::PUSH_CONSTANT_BINDING));
	return !r_wgsl.is_empty();
}
#endif

#ifndef WEB_ENABLED
// Reads the global declarations of the WGSL that Tint wrote: `@group(0u) @binding(2u) var name : sampler_comparison;`.
// Tint turns textures that are sampled with a comparison sampler into depth textures, which the SPIR-V reflection does
// not see, so the bind group layouts take these facts from the WGSL.
// Reads the unsigned number that starts at p_from ("0u)" -> 0, "505u)" -> 505).
uint32_t _read_uint(const String &p_text, int p_from) {
	uint32_t value = 0;
	for (int i = p_from; i < p_text.length() && p_text[i] >= '0' && p_text[i] <= '9'; i++) {
		value = value * 10 + (p_text[i] - '0');
	}
	return value;
}

// A resource of the shader as the reflection (and with it the uniform sets of the engine) knows it.
struct NamedBinding {
	String name;
	uint32_t set = 0;
	uint32_t binding = 0;
	bool combined = false; // A sampler2D: Tint splits it into `name_image` and `name_sampler`.
};

// Tint renumbers bindings: it splits combined samplers and then resolves binding conflicts, which can shift unrelated
// resources. The engine creates its bind groups from the reflected bindings, so every global goes back to those
// bindings; the sampler half of a combined binding goes to binding + COMBINED_SAMPLER_BINDING_OFFSET.
void _annotate_from_wgsl(String &r_wgsl, const Vector<Vector3i> &p_uniform_keys, const Vector<NamedBinding> &p_named, Vector<RenderingShaderContainerWebGPU::BindingExtra> &r_extras) {
	Vector<String> lines = r_wgsl.split("\n");
	for (int i = 0; i < lines.size(); i++) {
		String &line = lines.write[i];
		if (!line.begins_with("@group(")) {
			continue;
		}
		const int var_at = line.find(" var");
		const int colon_at = line.find(" : ");
		if (var_at < 0 || colon_at < 0) {
			continue;
		}
		const int name_at = line.rfind(" ", colon_at - 1) + 1;
		const String name = line.substr(name_at, colon_at - name_at);
		const uint32_t group = _read_uint(line, 7);
		for (const NamedBinding &named : p_named) {
			if (named.set != group) {
				continue;
			}
			int new_binding = -1;
			if (named.name == name) {
				new_binding = named.binding;
			} else if (named.combined && name == named.name + "_image") {
				new_binding = named.binding;
			} else if (named.combined && name == named.name + "_sampler") {
				new_binding = named.binding + RenderingShaderContainerWebGPU::COMBINED_SAMPLER_BINDING_OFFSET;
			}
			if (new_binding >= 0) {
				line = vformat("@group(%du) @binding(%du)", group, new_binding) + line.substr(var_at);
				break;
			}
		}
	}
	r_wgsl = String("\n").join(lines);
	for (const String &line : lines) {
		if (!line.begins_with("@group(")) {
			continue;
		}
		const int binding_at = line.find("@binding(");
		const int type_at = line.find(" : ");
		if (binding_at < 0 || type_at < 0 || line.find("var<") >= 0) {
			continue; // Buffers and push constants are not annotated.
		}
		const uint32_t group = _read_uint(line, 7);
		uint32_t binding = _read_uint(line, binding_at + 9);
		const String type = line.substr(type_at + 3).strip_edges().trim_suffix(";");
		bool combined_sampler = false;
		if (type.begins_with("sampler") && binding >= RenderingShaderContainerWebGPU::COMBINED_SAMPLER_BINDING_OFFSET) {
			binding -= RenderingShaderContainerWebGPU::COMBINED_SAMPLER_BINDING_OFFSET;
			combined_sampler = true;
		}
		for (int i = 0; i < p_uniform_keys.size(); i++) {
			if ((uint32_t)p_uniform_keys[i].x != group || (uint32_t)p_uniform_keys[i].y != binding) {
				continue;
			}
			RenderingShaderContainerWebGPU::BindingExtra &extra = r_extras.write[p_uniform_keys[i].z];
			if (type.begins_with("texture_depth")) {
				extra.multisampled = type.contains("multisampled");
			} else if (type.begins_with("texture_multisampled")) {
				extra.multisampled = 1;
			}
			if (type.ends_with("<u32>")) {
				extra.numeric = 2;
			} else if (type.ends_with("<i32>")) {
				extra.numeric = 1;
			} else if (!combined_sampler && type.begins_with("texture_")) {
				extra.numeric = type.begins_with("texture_depth") ? 0 : extra.numeric;
			}
			break;
		}
	}
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
	_set_from_shader_reflection_post(p_shader); // Make sure binding_extras matches this reflection before it is annotated.
#ifndef WEB_ENABLED
	Vector<NamedBinding> named_bindings;
	Vector<Vector3i> uniform_keys; // (set, binding, flat index into binding_extras).
	{
		int flat_index = 0;
		for (uint32_t set = 0; set < p_shader.uniform_sets.size(); set++) {
			for (const ReflectUniform &uniform : p_shader.uniform_sets[set]) {
				uniform_keys.push_back(Vector3i(set, uniform.binding, flat_index++));
				NamedBinding named;
				named.name = String::utf8(uniform.get_spv_reflect().name);
				named.set = set;
				named.binding = uniform.binding;
				named.combined = uniform.type == RDC::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
				named_bindings.push_back(named);
			}
		}
	}
#endif
	shaders.resize(stages.size());
	for (uint32_t i = 0; i < stages.size(); i++) {
		RenderingShaderContainer::Shader &shader = shaders.ptrw()[i];
		shader.shader_stage = stages[i].shader_stage;
		Vector<uint8_t> spirv = _lower_unsupported_instructions(_downgrade_spirv_version(_strip_buffer_non_readable(stages[i].spirv_data())));
		if (stages[i].shader_stage == RDC::SHADER_STAGE_VERTEX) {
			spirv = _flip_vertex_y(spirv);
		}
		shader.code_compression_flags = 0;
		shader.code_decompressed_size = 0;
#ifndef WEB_ENABLED
		if (!tint.is_empty()) {
			String wgsl;
			ERR_FAIL_COND_V(!_spirv_to_wgsl(tint, spirv, String::utf8(shader_name.get_data()), wgsl), false);
			_annotate_from_wgsl(wgsl, uniform_keys, named_bindings, binding_extras);
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
	int shadow_sampler_index = -1;
	bool has_shadow_atlas = false;
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
			const String lower_name = String::utf8(spv.name).to_lower();
			extra.depth_like = lower_name.contains("depth") || lower_name == "shadow_atlas" || lower_name == "directional_shadow_atlas" || lower_name == "source_cube";
			extra.nearest = lower_name.contains("nearest") && !lower_name.contains("mipmaps"); // Nearest with mipmaps filters between mip levels.
			// All variants of a shader have to give the same bind group layout, but Tint only knows that a texture is a depth
			// texture when this variant samples it with a comparison. Shadow maps and comparison samplers follow their names
			// (the 2D light shadows of the canvas are plain float textures, hence the `_texture` exception).
			const bool shadow_atlas = lower_name == "shadow_atlas" || lower_name == "directional_shadow_atlas";
			has_shadow_atlas = has_shadow_atlas || shadow_atlas;
			if (shadow_atlas && (uniform.type == RDC::UNIFORM_TYPE_TEXTURE || uniform.type == RDC::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE)) {
				extra.depth = 1;
			}
			if (lower_name == "shadow_sampler" && uniform.type == RDC::UNIFORM_TYPE_SAMPLER) {
				shadow_sampler_index = binding_extras.size();
			}
			binding_extras.push_back(extra);
		}
	}
	// The scene and fog shaders compare against their shadow atlases; the canvas `shadow_sampler` samples a float texture.
	if (shadow_sampler_index >= 0 && has_shadow_atlas) {
		binding_extras.write[shadow_sampler_index].comparison = 1;
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
