/**************************************************************************/
/*  rendering_device_driver_webgpu.cpp                                    */
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

#include "rendering_device_driver_webgpu.h"

#include "core/config/engine.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "thirdparty/spirv-reflect/spirv_reflect.h"

namespace {

constexpr uint32_t COPY_BYTES_PER_ROW_ALIGNMENT = 256;

WGPUStringView _sv(const char *p_text) {
	WGPUStringView view = {};
	view.data = p_text;
	view.length = WGPU_STRLEN;
	return view;
}

String _sv_to_string(WGPUStringView p_view) {
	if (p_view.data == nullptr || p_view.length == 0) {
		return String();
	}
	size_t length = p_view.length == WGPU_STRLEN ? strlen(p_view.data) : p_view.length;
	return String::utf8(p_view.data, (int)length);
}

_FORCE_INLINE_ uint64_t _align_up(uint64_t p_value, uint64_t p_alignment) {
	return (p_value + p_alignment - 1) / p_alignment * p_alignment;
}

struct DeviceRequest {
	WGPUDevice device = nullptr;
	String message;
	bool done = false;
};

void _on_device_request(WGPURequestDeviceStatus p_status, WGPUDevice p_device, WGPUStringView p_message, void *p_userdata1, void *p_userdata2) {
	DeviceRequest *request = (DeviceRequest *)p_userdata1;
	request->done = true;
	if (p_status == WGPURequestDeviceStatus_Success) {
		request->device = p_device;
	} else {
		request->message = _sv_to_string(p_message);
	}
}

void _on_uncaptured_error(WGPUDevice const *p_device, WGPUErrorType p_type, WGPUStringView p_message, void *p_userdata1, void *p_userdata2) {
	ERR_PRINT(vformat("WebGPU error (type %d): %s", (int)p_type, _sv_to_string(p_message)));
}

void _on_device_lost(WGPUDevice const *p_device, WGPUDeviceLostReason p_reason, WGPUStringView p_message, void *p_userdata1, void *p_userdata2) {
	if (p_reason != WGPUDeviceLostReason_Destroyed) {
		ERR_PRINT(vformat("WebGPU device lost (reason %d): %s", (int)p_reason, _sv_to_string(p_message)));
	}
}

struct MapRequest {
	WGPUMapAsyncStatus status = WGPUMapAsyncStatus_Error;
	bool done = false;
};

void _on_map(WGPUMapAsyncStatus p_status, WGPUStringView p_message, void *p_userdata1, void *p_userdata2) {
	MapRequest *request = (MapRequest *)p_userdata1;
	request->status = p_status;
	request->done = true;
	if (p_status != WGPUMapAsyncStatus_Success) {
		ERR_PRINT(vformat("WebGPU buffer map failed: %s", _sv_to_string(p_message)));
	}
}

void _on_work_done(WGPUQueueWorkDoneStatus p_status, WGPUStringView p_message, void *p_userdata1, void *p_userdata2) {
	*(volatile bool *)p_userdata1 = true;
}

WGPUTextureFormat _to_wgpu_format(RenderingDeviceCommons::DataFormat p_format) {
	using RDC = RenderingDeviceCommons;
	switch (p_format) {
		case RDC::DATA_FORMAT_R8_UNORM:
			return WGPUTextureFormat_R8Unorm;
		case RDC::DATA_FORMAT_R8_SNORM:
			return WGPUTextureFormat_R8Snorm;
		case RDC::DATA_FORMAT_R8_UINT:
			return WGPUTextureFormat_R8Uint;
		case RDC::DATA_FORMAT_R8_SINT:
			return WGPUTextureFormat_R8Sint;
		case RDC::DATA_FORMAT_R8G8_UNORM:
			return WGPUTextureFormat_RG8Unorm;
		case RDC::DATA_FORMAT_R8G8_SNORM:
			return WGPUTextureFormat_RG8Snorm;
		case RDC::DATA_FORMAT_R8G8_UINT:
			return WGPUTextureFormat_RG8Uint;
		case RDC::DATA_FORMAT_R8G8_SINT:
			return WGPUTextureFormat_RG8Sint;
		case RDC::DATA_FORMAT_R8G8B8A8_UNORM:
			return WGPUTextureFormat_RGBA8Unorm;
		case RDC::DATA_FORMAT_R8G8B8A8_SNORM:
			return WGPUTextureFormat_RGBA8Snorm;
		case RDC::DATA_FORMAT_R8G8B8A8_UINT:
			return WGPUTextureFormat_RGBA8Uint;
		case RDC::DATA_FORMAT_R8G8B8A8_SINT:
			return WGPUTextureFormat_RGBA8Sint;
		case RDC::DATA_FORMAT_R8G8B8A8_SRGB:
			return WGPUTextureFormat_RGBA8UnormSrgb;
		case RDC::DATA_FORMAT_B8G8R8A8_UNORM:
			return WGPUTextureFormat_BGRA8Unorm;
		case RDC::DATA_FORMAT_B8G8R8A8_SRGB:
			return WGPUTextureFormat_BGRA8UnormSrgb;
		case RDC::DATA_FORMAT_A2B10G10R10_UNORM_PACK32:
			return WGPUTextureFormat_RGB10A2Unorm;
		case RDC::DATA_FORMAT_A2B10G10R10_UINT_PACK32:
			return WGPUTextureFormat_RGB10A2Uint;
		case RDC::DATA_FORMAT_R16_UNORM:
			return WGPUTextureFormat_R16Unorm;
		case RDC::DATA_FORMAT_R16_SNORM:
			return WGPUTextureFormat_R16Snorm;
		case RDC::DATA_FORMAT_R16_UINT:
			return WGPUTextureFormat_R16Uint;
		case RDC::DATA_FORMAT_R16_SINT:
			return WGPUTextureFormat_R16Sint;
		case RDC::DATA_FORMAT_R16_SFLOAT:
			return WGPUTextureFormat_R16Float;
		case RDC::DATA_FORMAT_R16G16_UNORM:
			return WGPUTextureFormat_RG16Unorm;
		case RDC::DATA_FORMAT_R16G16_SNORM:
			return WGPUTextureFormat_RG16Snorm;
		case RDC::DATA_FORMAT_R16G16_UINT:
			return WGPUTextureFormat_RG16Uint;
		case RDC::DATA_FORMAT_R16G16_SINT:
			return WGPUTextureFormat_RG16Sint;
		case RDC::DATA_FORMAT_R16G16_SFLOAT:
			return WGPUTextureFormat_RG16Float;
		case RDC::DATA_FORMAT_R16G16B16A16_UNORM:
			return WGPUTextureFormat_RGBA16Unorm;
		case RDC::DATA_FORMAT_R16G16B16A16_SNORM:
			return WGPUTextureFormat_RGBA16Snorm;
		case RDC::DATA_FORMAT_R16G16B16A16_UINT:
			return WGPUTextureFormat_RGBA16Uint;
		case RDC::DATA_FORMAT_R16G16B16A16_SINT:
			return WGPUTextureFormat_RGBA16Sint;
		case RDC::DATA_FORMAT_R16G16B16A16_SFLOAT:
			return WGPUTextureFormat_RGBA16Float;
		case RDC::DATA_FORMAT_R32_UINT:
			return WGPUTextureFormat_R32Uint;
		case RDC::DATA_FORMAT_R32_SINT:
			return WGPUTextureFormat_R32Sint;
		case RDC::DATA_FORMAT_R32_SFLOAT:
			return WGPUTextureFormat_R32Float;
		case RDC::DATA_FORMAT_R32G32_UINT:
			return WGPUTextureFormat_RG32Uint;
		case RDC::DATA_FORMAT_R32G32_SINT:
			return WGPUTextureFormat_RG32Sint;
		case RDC::DATA_FORMAT_R32G32_SFLOAT:
			return WGPUTextureFormat_RG32Float;
		case RDC::DATA_FORMAT_R32G32B32A32_UINT:
			return WGPUTextureFormat_RGBA32Uint;
		case RDC::DATA_FORMAT_R32G32B32A32_SINT:
			return WGPUTextureFormat_RGBA32Sint;
		case RDC::DATA_FORMAT_R32G32B32A32_SFLOAT:
			return WGPUTextureFormat_RGBA32Float;
		case RDC::DATA_FORMAT_B10G11R11_UFLOAT_PACK32:
			return WGPUTextureFormat_RG11B10Ufloat;
		case RDC::DATA_FORMAT_E5B9G9R9_UFLOAT_PACK32:
			return WGPUTextureFormat_RGB9E5Ufloat;
		case RDC::DATA_FORMAT_D16_UNORM:
			return WGPUTextureFormat_Depth16Unorm;
		case RDC::DATA_FORMAT_X8_D24_UNORM_PACK32:
			return WGPUTextureFormat_Depth24Plus;
		case RDC::DATA_FORMAT_D32_SFLOAT:
			return WGPUTextureFormat_Depth32Float;
		case RDC::DATA_FORMAT_S8_UINT:
			return WGPUTextureFormat_Stencil8;
		case RDC::DATA_FORMAT_D24_UNORM_S8_UINT:
			return WGPUTextureFormat_Depth24PlusStencil8;
		case RDC::DATA_FORMAT_D32_SFLOAT_S8_UINT:
			return WGPUTextureFormat_Depth32FloatStencil8;
		case RDC::DATA_FORMAT_BC1_RGB_UNORM_BLOCK:
		case RDC::DATA_FORMAT_BC1_RGBA_UNORM_BLOCK:
			return WGPUTextureFormat_BC1RGBAUnorm;
		case RDC::DATA_FORMAT_BC1_RGB_SRGB_BLOCK:
		case RDC::DATA_FORMAT_BC1_RGBA_SRGB_BLOCK:
			return WGPUTextureFormat_BC1RGBAUnormSrgb;
		case RDC::DATA_FORMAT_BC2_UNORM_BLOCK:
			return WGPUTextureFormat_BC2RGBAUnorm;
		case RDC::DATA_FORMAT_BC2_SRGB_BLOCK:
			return WGPUTextureFormat_BC2RGBAUnormSrgb;
		case RDC::DATA_FORMAT_BC3_UNORM_BLOCK:
			return WGPUTextureFormat_BC3RGBAUnorm;
		case RDC::DATA_FORMAT_BC3_SRGB_BLOCK:
			return WGPUTextureFormat_BC3RGBAUnormSrgb;
		case RDC::DATA_FORMAT_BC4_UNORM_BLOCK:
			return WGPUTextureFormat_BC4RUnorm;
		case RDC::DATA_FORMAT_BC4_SNORM_BLOCK:
			return WGPUTextureFormat_BC4RSnorm;
		case RDC::DATA_FORMAT_BC5_UNORM_BLOCK:
			return WGPUTextureFormat_BC5RGUnorm;
		case RDC::DATA_FORMAT_BC5_SNORM_BLOCK:
			return WGPUTextureFormat_BC5RGSnorm;
		case RDC::DATA_FORMAT_BC6H_UFLOAT_BLOCK:
			return WGPUTextureFormat_BC6HRGBUfloat;
		case RDC::DATA_FORMAT_BC6H_SFLOAT_BLOCK:
			return WGPUTextureFormat_BC6HRGBFloat;
		case RDC::DATA_FORMAT_BC7_UNORM_BLOCK:
			return WGPUTextureFormat_BC7RGBAUnorm;
		case RDC::DATA_FORMAT_BC7_SRGB_BLOCK:
			return WGPUTextureFormat_BC7RGBAUnormSrgb;
		case RDC::DATA_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
			return WGPUTextureFormat_ETC2RGB8Unorm;
		case RDC::DATA_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
			return WGPUTextureFormat_ETC2RGB8UnormSrgb;
		case RDC::DATA_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
			return WGPUTextureFormat_ETC2RGB8A1Unorm;
		case RDC::DATA_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
			return WGPUTextureFormat_ETC2RGB8A1UnormSrgb;
		case RDC::DATA_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
			return WGPUTextureFormat_ETC2RGBA8Unorm;
		case RDC::DATA_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
			return WGPUTextureFormat_ETC2RGBA8UnormSrgb;
		default:
			return WGPUTextureFormat_Undefined;
	}
}

// Storage-image format declared in the shader (SpvImageFormat) to WebGPU. Only the formats that WebGPU can use as storage textures.
WGPUTextureFormat _spv_image_format_to_wgpu(uint32_t p_spv_format) {
	switch (p_spv_format) {
		case SpvImageFormatRgba32f:
			return WGPUTextureFormat_RGBA32Float;
		case SpvImageFormatRgba16f:
			return WGPUTextureFormat_RGBA16Float;
		case SpvImageFormatR32f:
			return WGPUTextureFormat_R32Float;
		case SpvImageFormatRgba8:
			return WGPUTextureFormat_RGBA8Unorm;
		case SpvImageFormatRgba8Snorm:
			return WGPUTextureFormat_RGBA8Snorm;
		case SpvImageFormatRg32f:
			return WGPUTextureFormat_RG32Float;
		case SpvImageFormatRg16f:
			return WGPUTextureFormat_RG16Float;
		case SpvImageFormatR11fG11fB10f:
			return WGPUTextureFormat_RG11B10Ufloat;
		case SpvImageFormatR16f:
			return WGPUTextureFormat_R16Float;
		case SpvImageFormatRgba16:
			return WGPUTextureFormat_RGBA16Unorm;
		case SpvImageFormatRgb10A2:
			return WGPUTextureFormat_RGB10A2Unorm;
		case SpvImageFormatRg16:
			return WGPUTextureFormat_RG16Unorm;
		case SpvImageFormatRg8:
			return WGPUTextureFormat_RG8Unorm;
		case SpvImageFormatR16:
			return WGPUTextureFormat_R16Unorm;
		case SpvImageFormatR8:
			return WGPUTextureFormat_R8Unorm;
		case SpvImageFormatRgba16Snorm:
			return WGPUTextureFormat_RGBA16Snorm;
		case SpvImageFormatRg16Snorm:
			return WGPUTextureFormat_RG16Snorm;
		case SpvImageFormatRg8Snorm:
			return WGPUTextureFormat_RG8Snorm;
		case SpvImageFormatR16Snorm:
			return WGPUTextureFormat_R16Snorm;
		case SpvImageFormatR8Snorm:
			return WGPUTextureFormat_R8Snorm;
		case SpvImageFormatRgba32i:
			return WGPUTextureFormat_RGBA32Sint;
		case SpvImageFormatRgba16i:
			return WGPUTextureFormat_RGBA16Sint;
		case SpvImageFormatRgba8i:
			return WGPUTextureFormat_RGBA8Sint;
		case SpvImageFormatR32i:
			return WGPUTextureFormat_R32Sint;
		case SpvImageFormatRg32i:
			return WGPUTextureFormat_RG32Sint;
		case SpvImageFormatRg16i:
			return WGPUTextureFormat_RG16Sint;
		case SpvImageFormatRg8i:
			return WGPUTextureFormat_RG8Sint;
		case SpvImageFormatR16i:
			return WGPUTextureFormat_R16Sint;
		case SpvImageFormatR8i:
			return WGPUTextureFormat_R8Sint;
		case SpvImageFormatRgba32ui:
			return WGPUTextureFormat_RGBA32Uint;
		case SpvImageFormatRgba16ui:
			return WGPUTextureFormat_RGBA16Uint;
		case SpvImageFormatRgba8ui:
			return WGPUTextureFormat_RGBA8Uint;
		case SpvImageFormatR32ui:
			return WGPUTextureFormat_R32Uint;
		case SpvImageFormatRgb10a2ui:
			return WGPUTextureFormat_RGB10A2Uint;
		case SpvImageFormatRg32ui:
			return WGPUTextureFormat_RG32Uint;
		case SpvImageFormatRg16ui:
			return WGPUTextureFormat_RG16Uint;
		case SpvImageFormatRg8ui:
			return WGPUTextureFormat_RG8Uint;
		case SpvImageFormatR16ui:
			return WGPUTextureFormat_R16Uint;
		case SpvImageFormatR8ui:
			return WGPUTextureFormat_R8Uint;
		default:
			return WGPUTextureFormat_Undefined;
	}
}

WGPUTextureViewDimension _spv_dim_to_view_dimension(uint32_t p_dim, bool p_arrayed) {
	switch (p_dim) {
		case SpvDim1D:
			return WGPUTextureViewDimension_1D;
		case SpvDim2D:
			return p_arrayed ? WGPUTextureViewDimension_2DArray : WGPUTextureViewDimension_2D;
		case SpvDim3D:
			return WGPUTextureViewDimension_3D;
		case SpvDimCube:
			return p_arrayed ? WGPUTextureViewDimension_CubeArray : WGPUTextureViewDimension_Cube;
		default:
			return WGPUTextureViewDimension_2D;
	}
}

WGPUTextureViewDimension _texture_type_to_view_dimension(RenderingDeviceCommons::TextureType p_type) {
	using RDC = RenderingDeviceCommons;
	switch (p_type) {
		case RDC::TEXTURE_TYPE_1D:
			return WGPUTextureViewDimension_1D;
		case RDC::TEXTURE_TYPE_2D:
			return WGPUTextureViewDimension_2D;
		case RDC::TEXTURE_TYPE_3D:
			return WGPUTextureViewDimension_3D;
		case RDC::TEXTURE_TYPE_CUBE:
			return WGPUTextureViewDimension_Cube;
		case RDC::TEXTURE_TYPE_2D_ARRAY:
			return WGPUTextureViewDimension_2DArray;
		case RDC::TEXTURE_TYPE_CUBE_ARRAY:
			return WGPUTextureViewDimension_CubeArray;
		default:
			return WGPUTextureViewDimension_2D;
	}
}

WGPUCompareFunction _to_wgpu_compare(RenderingDeviceCommons::CompareOperator p_op) {
	using RDC = RenderingDeviceCommons;
	switch (p_op) {
		case RDC::COMPARE_OP_NEVER:
			return WGPUCompareFunction_Never;
		case RDC::COMPARE_OP_LESS:
			return WGPUCompareFunction_Less;
		case RDC::COMPARE_OP_EQUAL:
			return WGPUCompareFunction_Equal;
		case RDC::COMPARE_OP_LESS_OR_EQUAL:
			return WGPUCompareFunction_LessEqual;
		case RDC::COMPARE_OP_GREATER:
			return WGPUCompareFunction_Greater;
		case RDC::COMPARE_OP_NOT_EQUAL:
			return WGPUCompareFunction_NotEqual;
		case RDC::COMPARE_OP_GREATER_OR_EQUAL:
			return WGPUCompareFunction_GreaterEqual;
		default:
			return WGPUCompareFunction_Always;
	}
}

WGPUAddressMode _to_wgpu_address_mode(RenderingDeviceCommons::SamplerRepeatMode p_mode) {
	using RDC = RenderingDeviceCommons;
	switch (p_mode) {
		case RDC::SAMPLER_REPEAT_MODE_REPEAT:
			return WGPUAddressMode_Repeat;
		case RDC::SAMPLER_REPEAT_MODE_MIRRORED_REPEAT:
		case RDC::SAMPLER_REPEAT_MODE_MIRROR_CLAMP_TO_EDGE:
			return WGPUAddressMode_MirrorRepeat;
		default:
			return WGPUAddressMode_ClampToEdge;
	}
}

bool _format_has_storage_support(RenderingDeviceCommons::DataFormat p_format) {
	using RDC = RenderingDeviceCommons;
	switch (p_format) {
		case RDC::DATA_FORMAT_R8G8B8A8_UNORM:
		case RDC::DATA_FORMAT_R8G8B8A8_SNORM:
		case RDC::DATA_FORMAT_R8G8B8A8_UINT:
		case RDC::DATA_FORMAT_R8G8B8A8_SINT:
		case RDC::DATA_FORMAT_R16G16B16A16_UINT:
		case RDC::DATA_FORMAT_R16G16B16A16_SINT:
		case RDC::DATA_FORMAT_R16G16B16A16_SFLOAT:
		case RDC::DATA_FORMAT_R32_UINT:
		case RDC::DATA_FORMAT_R32_SINT:
		case RDC::DATA_FORMAT_R32_SFLOAT:
		case RDC::DATA_FORMAT_R32G32_UINT:
		case RDC::DATA_FORMAT_R32G32_SINT:
		case RDC::DATA_FORMAT_R32G32_SFLOAT:
		case RDC::DATA_FORMAT_R32G32B32A32_UINT:
		case RDC::DATA_FORMAT_R32G32B32A32_SINT:
		case RDC::DATA_FORMAT_R32G32B32A32_SFLOAT:
			return true;
		default:
			return false;
	}
}

bool _format_is_depth_stencil(RenderingDeviceCommons::DataFormat p_format) {
	using RDC = RenderingDeviceCommons;
	switch (p_format) {
		case RDC::DATA_FORMAT_D16_UNORM:
		case RDC::DATA_FORMAT_X8_D24_UNORM_PACK32:
		case RDC::DATA_FORMAT_D32_SFLOAT:
		case RDC::DATA_FORMAT_S8_UINT:
		case RDC::DATA_FORMAT_D16_UNORM_S8_UINT:
		case RDC::DATA_FORMAT_D24_UNORM_S8_UINT:
		case RDC::DATA_FORMAT_D32_SFLOAT_S8_UINT:
			return true;
		default:
			return false;
	}
}

} // namespace

#define WGPU_UNIMPLEMENTED_VOID() ERR_FAIL_MSG(vformat("WebGPU driver: %s is not implemented yet.", __FUNCTION__))
#define WGPU_UNIMPLEMENTED(m_ret) ERR_FAIL_V_MSG(m_ret, vformat("WebGPU driver: %s is not implemented yet.", __FUNCTION__))

// ----- Generic -----

RenderingDeviceDriverWebGPU::RenderingDeviceDriverWebGPU(RenderingContextDriverWebGPU *p_context_driver) :
		context_driver(p_context_driver) {
}

RenderingDeviceDriverWebGPU::~RenderingDeviceDriverWebGPU() {
	if (queue) {
		wgpuQueueRelease(queue);
	}
	if (device) {
		wgpuDeviceDestroy(device);
		wgpuDeviceRelease(device);
	}
}

void RenderingDeviceDriverWebGPU::_wait_for(WGPUFuture p_future, const volatile bool &p_done) {
	webgpu_wait(context_driver->instance_get(), device, p_future, p_done);
}

Error RenderingDeviceDriverWebGPU::initialize(uint32_t p_device_index, uint32_t p_frame_count) {
	WGPUAdapter adapter = context_driver->adapter_get();
	ERR_FAIL_NULL_V(adapter, ERR_CANT_CREATE);

	WGPULimits adapter_limits = {};
	wgpuAdapterGetLimits(adapter, &adapter_limits);

	LocalVector<WGPUFeatureName> features;
	const WGPUFeatureName optional_features[] = {
		WGPUFeatureName_Depth32FloatStencil8,
		WGPUFeatureName_TextureCompressionBC,
		WGPUFeatureName_TextureCompressionETC2,
		WGPUFeatureName_TextureCompressionASTC,
		WGPUFeatureName_ShaderF16,
		WGPUFeatureName_Float32Filterable,
		WGPUFeatureName_RG11B10UfloatRenderable,
		WGPUFeatureName_TimestampQuery,
		WGPUFeatureName_IndirectFirstInstance,
#ifndef __EMSCRIPTEN__
		(WGPUFeatureName)WGPUNativeFeature_Immediates,
		(WGPUFeatureName)WGPUNativeFeature_TextureFormat16bitNorm,
		(WGPUFeatureName)WGPUNativeFeature_TextureAdapterSpecificFormatFeatures,
		(WGPUFeatureName)WGPUNativeFeature_ClearTexture,
#endif
	};
	for (WGPUFeatureName feature : optional_features) {
		if (wgpuAdapterHasFeature(adapter, feature)) {
			features.push_back(feature);
		}
	}
#ifndef __EMSCRIPTEN__
	immediates_supported = wgpuAdapterHasFeature(adapter, (WGPUFeatureName)WGPUNativeFeature_Immediates);
#else
	immediates_supported = false;
#endif

	WGPUDeviceDescriptor device_desc = {};
	device_desc.label = _sv("Godot WebGPU device");
	device_desc.requiredFeatureCount = features.size();
	device_desc.requiredFeatures = features.ptr();
	device_desc.requiredLimits = &adapter_limits;
	device_desc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
	device_desc.deviceLostCallbackInfo.callback = _on_device_lost;
	device_desc.uncapturedErrorCallbackInfo.callback = _on_uncaptured_error;

	DeviceRequest request;
	WGPURequestDeviceCallbackInfo callback_info = {};
	callback_info.mode = WEBGPU_CALLBACK_MODE;
	callback_info.callback = _on_device_request;
	callback_info.userdata1 = &request;
	webgpu_wait(context_driver->instance_get(), nullptr, wgpuAdapterRequestDevice(adapter, &device_desc, callback_info), request.done);
	ERR_FAIL_NULL_V_MSG(request.device, ERR_CANT_CREATE, vformat("Failed to create the WebGPU device: %s", request.message));

	device = request.device;
	queue = wgpuDeviceGetQueue(device);
	wgpuDeviceGetLimits(device, &limits);

	capabilities.device_family = DEVICE_WEBGPU;
	capabilities.version_major = 1;
	capabilities.version_minor = 0;
	return OK;
}

// ----- Buffers -----

RenderingDeviceDriver::BufferID RenderingDeviceDriverWebGPU::buffer_create(uint64_t p_size, BitField<BufferUsageBits> p_usage, MemoryAllocationType p_allocation_type, uint64_t p_frames_drawn) {
	BufferInfo *info = memnew(BufferInfo);
	info->size = _align_up(p_size, 4);
	info->usage = p_usage;
	info->cpu = p_allocation_type == MEMORY_ALLOCATION_TYPE_CPU;

	WGPUBufferUsage usage = 0;
	if (info->cpu) {
		info->download = p_usage.has_flag(BUFFER_USAGE_TRANSFER_TO_BIT) && !p_usage.has_flag(BUFFER_USAGE_TRANSFER_FROM_BIT);
		usage = info->download ? (WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst) : (WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst);
		info->shadow.resize(info->size);
		memset(info->shadow.ptrw(), 0, info->size);
	} else {
		usage = WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
		if (p_usage.has_flag(BUFFER_USAGE_UNIFORM_BIT)) {
			usage |= WGPUBufferUsage_Uniform;
		}
		if (p_usage.has_flag(BUFFER_USAGE_STORAGE_BIT)) {
			usage |= WGPUBufferUsage_Storage;
		}
		if (p_usage.has_flag(BUFFER_USAGE_INDEX_BIT)) {
			usage |= WGPUBufferUsage_Index;
		}
		if (p_usage.has_flag(BUFFER_USAGE_VERTEX_BIT)) {
			usage |= WGPUBufferUsage_Vertex;
		}
		if (p_usage.has_flag(BUFFER_USAGE_INDIRECT_BIT)) {
			usage |= WGPUBufferUsage_Indirect;
		}
	}

	WGPUBufferDescriptor desc = {};
	desc.usage = usage;
	desc.size = info->size;
	info->buffer = wgpuDeviceCreateBuffer(device, &desc);
	if (!info->buffer) {
		memdelete(info);
		return BufferID();
	}
	total_memory_used += info->size;
	return BufferID(info);
}

bool RenderingDeviceDriverWebGPU::buffer_set_texel_format(BufferID p_buffer, DataFormat p_format) {
	WGPU_UNIMPLEMENTED(false);
}

void RenderingDeviceDriverWebGPU::buffer_free(BufferID p_buffer) {
	BufferInfo *info = (BufferInfo *)p_buffer.id;
	total_memory_used -= info->size;
	wgpuBufferDestroy(info->buffer);
	wgpuBufferRelease(info->buffer);
	memdelete(info);
}

uint64_t RenderingDeviceDriverWebGPU::buffer_get_allocation_size(BufferID p_buffer) {
	return ((const BufferInfo *)p_buffer.id)->size;
}

bool RenderingDeviceDriverWebGPU::_map_for_read(BufferInfo *p_buffer) {
	MapRequest request;
	WGPUBufferMapCallbackInfo callback_info = {};
	callback_info.mode = WEBGPU_CALLBACK_MODE;
	callback_info.callback = _on_map;
	callback_info.userdata1 = &request;
	_wait_for(wgpuBufferMapAsync(p_buffer->buffer, WGPUMapMode_Read, 0, p_buffer->size, callback_info), request.done);
	if (request.status != WGPUMapAsyncStatus_Success) {
		return false;
	}
	const void *mapped = wgpuBufferGetConstMappedRange(p_buffer->buffer, 0, p_buffer->size);
	memcpy(p_buffer->shadow.ptrw(), mapped, p_buffer->size);
	wgpuBufferUnmap(p_buffer->buffer);
	return true;
}

uint8_t *RenderingDeviceDriverWebGPU::buffer_map(BufferID p_buffer) {
	BufferInfo *info = (BufferInfo *)p_buffer.id;
	ERR_FAIL_COND_V_MSG(!info->cpu, nullptr, "WebGPU driver: only CPU buffers can be mapped.");
	if (info->download && info->gpu_written) {
		ERR_FAIL_COND_V(!_map_for_read(info), nullptr);
		info->gpu_written = false;
	}
	return info->shadow.ptrw();
}

void RenderingDeviceDriverWebGPU::buffer_unmap(BufferID p_buffer) {
	// Shadow memory stays valid; uploads are flushed lazily when the buffer is used as a copy source.
}

uint8_t *RenderingDeviceDriverWebGPU::buffer_persistent_map_advance(BufferID p_buffer, uint64_t p_frames_drawn) {
	WGPU_UNIMPLEMENTED(nullptr);
}

uint64_t RenderingDeviceDriverWebGPU::buffer_get_dynamic_offsets(Span<BufferID> p_buffers) {
	return 0;
}

uint64_t RenderingDeviceDriverWebGPU::buffer_get_device_address(BufferID p_buffer) {
	WGPU_UNIMPLEMENTED(0);
}

// ----- Textures -----

RenderingDeviceDriver::TextureID RenderingDeviceDriverWebGPU::texture_create(const TextureFormat &p_format, const TextureView &p_view) {
	WGPUTextureFormat wgpu_format = _to_wgpu_format(p_format.format);
	ERR_FAIL_COND_V_MSG(wgpu_format == WGPUTextureFormat_Undefined, TextureID(), vformat("WebGPU driver: unsupported texture format %d.", (int)p_format.format));
	ERR_FAIL_COND_V_MSG(p_format.texture_type == TEXTURE_TYPE_1D_ARRAY, TextureID(), "WebGPU driver: 1D texture arrays are not supported.");

	WGPUTextureUsage usage = WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst;
	if (p_format.usage_bits & TEXTURE_USAGE_SAMPLING_BIT) {
		usage |= WGPUTextureUsage_TextureBinding;
	}
	if (p_format.usage_bits & (TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
		usage |= WGPUTextureUsage_RenderAttachment;
	}
	if (p_format.usage_bits & TEXTURE_USAGE_STORAGE_BIT) {
		usage |= WGPUTextureUsage_StorageBinding;
	}

	LocalVector<WGPUTextureFormat> view_formats;
	for (int i = 0; i < p_format.shareable_formats.size(); i++) {
		WGPUTextureFormat view_format = _to_wgpu_format(p_format.shareable_formats[i]);
		if (view_format != WGPUTextureFormat_Undefined && view_format != wgpu_format) {
			view_formats.push_back(view_format);
		}
	}

	WGPUTextureDescriptor desc = {};
	desc.usage = usage;
	desc.dimension = p_format.texture_type == TEXTURE_TYPE_3D ? WGPUTextureDimension_3D : (p_format.texture_type == TEXTURE_TYPE_1D ? WGPUTextureDimension_1D : WGPUTextureDimension_2D);
	desc.size.width = p_format.width;
	desc.size.height = p_format.height;
	desc.size.depthOrArrayLayers = p_format.texture_type == TEXTURE_TYPE_3D ? p_format.depth : p_format.array_layers;
	desc.format = wgpu_format;
	desc.mipLevelCount = p_format.mipmaps;
	desc.sampleCount = (uint32_t)1 << (uint32_t)p_format.samples;
	desc.viewFormatCount = view_formats.size();
	desc.viewFormats = view_formats.ptr();

	WGPUTexture texture = wgpuDeviceCreateTexture(device, &desc);
	ERR_FAIL_NULL_V(texture, TextureID());

	TextureInfo *info = memnew(TextureInfo);
	info->texture = texture;
	info->format = p_format.format;
	info->wgpu_format = wgpu_format;
	info->type = p_format.texture_type;
	info->width = p_format.width;
	info->height = p_format.height;
	info->depth = p_format.depth;
	info->layers = p_format.array_layers;
	info->mipmaps = p_format.mipmaps;
	info->samples = desc.sampleCount;
	info->usage_bits = p_format.usage_bits;

	uint32_t pixel_size = get_image_format_required_size(p_format.format, p_format.width, p_format.height, p_format.depth, p_format.mipmaps);
	info->allocation_size = (uint64_t)pixel_size * MAX(1u, p_format.array_layers);
	total_memory_used += info->allocation_size;

	WGPUTextureViewDescriptor view_desc = {};
	view_desc.format = p_view.format != DATA_FORMAT_MAX && _to_wgpu_format(p_view.format) != WGPUTextureFormat_Undefined ? _to_wgpu_format(p_view.format) : wgpu_format;
	view_desc.dimension = _texture_type_to_view_dimension(p_format.texture_type);
	view_desc.baseMipLevel = 0;
	view_desc.mipLevelCount = p_format.mipmaps;
	view_desc.baseArrayLayer = 0;
	view_desc.arrayLayerCount = p_format.texture_type == TEXTURE_TYPE_3D ? 1 : p_format.array_layers;
	view_desc.aspect = WGPUTextureAspect_All;
	view_desc.usage = WGPUTextureUsage_None; // Inherit the texture usage.
	info->view = wgpuTextureCreateView(texture, &view_desc);
	info->view_dimension = view_desc.dimension;
	info->base_mip = 0;
	info->view_mip_count = view_desc.mipLevelCount;
	info->base_layer = 0;
	info->view_layer_count = view_desc.arrayLayerCount;
	return TextureID(info);
}

RenderingDeviceDriver::TextureID RenderingDeviceDriverWebGPU::texture_create_from_extension(uint64_t p_native_texture, TextureType p_type, DataFormat p_format, uint32_t p_array_layers, bool p_depth_stencil, uint32_t p_mipmaps) {
	WGPU_UNIMPLEMENTED(TextureID());
}

RenderingDeviceDriver::TextureID RenderingDeviceDriverWebGPU::texture_create_shared(TextureID p_original_texture, const TextureView &p_view) {
	const TextureInfo *original = (const TextureInfo *)p_original_texture.id;
	TextureInfo *info = memnew(TextureInfo);
	*info = *original;
	wgpuTextureAddRef(info->texture);

	WGPUTextureFormat view_format = _to_wgpu_format(p_view.format);
	if (view_format == WGPUTextureFormat_Undefined) {
		view_format = original->wgpu_format;
	}
	WGPUTextureViewDescriptor view_desc = {};
	view_desc.format = view_format;
	view_desc.dimension = _texture_type_to_view_dimension(original->type);
	view_desc.mipLevelCount = original->mipmaps;
	view_desc.arrayLayerCount = original->type == TEXTURE_TYPE_3D ? 1 : original->layers;
	view_desc.aspect = WGPUTextureAspect_All;
	view_desc.usage = WGPUTextureUsage_None; // Inherit the texture usage.
	info->view = wgpuTextureCreateView(info->texture, &view_desc);
	info->owns_texture = false;
	info->allocation_size = 0;
	info->view_dimension = view_desc.dimension;
	info->base_mip = 0;
	info->view_mip_count = view_desc.mipLevelCount;
	info->base_layer = 0;
	info->view_layer_count = view_desc.arrayLayerCount;
	return TextureID(info);
}

RenderingDeviceDriver::TextureID RenderingDeviceDriverWebGPU::texture_create_shared_from_slice(TextureID p_original_texture, const TextureView &p_view, TextureSliceType p_slice_type, uint32_t p_layer, uint32_t p_layers, uint32_t p_mipmap, uint32_t p_mipmaps) {
	const TextureInfo *original = (const TextureInfo *)p_original_texture.id;
	TextureInfo *info = memnew(TextureInfo);
	*info = *original;
	wgpuTextureAddRef(info->texture);

	WGPUTextureFormat view_format = _to_wgpu_format(p_view.format);
	if (view_format == WGPUTextureFormat_Undefined) {
		view_format = original->wgpu_format;
	}

	WGPUTextureViewDescriptor view_desc = {};
	view_desc.format = view_format;
	switch (p_slice_type) {
		case TEXTURE_SLICE_2D:
			view_desc.dimension = WGPUTextureViewDimension_2D;
			break;
		case TEXTURE_SLICE_CUBEMAP:
			view_desc.dimension = WGPUTextureViewDimension_Cube;
			break;
		case TEXTURE_SLICE_3D:
			view_desc.dimension = WGPUTextureViewDimension_3D;
			break;
		case TEXTURE_SLICE_2D_ARRAY:
			view_desc.dimension = WGPUTextureViewDimension_2DArray;
			break;
		default:
			view_desc.dimension = WGPUTextureViewDimension_2D;
			break;
	}
	view_desc.baseMipLevel = p_mipmap;
	view_desc.mipLevelCount = p_mipmaps;
	view_desc.baseArrayLayer = p_layer;
	view_desc.arrayLayerCount = p_slice_type == TEXTURE_SLICE_3D ? 1 : p_layers;
	view_desc.aspect = WGPUTextureAspect_All;
	view_desc.usage = WGPUTextureUsage_None; // Inherit the texture usage.
	info->view = wgpuTextureCreateView(info->texture, &view_desc);
	info->owns_texture = false;
	info->allocation_size = 0;
	info->view_dimension = view_desc.dimension;
	info->base_mip = view_desc.baseMipLevel;
	info->view_mip_count = view_desc.mipLevelCount;
	info->base_layer = view_desc.baseArrayLayer;
	info->view_layer_count = view_desc.arrayLayerCount;
	return TextureID(info);
}

void RenderingDeviceDriverWebGPU::texture_free(TextureID p_texture) {
	TextureInfo *info = (TextureInfo *)p_texture.id;
	total_memory_used -= info->allocation_size;
	if (info->view) {
		wgpuTextureViewRelease(info->view);
	}
	if (info->owns_texture) {
		wgpuTextureDestroy(info->texture);
	}
	wgpuTextureRelease(info->texture);
	memdelete(info);
}

uint64_t RenderingDeviceDriverWebGPU::texture_get_allocation_size(TextureID p_texture) {
	return ((const TextureInfo *)p_texture.id)->allocation_size;
}

void RenderingDeviceDriverWebGPU::texture_get_copyable_layout(TextureID p_texture, const TextureSubresource &p_subresource, TextureCopyableLayout *r_layout) {
	const TextureInfo *info = (const TextureInfo *)p_texture.id;
	uint32_t block_width = 1;
	uint32_t block_height = 1;
	get_compressed_image_format_block_dimensions(info->format, block_width, block_height);
	const uint32_t block_bytes = (block_width > 1 || block_height > 1) ? get_compressed_image_format_block_byte_size(info->format) : get_image_format_pixel_size(info->format);

	const uint32_t width = MAX(1u, info->width >> p_subresource.mipmap);
	const uint32_t height = MAX(1u, info->height >> p_subresource.mipmap);
	const uint32_t depth = info->type == TEXTURE_TYPE_3D ? MAX(1u, info->depth >> p_subresource.mipmap) : 1;

	const uint64_t row_bytes = (uint64_t)((width + block_width - 1) / block_width) * block_bytes;
	r_layout->row_pitch = _align_up(row_bytes, COPY_BYTES_PER_ROW_ALIGNMENT);
	r_layout->size = r_layout->row_pitch * ((height + block_height - 1) / block_height) * depth;
}

Vector<uint8_t> RenderingDeviceDriverWebGPU::texture_get_data(TextureID p_texture, uint32_t p_layer) {
	WGPU_UNIMPLEMENTED(Vector<uint8_t>());
}

BitField<RenderingDeviceDriver::TextureUsageBits> RenderingDeviceDriverWebGPU::texture_get_usages_supported_by_format(DataFormat p_format, bool p_cpu_readable) {
	if (_to_wgpu_format(p_format) == WGPUTextureFormat_Undefined) {
		return 0;
	}
	BitField<TextureUsageBits> supported = TEXTURE_USAGE_SAMPLING_BIT | TEXTURE_USAGE_CAN_UPDATE_BIT | TEXTURE_USAGE_CAN_COPY_FROM_BIT | TEXTURE_USAGE_CAN_COPY_TO_BIT;
	if (_format_is_depth_stencil(p_format)) {
		supported.set_flag(TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
	} else {
		uint32_t block_width = 1;
		uint32_t block_height = 1;
		get_compressed_image_format_block_dimensions(p_format, block_width, block_height);
		if (block_width == 1 && block_height == 1) {
			supported.set_flag(TEXTURE_USAGE_COLOR_ATTACHMENT_BIT);
		}
		if (_format_has_storage_support(p_format)) {
			supported.set_flag(TEXTURE_USAGE_STORAGE_BIT);
		}
	}
	return supported;
}

bool RenderingDeviceDriverWebGPU::texture_can_make_shared_with_format(TextureID p_texture, DataFormat p_format, bool &r_raw_reinterpretation) {
	r_raw_reinterpretation = false;
	const TextureInfo *info = (const TextureInfo *)p_texture.id;
	return info->format == p_format;
}

// ----- Samplers -----

RenderingDeviceDriver::SamplerID RenderingDeviceDriverWebGPU::sampler_create(const SamplerState &p_state) {
	WGPUSamplerDescriptor desc = {};
	desc.addressModeU = _to_wgpu_address_mode(p_state.repeat_u);
	desc.addressModeV = _to_wgpu_address_mode(p_state.repeat_v);
	desc.addressModeW = _to_wgpu_address_mode(p_state.repeat_w);
	desc.magFilter = p_state.mag_filter == SAMPLER_FILTER_LINEAR ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
	desc.minFilter = p_state.min_filter == SAMPLER_FILTER_LINEAR ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
	desc.mipmapFilter = p_state.mip_filter == SAMPLER_FILTER_LINEAR ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
	desc.lodMinClamp = p_state.min_lod;
	desc.lodMaxClamp = MIN(p_state.max_lod, 32.0f);
	desc.compare = p_state.enable_compare ? _to_wgpu_compare(p_state.compare_op) : WGPUCompareFunction_Undefined;
	const bool all_linear = desc.magFilter == WGPUFilterMode_Linear && desc.minFilter == WGPUFilterMode_Linear && desc.mipmapFilter == WGPUMipmapFilterMode_Linear;
	desc.maxAnisotropy = (p_state.use_anisotropy && all_linear) ? (uint16_t)CLAMP(p_state.anisotropy_max, 1.0f, 16.0f) : 1;

	SamplerInfo *info = memnew(SamplerInfo);
	info->sampler = wgpuDeviceCreateSampler(device, &desc);
	info->comparison = p_state.enable_compare;
	info->linear = desc.magFilter == WGPUFilterMode_Linear || desc.minFilter == WGPUFilterMode_Linear || desc.mipmapFilter == WGPUMipmapFilterMode_Linear;
	if (!info->sampler) {
		memdelete(info);
		return SamplerID();
	}
	return SamplerID(info);
}

void RenderingDeviceDriverWebGPU::sampler_free(SamplerID p_sampler) {
	SamplerInfo *info = (SamplerInfo *)p_sampler.id;
	wgpuSamplerRelease(info->sampler);
	memdelete(info);
}

bool RenderingDeviceDriverWebGPU::sampler_is_format_supported_for_filter(DataFormat p_format, SamplerFilter p_filter) {
	if (p_filter == SAMPLER_FILTER_NEAREST) {
		return true;
	}
	switch (p_format) {
		case DATA_FORMAT_R32_SFLOAT:
		case DATA_FORMAT_R32G32_SFLOAT:
		case DATA_FORMAT_R32G32B32A32_SFLOAT:
			return wgpuDeviceHasFeature(device, WGPUFeatureName_Float32Filterable);
		default:
			return true;
	}
}

// ----- Vertex formats (render path not implemented yet) -----

RenderingDeviceDriver::VertexFormatID RenderingDeviceDriverWebGPU::vertex_format_create(Span<VertexAttribute> p_vertex_attribs, const VertexAttributeBindingsMap &p_vertex_bindings) {
	WGPU_UNIMPLEMENTED(VertexFormatID());
}

void RenderingDeviceDriverWebGPU::vertex_format_free(VertexFormatID p_vertex_format) {
}

// ----- Barriers: WebGPU tracks resource usage itself. -----

void RenderingDeviceDriverWebGPU::command_pipeline_barrier(CommandBufferID p_cmd_buffer, BitField<PipelineStageBits> p_src_stages, BitField<PipelineStageBits> p_dst_stages, VectorView<MemoryAccessBarrier> p_memory_barriers, VectorView<BufferBarrier> p_buffer_barriers, VectorView<TextureBarrier> p_texture_barriers, VectorView<AccelerationStructureBarrier> p_acceleration_structure_barriers) {
}

// ----- Fences and semaphores -----

RenderingDeviceDriver::FenceID RenderingDeviceDriverWebGPU::fence_create() {
	return FenceID(memnew(FenceInfo));
}

Error RenderingDeviceDriverWebGPU::fence_wait(FenceID p_fence) {
	FenceInfo *fence = (FenceInfo *)p_fence.id;
	if (fence->pending) {
		_wait_for(fence->future, fence->done);
		fence->pending = false;
	}
	return OK;
}

void RenderingDeviceDriverWebGPU::fence_free(FenceID p_fence) {
	fence_wait(p_fence);
	memdelete((FenceInfo *)p_fence.id);
}

RenderingDeviceDriver::SemaphoreID RenderingDeviceDriverWebGPU::semaphore_create() {
	// WebGPU queues are implicitly ordered; semaphores carry no state.
	return SemaphoreID(uint64_t(1));
}

void RenderingDeviceDriverWebGPU::semaphore_free(SemaphoreID p_semaphore) {
}

// ----- Command queues, pools and buffers -----

RenderingDeviceDriver::CommandQueueFamilyID RenderingDeviceDriverWebGPU::command_queue_family_get(BitField<CommandQueueFamilyBits> p_cmd_queue_family_bits, RenderingContextDriver::SurfaceID p_surface) {
	if (p_cmd_queue_family_bits.has_flag(COMMAND_QUEUE_FAMILY_TRANSFER_BIT) && !p_cmd_queue_family_bits.has_flag(COMMAND_QUEUE_FAMILY_GRAPHICS_BIT) && !p_cmd_queue_family_bits.has_flag(COMMAND_QUEUE_FAMILY_COMPUTE_BIT)) {
		// No dedicated transfer family: the main one is used.
		return CommandQueueFamilyID();
	}
	return CommandQueueFamilyID(uint64_t(1));
}

RenderingDeviceDriver::CommandQueueID RenderingDeviceDriverWebGPU::command_queue_create(CommandQueueFamilyID p_cmd_queue_family, bool p_identify_as_main_queue) {
	return CommandQueueID(uint64_t(1));
}

Error RenderingDeviceDriverWebGPU::command_queue_execute_and_present(CommandQueueID p_cmd_queue, VectorView<SemaphoreID> p_wait_semaphores, VectorView<CommandBufferID> p_cmd_buffers, VectorView<SemaphoreID> p_cmd_semaphores, FenceID p_cmd_fence, VectorView<SwapChainID> p_swap_chains) {
	LocalVector<WGPUCommandBuffer> submit;
	for (uint32_t i = 0; i < p_cmd_buffers.size(); i++) {
		CommandBufferInfo *cmd = (CommandBufferInfo *)p_cmd_buffers[i].id;
		if (cmd->finished) {
			submit.push_back(cmd->finished);
			cmd->finished = nullptr;
		}
	}
	if (!submit.is_empty()) {
		wgpuQueueSubmit(queue, submit.size(), submit.ptr());
		for (WGPUCommandBuffer command_buffer : submit) {
			wgpuCommandBufferRelease(command_buffer);
		}
	}
	if (p_cmd_fence) {
		FenceInfo *fence = (FenceInfo *)p_cmd_fence.id;
		WGPUQueueWorkDoneCallbackInfo callback_info = {};
		callback_info.mode = WEBGPU_CALLBACK_MODE;
		callback_info.callback = _on_work_done;
		callback_info.userdata1 = (void *)&fence->done;
		fence->done = false;
		fence->future = wgpuQueueOnSubmittedWorkDone(queue, callback_info);
		fence->pending = true;
	}
	return OK;
}

void RenderingDeviceDriverWebGPU::command_queue_free(CommandQueueID p_cmd_queue) {
}

RenderingDeviceDriver::CommandPoolID RenderingDeviceDriverWebGPU::command_pool_create(CommandQueueFamilyID p_cmd_queue_family, CommandBufferType p_cmd_buffer_type) {
	return CommandPoolID(uint64_t(1));
}

bool RenderingDeviceDriverWebGPU::command_pool_reset(CommandPoolID p_cmd_pool) {
	return true;
}

void RenderingDeviceDriverWebGPU::command_pool_free(CommandPoolID p_cmd_pool) {
}

RenderingDeviceDriver::CommandBufferID RenderingDeviceDriverWebGPU::command_buffer_create(CommandPoolID p_cmd_pool) {
	return CommandBufferID(memnew(CommandBufferInfo));
}

bool RenderingDeviceDriverWebGPU::command_buffer_begin(CommandBufferID p_cmd_buffer) {
	CommandBufferInfo *cmd = (CommandBufferInfo *)p_cmd_buffer.id;
	if (cmd->encoder) {
		_end_compute_pass(cmd);
		wgpuCommandEncoderRelease(cmd->encoder);
	}
	if (cmd->finished) {
		wgpuCommandBufferRelease(cmd->finished);
		cmd->finished = nullptr;
	}
	WGPUCommandEncoderDescriptor desc = {};
	cmd->encoder = wgpuDeviceCreateCommandEncoder(device, &desc);
	return cmd->encoder != nullptr;
}

bool RenderingDeviceDriverWebGPU::command_buffer_begin_secondary(CommandBufferID p_cmd_buffer, RenderPassID p_render_pass, uint32_t p_subpass, FramebufferID p_framebuffer) {
	WGPU_UNIMPLEMENTED(false);
}

void RenderingDeviceDriverWebGPU::command_buffer_end(CommandBufferID p_cmd_buffer) {
	CommandBufferInfo *cmd = (CommandBufferInfo *)p_cmd_buffer.id;
	_end_compute_pass(cmd);
	if (cmd->encoder) {
		WGPUCommandBufferDescriptor desc = {};
		cmd->finished = wgpuCommandEncoderFinish(cmd->encoder, &desc);
		wgpuCommandEncoderRelease(cmd->encoder);
		cmd->encoder = nullptr;
	}
}

void RenderingDeviceDriverWebGPU::command_buffer_execute_secondary(CommandBufferID p_cmd_buffer, VectorView<CommandBufferID> p_secondary_cmd_buffers) {
	WGPU_UNIMPLEMENTED_VOID();
}

void RenderingDeviceDriverWebGPU::_end_compute_pass(CommandBufferInfo *p_cmd) {
	if (p_cmd->compute_pass) {
		wgpuComputePassEncoderEnd(p_cmd->compute_pass);
		wgpuComputePassEncoderRelease(p_cmd->compute_pass);
		p_cmd->compute_pass = nullptr;
	}
}

// ----- Swap chain and framebuffers (not implemented yet) -----

RenderingDeviceDriver::SwapChainID RenderingDeviceDriverWebGPU::swap_chain_create(RenderingContextDriver::SurfaceID p_surface) {
	WGPU_UNIMPLEMENTED(SwapChainID());
}

Error RenderingDeviceDriverWebGPU::swap_chain_resize(CommandQueueID p_cmd_queue, SwapChainID p_swap_chain, uint32_t p_desired_framebuffer_count) {
	WGPU_UNIMPLEMENTED(ERR_UNAVAILABLE);
}

RenderingDeviceDriver::FramebufferID RenderingDeviceDriverWebGPU::swap_chain_acquire_framebuffer(CommandQueueID p_cmd_queue, SwapChainID p_swap_chain, bool &r_resize_required) {
	WGPU_UNIMPLEMENTED(FramebufferID());
}

RenderingDeviceDriver::RenderPassID RenderingDeviceDriverWebGPU::swap_chain_get_render_pass(SwapChainID p_swap_chain) {
	WGPU_UNIMPLEMENTED(RenderPassID());
}

RenderingDeviceDriver::DataFormat RenderingDeviceDriverWebGPU::swap_chain_get_format(SwapChainID p_swap_chain) {
	return DATA_FORMAT_B8G8R8A8_UNORM;
}

RenderingDeviceDriver::ColorSpace RenderingDeviceDriverWebGPU::swap_chain_get_color_space(SwapChainID p_swap_chain) {
	return COLOR_SPACE_REC709_NONLINEAR_SRGB;
}

bool RenderingDeviceDriverWebGPU::swap_chain_get_hdr_output_supported(SwapChainID p_swap_chain) {
	return false;
}

void RenderingDeviceDriverWebGPU::swap_chain_free(SwapChainID p_swap_chain) {
}

RenderingDeviceDriver::FramebufferID RenderingDeviceDriverWebGPU::framebuffer_create(RenderPassID p_render_pass, VectorView<TextureID> p_attachments, uint32_t p_width, uint32_t p_height) {
	WGPU_UNIMPLEMENTED(FramebufferID());
}

void RenderingDeviceDriverWebGPU::framebuffer_free(FramebufferID p_framebuffer) {
}

// ----- Shaders -----

RenderingDeviceDriver::ShaderID RenderingDeviceDriverWebGPU::shader_create_from_container(const Ref<RenderingShaderContainer> &p_shader_container, const Vector<ImmutableSampler> &p_immutable_samplers) {
	ERR_FAIL_COND_V_MSG(!p_immutable_samplers.is_empty(), ShaderID(), "WebGPU driver: immutable samplers are not supported.");
	const RenderingShaderContainerWebGPU *container = Object::cast_to<RenderingShaderContainerWebGPU>(p_shader_container.ptr());
	ERR_FAIL_NULL_V(container, ShaderID());

	ShaderInfo *info = memnew(ShaderInfo);
	info->reflection = p_shader_container->get_shader_reflection();
	info->binding_extras = container->binding_extras;
	info->name = String::utf8(container->shader_name.get_data());
	info->push_constant_size = info->reflection.push_constant_size;
	const CharString name = info->name.utf8();

	bool failed = false;
	for (int i = 0; i < container->shaders.size(); i++) {
		const RenderingShaderContainer::Shader &shader = container->shaders[i];
		const bool is_wgsl = shader.code_compression_flags == RenderingShaderContainerWebGPU::COMPRESSION_FLAG_WGSL;
		if (!is_wgsl && (shader.code_compression_flags != 0 || (shader.code_compressed_bytes.size() % 4) != 0)) {
			ERR_PRINT("WebGPU driver: unexpected shader code encoding.");
			failed = true;
			break;
		}

		// Debug aid: GODOT_WEBGPU_DUMP_SPIRV=<dir> writes the code that is handed to WebGPU (.spv or .wgsl).
		const String dump_dir = OS::get_singleton()->get_environment("GODOT_WEBGPU_DUMP_SPIRV");
		if (!dump_dir.is_empty()) {
			Ref<FileAccess> dump = FileAccess::open(dump_dir.path_join(info->name.get_file().get_basename() + (is_wgsl ? ".wgsl" : ".spv")), FileAccess::WRITE);
			if (dump.is_valid()) {
				dump->store_buffer(shader.code_compressed_bytes.ptr(), shader.code_compressed_bytes.size());
			}
		}

		WGPUShaderModuleDescriptor module_desc = {};
		module_desc.label = _sv(name.get_data());
		LocalVector<uint32_t> words;
		WGPUShaderSourceWGSL wgsl_source = {};
#ifndef __EMSCRIPTEN__
		WGPUShaderSourceSPIRV spirv = {};
#endif
		if (is_wgsl) {
			wgsl_source.chain.sType = WGPUSType_ShaderSourceWGSL;
			wgsl_source.code.data = (const char *)shader.code_compressed_bytes.ptr();
			wgsl_source.code.length = shader.code_compressed_bytes.size();
			module_desc.nextInChain = &wgsl_source.chain;
		} else {
#ifndef __EMSCRIPTEN__
			words.resize(shader.code_compressed_bytes.size() / 4);
			memcpy(words.ptr(), shader.code_compressed_bytes.ptr(), shader.code_compressed_bytes.size());
			spirv.chain.sType = WGPUSType_ShaderSourceSPIRV;
			spirv.codeSize = words.size();
			spirv.code = words.ptr();
			module_desc.nextInChain = &spirv.chain;
#else
			ERR_PRINT("WebGPU driver: browsers need WGSL shaders; bake the container with GODOT_TINT_PATH set.");
			failed = true;
			break;
#endif
		}
		info->modules[shader.shader_stage] = wgpuDeviceCreateShaderModule(device, &module_desc);
		if (!info->modules[shader.shader_stage]) {
			failed = true;
			break;
		}
	}

	uint32_t flat_index = 0;
	if (!failed) {
		for (int set = 0; set < info->reflection.uniform_sets.size() && !failed; set++) {
			LocalVector<WGPUBindGroupLayoutEntry> entries;
			for (const ShaderUniform &uniform : info->reflection.uniform_sets[set]) {
				const RenderingShaderContainerWebGPU::BindingExtra extra = info->binding_extras[flat_index++];

				WGPUBindGroupLayoutEntry entry = {};
				entry.binding = uniform.binding;
				if (uniform.stages.has_flag(SHADER_STAGE_VERTEX_BIT)) {
					entry.visibility |= WGPUShaderStage_Vertex;
				}
				if (uniform.stages.has_flag(SHADER_STAGE_FRAGMENT_BIT)) {
					entry.visibility |= WGPUShaderStage_Fragment;
				}
				if (uniform.stages.has_flag(SHADER_STAGE_COMPUTE_BIT)) {
					entry.visibility |= WGPUShaderStage_Compute;
				}

				switch (uniform.type) {
					case UNIFORM_TYPE_UNIFORM_BUFFER:
						entry.buffer.type = WGPUBufferBindingType_Uniform;
						break;
					case UNIFORM_TYPE_STORAGE_BUFFER:
						entry.buffer.type = uniform.writable ? WGPUBufferBindingType_Storage : WGPUBufferBindingType_ReadOnlyStorage;
						break;
					case UNIFORM_TYPE_IMAGE: {
						entry.storageTexture.format = _spv_image_format_to_wgpu(extra.image_format);
						entry.storageTexture.viewDimension = _spv_dim_to_view_dimension(extra.dim, extra.arrayed);
						if (extra.readable && extra.writable) {
							entry.storageTexture.access = WGPUStorageTextureAccess_ReadWrite;
						} else if (extra.readable) {
							entry.storageTexture.access = WGPUStorageTextureAccess_ReadOnly;
						} else {
							entry.storageTexture.access = WGPUStorageTextureAccess_WriteOnly;
						}
						if (entry.storageTexture.format == WGPUTextureFormat_Undefined) {
							ERR_PRINT(vformat("WebGPU driver: storage image at binding %d has an unsupported format (shader '%s').", uniform.binding, info->name));
							failed = true;
						}
					} break;
					case UNIFORM_TYPE_TEXTURE:
						entry.texture.sampleType = extra.depth ? WGPUTextureSampleType_Depth : (extra.numeric == 1 ? WGPUTextureSampleType_Sint : (extra.numeric == 2 ? WGPUTextureSampleType_Uint : WGPUTextureSampleType_Float));
						entry.texture.viewDimension = _spv_dim_to_view_dimension(extra.dim, extra.arrayed);
						entry.texture.multisampled = extra.multisampled;
						break;
					case UNIFORM_TYPE_SAMPLER:
						entry.sampler.type = WGPUSamplerBindingType_Filtering;
						break;
					default:
						ERR_PRINT(vformat("WebGPU driver: uniform type %d is not supported yet (shader '%s').", (int)uniform.type, info->name));
						failed = true;
						break;
				}
				entries.push_back(entry);
			}
			WGPUBindGroupLayoutDescriptor layout_desc = {};
			layout_desc.entryCount = entries.size();
			layout_desc.entries = entries.ptr();
			WGPUBindGroupLayout layout = wgpuDeviceCreateBindGroupLayout(device, &layout_desc);
			info->set_layouts.push_back(layout);
		}
	}

	if (!failed) {
		WGPUPipelineLayoutDescriptor pipeline_layout_desc = {};
		pipeline_layout_desc.bindGroupLayoutCount = info->set_layouts.size();
		pipeline_layout_desc.bindGroupLayouts = info->set_layouts.ptr();
		pipeline_layout_desc.immediateSize = immediates_supported ? info->push_constant_size : 0;
		ERR_FAIL_COND_V_MSG(info->push_constant_size > 0 && !immediates_supported, ShaderID(), "WebGPU driver: the shader uses push constants, but the adapter does not support immediates.");
		info->pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &pipeline_layout_desc);
		failed = info->pipeline_layout == nullptr;
	}

	if (failed) {
		shader_free(ShaderID(info));
		return ShaderID();
	}
	return ShaderID(info);
}

void RenderingDeviceDriverWebGPU::shader_free(ShaderID p_shader) {
	ShaderInfo *info = (ShaderInfo *)p_shader.id;
	for (WGPUShaderModule &module : info->modules) {
		if (module) {
			wgpuShaderModuleRelease(module);
			module = nullptr;
		}
	}
	if (info->pipeline_layout) {
		wgpuPipelineLayoutRelease(info->pipeline_layout);
	}
	for (WGPUBindGroupLayout layout : info->set_layouts) {
		if (layout) {
			wgpuBindGroupLayoutRelease(layout);
		}
	}
	memdelete(info);
}

void RenderingDeviceDriverWebGPU::shader_destroy_modules(ShaderID p_shader) {
	ShaderInfo *info = (ShaderInfo *)p_shader.id;
	for (WGPUShaderModule &module : info->modules) {
		if (module) {
			wgpuShaderModuleRelease(module);
			module = nullptr;
		}
	}
}

// ----- Uniform sets -----

RenderingDeviceDriver::UniformSetID RenderingDeviceDriverWebGPU::uniform_set_create(VectorView<BoundUniform> p_uniforms, ShaderID p_shader, uint32_t p_set_index, int p_linear_pool_index) {
	const ShaderInfo *shader = (const ShaderInfo *)p_shader.id;
	ERR_FAIL_COND_V(p_set_index >= shader->set_layouts.size(), UniformSetID());

	LocalVector<WGPUBindGroupEntry> entries;
	LocalVector<WGPUTextureView> temporary_views;
	for (uint32_t i = 0; i < p_uniforms.size(); i++) {
		const BoundUniform &uniform = p_uniforms[i];
		ERR_FAIL_COND_V_MSG(uniform.ids.size() != 1, UniformSetID(), "WebGPU driver: uniform arrays are not supported.");
		WGPUBindGroupEntry entry = {};
		entry.binding = uniform.binding;
		switch (uniform.type) {
			case UNIFORM_TYPE_UNIFORM_BUFFER:
			case UNIFORM_TYPE_STORAGE_BUFFER: {
				const BufferInfo *buffer = (const BufferInfo *)uniform.ids[0].id;
				entry.buffer = buffer->buffer;
				entry.offset = 0;
				entry.size = buffer->size;
			} break;
			case UNIFORM_TYPE_IMAGE: {
				const TextureInfo *texture = (const TextureInfo *)uniform.ids[0].id;
				if (texture->view_mip_count > 1) {
					// Storage bindings must view a single mip level.
					WGPUTextureViewDescriptor view_desc = {};
					view_desc.format = texture->wgpu_format;
					view_desc.dimension = texture->view_dimension;
					view_desc.baseMipLevel = texture->base_mip;
					view_desc.mipLevelCount = 1;
					view_desc.baseArrayLayer = texture->base_layer;
					view_desc.arrayLayerCount = texture->view_layer_count;
					view_desc.aspect = WGPUTextureAspect_All;
					view_desc.usage = WGPUTextureUsage_None; // Inherit the texture usage.
					WGPUTextureView view = wgpuTextureCreateView(texture->texture, &view_desc);
					temporary_views.push_back(view);
					entry.textureView = view;
				} else {
					entry.textureView = texture->view;
				}
			} break;
			case UNIFORM_TYPE_TEXTURE: {
				const TextureInfo *texture = (const TextureInfo *)uniform.ids[0].id;
				entry.textureView = texture->view;
			} break;
			case UNIFORM_TYPE_SAMPLER: {
				const SamplerInfo *sampler = (const SamplerInfo *)uniform.ids[0].id;
				entry.sampler = sampler->sampler;
			} break;
			default:
				ERR_FAIL_V_MSG(UniformSetID(), vformat("WebGPU driver: uniform type %d is not supported yet.", (int)uniform.type));
		}
		entries.push_back(entry);
	}

	WGPUBindGroupDescriptor desc = {};
	desc.layout = shader->set_layouts[p_set_index];
	desc.entryCount = entries.size();
	desc.entries = entries.ptr();
	WGPUBindGroup bind_group = wgpuDeviceCreateBindGroup(device, &desc);
	for (WGPUTextureView view : temporary_views) {
		wgpuTextureViewRelease(view);
	}
	ERR_FAIL_NULL_V(bind_group, UniformSetID());

	UniformSetInfo *info = memnew(UniformSetInfo);
	info->bind_group = bind_group;
	return UniformSetID(info);
}

void RenderingDeviceDriverWebGPU::uniform_set_free(UniformSetID p_uniform_set) {
	UniformSetInfo *info = (UniformSetInfo *)p_uniform_set.id;
	wgpuBindGroupRelease(info->bind_group);
	memdelete(info);
}

uint32_t RenderingDeviceDriverWebGPU::uniform_sets_get_dynamic_offsets(VectorView<UniformSetID> p_uniform_sets, ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count) const {
	return 0;
}

void RenderingDeviceDriverWebGPU::command_uniform_set_prepare_for_use(CommandBufferID p_cmd_buffer, UniformSetID p_uniform_set, ShaderID p_shader, uint32_t p_set_index) {
}

// ----- Transfers -----

void RenderingDeviceDriverWebGPU::command_clear_buffer(CommandBufferID p_cmd_buffer, BufferID p_buffer, uint64_t p_offset, uint64_t p_size) {
	CommandBufferInfo *cmd = (CommandBufferInfo *)p_cmd_buffer.id;
	BufferInfo *buffer = (BufferInfo *)p_buffer.id;
	_end_compute_pass(cmd);
	const uint64_t size = p_size == BUFFER_WHOLE_SIZE ? buffer->size - p_offset : p_size;
	wgpuCommandEncoderClearBuffer(cmd->encoder, buffer->buffer, p_offset, _align_up(size, 4));
}

void RenderingDeviceDriverWebGPU::command_copy_buffer(CommandBufferID p_cmd_buffer, BufferID p_src_buffer, BufferID p_dst_buffer, VectorView<BufferCopyRegion> p_regions) {
	CommandBufferInfo *cmd = (CommandBufferInfo *)p_cmd_buffer.id;
	BufferInfo *src = (BufferInfo *)p_src_buffer.id;
	BufferInfo *dst = (BufferInfo *)p_dst_buffer.id;
	_end_compute_pass(cmd);
	for (uint32_t i = 0; i < p_regions.size(); i++) {
		const BufferCopyRegion &region = p_regions[i];
		const uint64_t size = MIN(_align_up(region.size, 4), MIN(src->size - region.src_offset, dst->size - region.dst_offset));
		if (src->cpu && !src->download) {
			// Flush the shadow copy of the upload staging buffer right before the GPU reads it.
			wgpuQueueWriteBuffer(queue, src->buffer, region.src_offset, src->shadow.ptr() + region.src_offset, size);
		}
		wgpuCommandEncoderCopyBufferToBuffer(cmd->encoder, src->buffer, region.src_offset, dst->buffer, region.dst_offset, size);
		if (dst->cpu) {
			dst->gpu_written = true;
		}
	}
}

void RenderingDeviceDriverWebGPU::command_copy_texture(CommandBufferID p_cmd_buffer, TextureID p_src_texture, TextureLayout p_src_texture_layout, TextureID p_dst_texture, TextureLayout p_dst_texture_layout, VectorView<TextureCopyRegion> p_regions) {
	CommandBufferInfo *cmd = (CommandBufferInfo *)p_cmd_buffer.id;
	const TextureInfo *src = (const TextureInfo *)p_src_texture.id;
	const TextureInfo *dst = (const TextureInfo *)p_dst_texture.id;
	_end_compute_pass(cmd);
	for (uint32_t i = 0; i < p_regions.size(); i++) {
		const TextureCopyRegion &region = p_regions[i];
		WGPUTexelCopyTextureInfo src_info = {};
		src_info.texture = src->texture;
		src_info.mipLevel = region.src_subresources.mipmap;
		src_info.origin = { (uint32_t)region.src_offset.x, (uint32_t)region.src_offset.y, src->type == TEXTURE_TYPE_3D ? (uint32_t)region.src_offset.z : region.src_subresources.base_layer };
		src_info.aspect = WGPUTextureAspect_All;
		WGPUTexelCopyTextureInfo dst_info = {};
		dst_info.texture = dst->texture;
		dst_info.mipLevel = region.dst_subresources.mipmap;
		dst_info.origin = { (uint32_t)region.dst_offset.x, (uint32_t)region.dst_offset.y, dst->type == TEXTURE_TYPE_3D ? (uint32_t)region.dst_offset.z : region.dst_subresources.base_layer };
		dst_info.aspect = WGPUTextureAspect_All;
		WGPUExtent3D extent = { (uint32_t)region.size.x, (uint32_t)region.size.y, src->type == TEXTURE_TYPE_3D ? (uint32_t)region.size.z : MAX(1u, region.src_subresources.layer_count) };
		wgpuCommandEncoderCopyTextureToTexture(cmd->encoder, &src_info, &dst_info, &extent);
	}
}

void RenderingDeviceDriverWebGPU::command_resolve_texture(CommandBufferID p_cmd_buffer, TextureID p_src_texture, TextureLayout p_src_texture_layout, uint32_t p_src_layer, uint32_t p_src_mipmap, TextureID p_dst_texture, TextureLayout p_dst_texture_layout, uint32_t p_dst_layer, uint32_t p_dst_mipmap) {
	WGPU_UNIMPLEMENTED_VOID();
}

void RenderingDeviceDriverWebGPU::command_clear_color_texture(CommandBufferID p_cmd_buffer, TextureID p_texture, TextureLayout p_texture_layout, const Color &p_color, const TextureSubresourceRange &p_subresources) {
	WGPU_UNIMPLEMENTED_VOID();
}

void RenderingDeviceDriverWebGPU::command_clear_depth_stencil_texture(CommandBufferID p_cmd_buffer, TextureID p_texture, TextureLayout p_texture_layout, float p_depth, uint8_t p_stencil, const TextureSubresourceRange &p_subresources) {
	WGPU_UNIMPLEMENTED_VOID();
}

void RenderingDeviceDriverWebGPU::command_copy_buffer_to_texture(CommandBufferID p_cmd_buffer, BufferID p_src_buffer, TextureID p_dst_texture, TextureLayout p_dst_texture_layout, VectorView<BufferTextureCopyRegion> p_regions) {
	CommandBufferInfo *cmd = (CommandBufferInfo *)p_cmd_buffer.id;
	BufferInfo *src = (BufferInfo *)p_src_buffer.id;
	const TextureInfo *dst = (const TextureInfo *)p_dst_texture.id;
	_end_compute_pass(cmd);

	uint32_t block_width = 1;
	uint32_t block_height = 1;
	get_compressed_image_format_block_dimensions(dst->format, block_width, block_height);

	for (uint32_t i = 0; i < p_regions.size(); i++) {
		const BufferTextureCopyRegion &region = p_regions[i];
		const uint32_t rows = (region.texture_region_size.y + block_height - 1) / block_height;
		const uint64_t bytes = region.row_pitch * rows * MAX(1, region.texture_region_size.z);
		if (src->cpu && !src->download) {
			const uint64_t flush_size = MIN(_align_up(bytes, 4), src->size - region.buffer_offset);
			wgpuQueueWriteBuffer(queue, src->buffer, region.buffer_offset, src->shadow.ptr() + region.buffer_offset, flush_size);
		}

		WGPUTexelCopyBufferInfo buffer_info = {};
		buffer_info.buffer = src->buffer;
		buffer_info.layout.offset = region.buffer_offset;
		buffer_info.layout.bytesPerRow = region.row_pitch;
		buffer_info.layout.rowsPerImage = rows;
		WGPUTexelCopyTextureInfo texture_info = {};
		texture_info.texture = dst->texture;
		texture_info.mipLevel = region.texture_subresource.mipmap;
		texture_info.origin = { (uint32_t)region.texture_offset.x, (uint32_t)region.texture_offset.y, dst->type == TEXTURE_TYPE_3D ? (uint32_t)region.texture_offset.z : region.texture_subresource.layer };
		texture_info.aspect = WGPUTextureAspect_All;
		WGPUExtent3D extent = { (uint32_t)region.texture_region_size.x, (uint32_t)region.texture_region_size.y, dst->type == TEXTURE_TYPE_3D ? (uint32_t)region.texture_region_size.z : 1u };
		wgpuCommandEncoderCopyBufferToTexture(cmd->encoder, &buffer_info, &texture_info, &extent);
	}
}

void RenderingDeviceDriverWebGPU::command_copy_texture_to_buffer(CommandBufferID p_cmd_buffer, TextureID p_src_texture, TextureLayout p_src_texture_layout, BufferID p_dst_buffer, VectorView<BufferTextureCopyRegion> p_regions) {
	CommandBufferInfo *cmd = (CommandBufferInfo *)p_cmd_buffer.id;
	const TextureInfo *src = (const TextureInfo *)p_src_texture.id;
	BufferInfo *dst = (BufferInfo *)p_dst_buffer.id;
	_end_compute_pass(cmd);

	uint32_t block_width = 1;
	uint32_t block_height = 1;
	get_compressed_image_format_block_dimensions(src->format, block_width, block_height);

	for (uint32_t i = 0; i < p_regions.size(); i++) {
		const BufferTextureCopyRegion &region = p_regions[i];
		WGPUTexelCopyBufferInfo buffer_info = {};
		buffer_info.buffer = dst->buffer;
		buffer_info.layout.offset = region.buffer_offset;
		buffer_info.layout.bytesPerRow = region.row_pitch;
		buffer_info.layout.rowsPerImage = (region.texture_region_size.y + block_height - 1) / block_height;
		WGPUTexelCopyTextureInfo texture_info = {};
		texture_info.texture = src->texture;
		texture_info.mipLevel = region.texture_subresource.mipmap;
		texture_info.origin = { (uint32_t)region.texture_offset.x, (uint32_t)region.texture_offset.y, src->type == TEXTURE_TYPE_3D ? (uint32_t)region.texture_offset.z : region.texture_subresource.layer };
		texture_info.aspect = WGPUTextureAspect_All;
		WGPUExtent3D extent = { (uint32_t)region.texture_region_size.x, (uint32_t)region.texture_region_size.y, src->type == TEXTURE_TYPE_3D ? (uint32_t)region.texture_region_size.z : 1u };
		wgpuCommandEncoderCopyTextureToBuffer(cmd->encoder, &texture_info, &buffer_info, &extent);
		dst->gpu_written = true;
	}
}

// ----- Pipelines -----

void RenderingDeviceDriverWebGPU::pipeline_free(PipelineID p_pipeline) {
	PipelineInfo *info = (PipelineInfo *)p_pipeline.id;
	if (info->compute) {
		wgpuComputePipelineRelease(info->compute);
	}
	if (info->render) {
		wgpuRenderPipelineRelease(info->render);
	}
	memdelete(info);
}

void RenderingDeviceDriverWebGPU::command_bind_push_constants(CommandBufferID p_cmd_buffer, ShaderID p_shader, uint32_t p_first_index, VectorView<uint32_t> p_data) {
	CommandBufferInfo *cmd = (CommandBufferInfo *)p_cmd_buffer.id;
	ERR_FAIL_COND(!immediates_supported);
	if (cmd->compute_pass) {
#ifndef __EMSCRIPTEN__
		wgpuComputePassEncoderSetImmediates(cmd->compute_pass, p_first_index * sizeof(uint32_t), p_data.ptr(), p_data.size() * sizeof(uint32_t));
#else
		WGPU_UNIMPLEMENTED_VOID();
#endif
	} else {
		WGPU_UNIMPLEMENTED_VOID();
	}
}

bool RenderingDeviceDriverWebGPU::pipeline_cache_create(const Vector<uint8_t> &p_data) {
	return false;
}

void RenderingDeviceDriverWebGPU::pipeline_cache_free() {
}

size_t RenderingDeviceDriverWebGPU::pipeline_cache_query_size() {
	return 0;
}

Vector<uint8_t> RenderingDeviceDriverWebGPU::pipeline_cache_serialize() {
	return Vector<uint8_t>();
}

// ----- Render passes and drawing (not implemented yet) -----

RenderingDeviceDriver::RenderPassID RenderingDeviceDriverWebGPU::render_pass_create(VectorView<Attachment> p_attachments, VectorView<Subpass> p_subpasses, VectorView<SubpassDependency> p_subpass_dependencies, uint32_t p_view_count, AttachmentReference p_fragment_density_map_attachment) {
	WGPU_UNIMPLEMENTED(RenderPassID());
}

void RenderingDeviceDriverWebGPU::render_pass_free(RenderPassID p_render_pass) {
}

void RenderingDeviceDriverWebGPU::command_begin_render_pass(CommandBufferID p_cmd_buffer, RenderPassID p_render_pass, FramebufferID p_framebuffer, CommandBufferType p_cmd_buffer_type, const Rect2i &p_rect, VectorView<RenderPassClearValue> p_clear_values) {
	WGPU_UNIMPLEMENTED_VOID();
}

void RenderingDeviceDriverWebGPU::command_end_render_pass(CommandBufferID p_cmd_buffer) {
	WGPU_UNIMPLEMENTED_VOID();
}

void RenderingDeviceDriverWebGPU::command_next_render_subpass(CommandBufferID p_cmd_buffer, CommandBufferType p_cmd_buffer_type) {
	WGPU_UNIMPLEMENTED_VOID();
}

void RenderingDeviceDriverWebGPU::command_render_set_viewport(CommandBufferID p_cmd_buffer, VectorView<Rect2i> p_viewports) {
	WGPU_UNIMPLEMENTED_VOID();
}

void RenderingDeviceDriverWebGPU::command_render_set_scissor(CommandBufferID p_cmd_buffer, VectorView<Rect2i> p_scissors) {
	WGPU_UNIMPLEMENTED_VOID();
}

void RenderingDeviceDriverWebGPU::command_render_clear_attachments(CommandBufferID p_cmd_buffer, VectorView<AttachmentClear> p_attachment_clears, VectorView<Rect2i> p_rects) {
	WGPU_UNIMPLEMENTED_VOID();
}

void RenderingDeviceDriverWebGPU::command_bind_render_pipeline(CommandBufferID p_cmd_buffer, PipelineID p_pipeline) {
	WGPU_UNIMPLEMENTED_VOID();
}

void RenderingDeviceDriverWebGPU::command_bind_render_uniform_sets(CommandBufferID p_cmd_buffer, VectorView<UniformSetID> p_uniform_sets, ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count, uint32_t p_dynamic_offsets) {
	WGPU_UNIMPLEMENTED_VOID();
}

void RenderingDeviceDriverWebGPU::command_render_draw(CommandBufferID p_cmd_buffer, uint32_t p_vertex_count, uint32_t p_instance_count, uint32_t p_base_vertex, uint32_t p_first_instance) {
	WGPU_UNIMPLEMENTED_VOID();
}

void RenderingDeviceDriverWebGPU::command_render_draw_indexed(CommandBufferID p_cmd_buffer, uint32_t p_index_count, uint32_t p_instance_count, uint32_t p_first_index, int32_t p_vertex_offset, uint32_t p_first_instance) {
	WGPU_UNIMPLEMENTED_VOID();
}

void RenderingDeviceDriverWebGPU::command_render_draw_indexed_indirect(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, uint32_t p_draw_count, uint32_t p_stride) {
	WGPU_UNIMPLEMENTED_VOID();
}

void RenderingDeviceDriverWebGPU::command_render_draw_indexed_indirect_count(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, BufferID p_count_buffer, uint64_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) {
	WGPU_UNIMPLEMENTED_VOID();
}

void RenderingDeviceDriverWebGPU::command_render_draw_indirect(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, uint32_t p_draw_count, uint32_t p_stride) {
	WGPU_UNIMPLEMENTED_VOID();
}

void RenderingDeviceDriverWebGPU::command_render_draw_indirect_count(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, BufferID p_count_buffer, uint64_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) {
	WGPU_UNIMPLEMENTED_VOID();
}

void RenderingDeviceDriverWebGPU::command_render_bind_vertex_buffers(CommandBufferID p_cmd_buffer, uint32_t p_binding_count, const BufferID *p_buffers, const uint64_t *p_offsets, uint64_t p_dynamic_offsets) {
	WGPU_UNIMPLEMENTED_VOID();
}

void RenderingDeviceDriverWebGPU::command_render_bind_index_buffer(CommandBufferID p_cmd_buffer, BufferID p_buffer, IndexBufferFormat p_format, uint64_t p_offset) {
	WGPU_UNIMPLEMENTED_VOID();
}

void RenderingDeviceDriverWebGPU::command_render_set_blend_constants(CommandBufferID p_cmd_buffer, const Color &p_constants) {
	WGPU_UNIMPLEMENTED_VOID();
}

void RenderingDeviceDriverWebGPU::command_render_set_line_width(CommandBufferID p_cmd_buffer, float p_width) {
	WGPU_UNIMPLEMENTED_VOID();
}

RenderingDeviceDriver::PipelineID RenderingDeviceDriverWebGPU::render_pipeline_create(ShaderID p_shader, VertexFormatID p_vertex_format, RenderPrimitive p_render_primitive, PipelineRasterizationState p_rasterization_state, PipelineMultisampleState p_multisample_state, PipelineDepthStencilState p_depth_stencil_state, PipelineColorBlendState p_blend_state, VectorView<int32_t> p_color_attachments, BitField<PipelineDynamicStateFlags> p_dynamic_state, RenderPassID p_render_pass, uint32_t p_render_subpass, VectorView<PipelineSpecializationConstant> p_specialization_constants) {
	WGPU_UNIMPLEMENTED(PipelineID());
}

// ----- Compute -----

void RenderingDeviceDriverWebGPU::command_bind_compute_pipeline(CommandBufferID p_cmd_buffer, PipelineID p_pipeline) {
	CommandBufferInfo *cmd = (CommandBufferInfo *)p_cmd_buffer.id;
	const PipelineInfo *pipeline = (const PipelineInfo *)p_pipeline.id;
	_ensure_compute_pass(cmd);
	wgpuComputePassEncoderSetPipeline(cmd->compute_pass, pipeline->compute);
}

void RenderingDeviceDriverWebGPU::command_bind_compute_uniform_sets(CommandBufferID p_cmd_buffer, VectorView<UniformSetID> p_uniform_sets, ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count, uint32_t p_dynamic_offsets) {
	CommandBufferInfo *cmd = (CommandBufferInfo *)p_cmd_buffer.id;
	_ensure_compute_pass(cmd);
	for (uint32_t i = 0; i < p_set_count; i++) {
		const UniformSetInfo *set = (const UniformSetInfo *)p_uniform_sets[i].id;
		wgpuComputePassEncoderSetBindGroup(cmd->compute_pass, p_first_set_index + i, set->bind_group, 0, nullptr);
	}
}

void RenderingDeviceDriverWebGPU::command_compute_dispatch(CommandBufferID p_cmd_buffer, uint32_t p_x_groups, uint32_t p_y_groups, uint32_t p_z_groups) {
	CommandBufferInfo *cmd = (CommandBufferInfo *)p_cmd_buffer.id;
	_ensure_compute_pass(cmd);
	wgpuComputePassEncoderDispatchWorkgroups(cmd->compute_pass, p_x_groups, p_y_groups, p_z_groups);
}

void RenderingDeviceDriverWebGPU::command_compute_dispatch_indirect(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset) {
	CommandBufferInfo *cmd = (CommandBufferInfo *)p_cmd_buffer.id;
	const BufferInfo *buffer = (const BufferInfo *)p_indirect_buffer.id;
	_ensure_compute_pass(cmd);
	wgpuComputePassEncoderDispatchWorkgroupsIndirect(cmd->compute_pass, buffer->buffer, p_offset);
}

RenderingDeviceDriver::PipelineID RenderingDeviceDriverWebGPU::compute_pipeline_create(ShaderID p_shader, VectorView<PipelineSpecializationConstant> p_specialization_constants) {
	ShaderInfo *shader = (ShaderInfo *)p_shader.id;
	ERR_FAIL_NULL_V(shader->modules[SHADER_STAGE_COMPUTE], PipelineID());

	// Specialization constants become pipeline-overridable constants, keyed by their SPIR-V id.
	LocalVector<CharString> keys;
	LocalVector<WGPUConstantEntry> constants;
	keys.resize(p_specialization_constants.size());
	for (uint32_t i = 0; i < p_specialization_constants.size(); i++) {
		const PipelineSpecializationConstant &constant = p_specialization_constants[i];
		keys[i] = itos(constant.constant_id).utf8();
		WGPUConstantEntry entry = {};
		entry.key = _sv(keys[i].get_data());
		switch (constant.type) {
			case PIPELINE_SPECIALIZATION_CONSTANT_TYPE_BOOL:
				entry.value = constant.bool_value ? 1.0 : 0.0;
				break;
			case PIPELINE_SPECIALIZATION_CONSTANT_TYPE_INT:
				entry.value = (double)constant.int_value;
				break;
			case PIPELINE_SPECIALIZATION_CONSTANT_TYPE_FLOAT:
				entry.value = (double)constant.float_value;
				break;
		}
		constants.push_back(entry);
	}

	WGPUComputePipelineDescriptor desc = {};
	desc.layout = shader->pipeline_layout;
	desc.compute.module = shader->modules[SHADER_STAGE_COMPUTE];
	desc.compute.entryPoint = _sv("main");
	desc.compute.constantCount = constants.size();
	desc.compute.constants = constants.ptr();
	WGPUComputePipeline pipeline = wgpuDeviceCreateComputePipeline(device, &desc);
	ERR_FAIL_NULL_V(pipeline, PipelineID());

	PipelineInfo *info = memnew(PipelineInfo);
	info->compute = pipeline;
	info->shader = shader;
	return PipelineID(info);
}

void RenderingDeviceDriverWebGPU::_ensure_compute_pass(CommandBufferInfo *p_cmd) {
	if (!p_cmd->compute_pass) {
		WGPUComputePassDescriptor desc = {};
		p_cmd->compute_pass = wgpuCommandEncoderBeginComputePass(p_cmd->encoder, &desc);
	}
}

// ----- Raytracing (not supported) -----

RenderingDeviceDriver::AccelerationStructureID RenderingDeviceDriverWebGPU::blas_create(VectorView<AccelerationStructureGeometry> p_geometries, BitField<AccelerationStructureFlagBits> p_flags) {
	WGPU_UNIMPLEMENTED(AccelerationStructureID());
}

RenderingDeviceDriver::AccelerationStructureID RenderingDeviceDriverWebGPU::tlas_create(uint32_t p_max_instance_count, BitField<AccelerationStructureFlagBits> p_flags) {
	WGPU_UNIMPLEMENTED(AccelerationStructureID());
}

void RenderingDeviceDriverWebGPU::acceleration_structure_instance_write(uint8_t *r_driver_instance, const AccelerationStructureInstance &p_instance) {
	WGPU_UNIMPLEMENTED_VOID();
}

void RenderingDeviceDriverWebGPU::acceleration_structure_free(AccelerationStructureID p_acceleration_structure) {
}

uint32_t RenderingDeviceDriverWebGPU::acceleration_structure_get_scratch_size_bytes(AccelerationStructureID p_acceleration_structure) {
	return 0;
}

RenderingDeviceDriver::RaytracingPipelineID RenderingDeviceDriverWebGPU::raytracing_pipeline_create(VectorView<PipelineShader> p_shaders, VectorView<uint32_t> p_raygen_shader_indices, VectorView<uint32_t> p_miss_shader_indices, VectorView<HitGroup> p_hit_groups, uint32_t p_max_trace_recursion_depth, ShaderID p_layout_defining_shader) {
	WGPU_UNIMPLEMENTED(RaytracingPipelineID());
}

void RenderingDeviceDriverWebGPU::raytracing_pipeline_free(RaytracingPipelineID p_pipeline) {
}

bool RenderingDeviceDriverWebGPU::raytracing_pipeline_get_shader_group_handles(RaytracingPipelineID p_pipeline, uint32_t p_group_index_offset, VectorView<uint32_t> p_group_indices, uint8_t *r_data, uint32_t p_data_stride_bytes) {
	return false;
}

void RenderingDeviceDriverWebGPU::command_build_blas(CommandBufferID p_cmd_buffer, AccelerationStructureID p_acceleration_structure, BufferID p_scratch_buffer) {
	WGPU_UNIMPLEMENTED_VOID();
}

void RenderingDeviceDriverWebGPU::command_build_tlas(CommandBufferID p_cmd_buffer, AccelerationStructureID p_acceleration_structure, BufferID p_scratch_buffer, BufferID p_instance_buffer, uint32_t p_instance_offset, uint32_t p_instance_count) {
	WGPU_UNIMPLEMENTED_VOID();
}

void RenderingDeviceDriverWebGPU::command_bind_raytracing_pipeline(CommandBufferID p_cmd_buffer, RaytracingPipelineID p_pipeline) {
	WGPU_UNIMPLEMENTED_VOID();
}

void RenderingDeviceDriverWebGPU::command_bind_raytracing_uniform_set(CommandBufferID p_cmd_buffer, UniformSetID p_uniform_set, ShaderID p_shader, uint32_t p_set_index) {
	WGPU_UNIMPLEMENTED_VOID();
}

void RenderingDeviceDriverWebGPU::command_trace_rays(CommandBufferID p_cmd_buffer, const ShaderBindingTable &p_raygen_sbt, const ShaderBindingTable &p_miss_sbt, const ShaderBindingTable &p_hit_sbt, uint32_t p_width, uint32_t p_height, uint32_t p_depth) {
	WGPU_UNIMPLEMENTED_VOID();
}

// ----- Timestamps (dummy: always zero) -----

RenderingDeviceDriver::QueryPoolID RenderingDeviceDriverWebGPU::timestamp_query_pool_create(uint32_t p_query_count) {
	return QueryPoolID(uint64_t(1));
}

void RenderingDeviceDriverWebGPU::timestamp_query_pool_free(QueryPoolID p_pool_id) {
}

void RenderingDeviceDriverWebGPU::timestamp_query_pool_get_results(QueryPoolID p_pool_id, uint32_t p_query_count, uint64_t *r_results) {
	memset(r_results, 0, p_query_count * sizeof(uint64_t));
}

uint64_t RenderingDeviceDriverWebGPU::timestamp_query_result_to_time(uint64_t p_result) {
	return p_result;
}

void RenderingDeviceDriverWebGPU::command_timestamp_query_pool_reset(CommandBufferID p_cmd_buffer, QueryPoolID p_pool_id, uint32_t p_query_count) {
}

void RenderingDeviceDriverWebGPU::command_timestamp_write(CommandBufferID p_cmd_buffer, QueryPoolID p_pool_id, uint32_t p_index) {
}

// ----- Labels, debugging, segments -----

void RenderingDeviceDriverWebGPU::command_begin_label(CommandBufferID p_cmd_buffer, const char *p_label_name, const Color &p_color) {
}

void RenderingDeviceDriverWebGPU::command_end_label(CommandBufferID p_cmd_buffer) {
}

void RenderingDeviceDriverWebGPU::command_insert_breadcrumb(CommandBufferID p_cmd_buffer, uint32_t p_data) {
}

void RenderingDeviceDriverWebGPU::begin_segment(uint32_t p_frame_index, uint32_t p_frames_drawn) {
}

void RenderingDeviceDriverWebGPU::end_segment() {
}

// ----- Misc -----

void RenderingDeviceDriverWebGPU::set_object_name(ObjectType p_type, ID p_driver_id, const String &p_name) {
}

uint64_t RenderingDeviceDriverWebGPU::get_resource_native_handle(DriverResource p_type, ID p_driver_id) {
	return 0;
}

uint64_t RenderingDeviceDriverWebGPU::get_total_memory_used() {
	return total_memory_used;
}

uint64_t RenderingDeviceDriverWebGPU::get_lazily_memory_used() {
	return 0;
}

uint64_t RenderingDeviceDriverWebGPU::limit_get(Limit p_limit) {
	switch (p_limit) {
		case LIMIT_MAX_BOUND_UNIFORM_SETS:
			return limits.maxBindGroups;
		case LIMIT_MAX_FRAMEBUFFER_COLOR_ATTACHMENTS:
			return limits.maxColorAttachments;
		case LIMIT_MAX_TEXTURES_PER_UNIFORM_SET:
			return limits.maxSampledTexturesPerShaderStage;
		case LIMIT_MAX_SAMPLERS_PER_UNIFORM_SET:
			return limits.maxSamplersPerShaderStage;
		case LIMIT_MAX_STORAGE_BUFFERS_PER_UNIFORM_SET:
			return limits.maxStorageBuffersPerShaderStage;
		case LIMIT_MAX_STORAGE_IMAGES_PER_UNIFORM_SET:
			return limits.maxStorageTexturesPerShaderStage;
		case LIMIT_MAX_UNIFORM_BUFFERS_PER_UNIFORM_SET:
			return limits.maxUniformBuffersPerShaderStage;
		case LIMIT_MAX_DRAW_INDEXED_INDEX:
			return 0xFFFFFFFF;
		case LIMIT_MAX_FRAMEBUFFER_HEIGHT:
		case LIMIT_MAX_FRAMEBUFFER_WIDTH:
		case LIMIT_MAX_TEXTURE_SIZE_2D:
		case LIMIT_MAX_TEXTURE_SIZE_CUBE:
			return limits.maxTextureDimension2D;
		case LIMIT_MAX_TEXTURE_ARRAY_LAYERS:
			return limits.maxTextureArrayLayers;
		case LIMIT_MAX_TEXTURE_SIZE_1D:
			return limits.maxTextureDimension1D;
		case LIMIT_MAX_TEXTURE_SIZE_3D:
			return limits.maxTextureDimension3D;
		case LIMIT_MAX_TEXTURES_PER_SHADER_STAGE:
			return limits.maxSampledTexturesPerShaderStage;
		case LIMIT_MAX_SAMPLERS_PER_SHADER_STAGE:
			return limits.maxSamplersPerShaderStage;
		case LIMIT_MAX_STORAGE_BUFFERS_PER_SHADER_STAGE:
			return limits.maxStorageBuffersPerShaderStage;
		case LIMIT_MAX_STORAGE_IMAGES_PER_SHADER_STAGE:
			return limits.maxStorageTexturesPerShaderStage;
		case LIMIT_MAX_UNIFORM_BUFFERS_PER_SHADER_STAGE:
			return limits.maxUniformBuffersPerShaderStage;
		case LIMIT_MAX_PUSH_CONSTANT_SIZE:
			return immediates_supported ? limits.maxImmediateSize : 0;
		case LIMIT_MAX_UNIFORM_BUFFER_SIZE:
			return limits.maxUniformBufferBindingSize;
		case LIMIT_MAX_VERTEX_INPUT_ATTRIBUTE_OFFSET:
			return limits.maxVertexBufferArrayStride;
		case LIMIT_MAX_VERTEX_INPUT_ATTRIBUTES:
			return limits.maxVertexAttributes;
		case LIMIT_MAX_VERTEX_INPUT_BINDINGS:
			return limits.maxVertexBuffers;
		case LIMIT_MAX_VERTEX_INPUT_BINDING_STRIDE:
			return limits.maxVertexBufferArrayStride;
		case LIMIT_MIN_UNIFORM_BUFFER_OFFSET_ALIGNMENT:
			return limits.minUniformBufferOffsetAlignment;
		case LIMIT_MAX_COMPUTE_SHARED_MEMORY_SIZE:
			return limits.maxComputeWorkgroupStorageSize;
		case LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X:
		case LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_Y:
		case LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_Z:
			return limits.maxComputeWorkgroupsPerDimension;
		case LIMIT_MAX_COMPUTE_WORKGROUP_INVOCATIONS:
			return limits.maxComputeInvocationsPerWorkgroup;
		case LIMIT_MAX_COMPUTE_WORKGROUP_SIZE_X:
			return limits.maxComputeWorkgroupSizeX;
		case LIMIT_MAX_COMPUTE_WORKGROUP_SIZE_Y:
			return limits.maxComputeWorkgroupSizeY;
		case LIMIT_MAX_COMPUTE_WORKGROUP_SIZE_Z:
			return limits.maxComputeWorkgroupSizeZ;
		case LIMIT_MAX_VIEWPORT_DIMENSIONS_X:
		case LIMIT_MAX_VIEWPORT_DIMENSIONS_Y:
			return limits.maxTextureDimension2D;
		case LIMIT_MAX_SHADER_VARYINGS:
			return limits.maxInterStageShaderVariables;
		default:
			return 0;
	}
}

uint64_t RenderingDeviceDriverWebGPU::api_trait_get(ApiTrait p_trait) {
	switch (p_trait) {
		case API_TRAIT_HONORS_PIPELINE_BARRIERS:
			return 0; // WebGPU synchronizes resource usage itself.
		case API_TRAIT_TEXTURE_TRANSFER_ALIGNMENT:
			return 4;
		case API_TRAIT_TEXTURE_DATA_ROW_PITCH_STEP:
			return COPY_BYTES_PER_ROW_ALIGNMENT;
		case API_TRAIT_SECONDARY_VIEWPORT_SCISSOR:
			return 0;
		case API_TRAIT_ACCELERATION_STRUCTURE_INSTANCE_SIZE:
		case API_TRAIT_SHADER_GROUP_HANDLE_SIZE:
		case API_TRAIT_SHADER_GROUP_BASE_ALIGNMENT:
		case API_TRAIT_SHADER_GROUP_HANDLE_ALIGNMENT:
			return 0;
		default:
			return RenderingDeviceDriver::api_trait_get(p_trait);
	}
}

bool RenderingDeviceDriverWebGPU::has_feature(Features p_feature) {
	switch (p_feature) {
		case SUPPORTS_FRAGMENT_SHADER_WITH_ONLY_SIDE_EFFECTS:
			return true;
		case SUPPORTS_HALF_FLOAT:
			return wgpuDeviceHasFeature(device, WGPUFeatureName_ShaderF16);
		default:
			return false;
	}
}

const RenderingDeviceDriver::MultiviewCapabilities &RenderingDeviceDriverWebGPU::get_multiview_capabilities() {
	return multiview_capabilities;
}

const RenderingDeviceDriver::FragmentShadingRateCapabilities &RenderingDeviceDriverWebGPU::get_fragment_shading_rate_capabilities() {
	return fsr_capabilities;
}

const RenderingDeviceDriver::FragmentDensityMapCapabilities &RenderingDeviceDriverWebGPU::get_fragment_density_map_capabilities() {
	return fdm_capabilities;
}

String RenderingDeviceDriverWebGPU::get_api_name() const {
	return "WebGPU";
}

String RenderingDeviceDriverWebGPU::get_api_version() const {
	return "wgpu-native";
}

String RenderingDeviceDriverWebGPU::get_pipeline_cache_uuid() const {
	return "webgpu";
}

const RenderingDeviceDriver::Capabilities &RenderingDeviceDriverWebGPU::get_capabilities() const {
	return capabilities;
}

const RenderingShaderContainerFormat &RenderingDeviceDriverWebGPU::get_shader_container_format() const {
	return shader_container_format;
}

#endif // WEBGPU_ENABLED
