/**************************************************************************/
/*  rendering_shader_container_webgpu.h                                   */
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

#pragma once

#ifdef WEBGPU_ENABLED

#include "servers/rendering/rendering_shader_container.h"

class RenderingShaderContainerWebGPU : public RenderingShaderContainer {
	GDSOFTCLASS(RenderingShaderContainerWebGPU, RenderingShaderContainer);

public:
	static const uint32_t FORMAT_VERSION;

	// Set in Shader::code_compression_flags when the stored code is WGSL text instead of SPIR-V words.
	enum CompressionFlagsWebGPU {
		COMPRESSION_FLAG_WGSL = 0x20000,
	};

	// Browsers have no push constants. WGSL shaders keep them in a uniform buffer that the driver binds in group 0 with a
	// dynamic offset, see RenderingDeviceDriverWebGPU::command_bind_push_constants().
	static const uint32_t PUSH_CONSTANT_BINDING = 900;
	static const uint32_t PUSH_CONSTANT_SLOT_SIZE = 256; // Also the minimum uniform buffer offset alignment.
	// WGSL has no combined texture/sampler: a `sampler2D` at binding N becomes a texture at N and a sampler at N + 500.
	static const uint32_t COMBINED_SAMPLER_BINDING_OFFSET = 500;

	// Per-binding information that WebGPU bind group layouts need and the generic reflection does not carry.
	struct BindingExtra {
		uint32_t image_format = 0; // SpvImageFormat.
		uint32_t dim = 0; // SpvDim.
		uint32_t arrayed = 0;
		uint32_t multisampled = 0;
		uint32_t depth = 0;
		uint32_t numeric = 0; // 0: float, 1: signed int, 2: unsigned int.
		uint32_t readable = 1;
		uint32_t writable = 1;
		uint32_t comparison = 0; // The sampler is a sampler_comparison (read from the WGSL; SPIR-V does not say).
	};

	Vector<BindingExtra> binding_extras;

protected:
	virtual uint32_t _format() const override;
	virtual uint32_t _format_version() const override;
	virtual bool _set_code_from_spirv(const ReflectShader &p_shader) override;
	virtual void _set_from_shader_reflection_post(const ReflectShader &p_shader) override;

	virtual uint32_t _from_bytes_reflection_binding_uniform_extra_data_start(const uint8_t *p_bytes) override;
	virtual uint32_t _from_bytes_reflection_binding_uniform_extra_data(const uint8_t *p_bytes, uint32_t p_index) override;
	virtual uint32_t _to_bytes_reflection_binding_uniform_extra_data(uint8_t *p_bytes, uint32_t p_index) const override;

public:
	RenderingShaderContainerWebGPU();
};

class RenderingShaderContainerFormatWebGPU : public RenderingShaderContainerFormat {
public:
	virtual Ref<RenderingShaderContainer> create_container() const override;
	virtual ShaderLanguageVersion get_shader_language_version() const override;
	virtual ShaderSpirvVersion get_shader_spirv_version() const override;
};

#endif // WEBGPU_ENABLED
