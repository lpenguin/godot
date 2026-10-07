/**************************************************************************/
/*  rendering_context_driver_webgpu.cpp                                   */
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

#include "rendering_context_driver_webgpu.h"

#include "rendering_device_driver_webgpu.h"
#include "webgpu_wait.h"

#include "core/string/print_string.h"

namespace {
struct AdapterRequest {
	WGPUAdapter adapter = nullptr;
	String message;
	bool done = false;
};

String _sv_to_string(WGPUStringView p_view) {
	if (p_view.data == nullptr || p_view.length == 0) {
		return String();
	}
	size_t length = p_view.length == WGPU_STRLEN ? strlen(p_view.data) : p_view.length;
	return String::utf8(p_view.data, (int)length);
}

void _on_adapter_request(WGPURequestAdapterStatus p_status, WGPUAdapter p_adapter, WGPUStringView p_message, void *p_userdata1, void *p_userdata2) {
	AdapterRequest *request = (AdapterRequest *)p_userdata1;
	request->done = true;
	if (p_status == WGPURequestAdapterStatus_Success) {
		request->adapter = p_adapter;
	} else {
		request->message = _sv_to_string(p_message);
	}
}
} // namespace

RenderingContextDriverWebGPU::RenderingContextDriverWebGPU() {
}

RenderingContextDriverWebGPU::~RenderingContextDriverWebGPU() {
	if (adapter) {
		wgpuAdapterRelease(adapter);
	}
	if (instance) {
		wgpuInstanceRelease(instance);
	}
}

Error RenderingContextDriverWebGPU::initialize() {
	const WGPUInstanceFeatureName instance_features[] = {
#ifdef __EMSCRIPTEN__
		WGPUInstanceFeatureName_TimedWaitAny,
#else
		WGPUInstanceFeatureName_ShaderSourceSPIRV,
#endif
	};
	WGPUInstanceDescriptor instance_desc = {};
	instance_desc.requiredFeatureCount = std::size(instance_features);
	instance_desc.requiredFeatures = instance_features;
	instance = wgpuCreateInstance(&instance_desc);
	ERR_FAIL_NULL_V_MSG(instance, ERR_CANT_CREATE, "Failed to create the WebGPU instance.");

	WGPURequestAdapterOptions options = {};
	options.powerPreference = WGPUPowerPreference_HighPerformance;

	AdapterRequest request;
	WGPURequestAdapterCallbackInfo callback_info = {};
	callback_info.mode = WEBGPU_CALLBACK_MODE;
	callback_info.callback = _on_adapter_request;
	callback_info.userdata1 = &request;
	webgpu_wait(instance, nullptr, wgpuInstanceRequestAdapter(instance, &options, callback_info), request.done);
	ERR_FAIL_COND_V_MSG(!request.adapter, ERR_CANT_CREATE, vformat("Failed to request a WebGPU adapter: %s", request.message));
	adapter = request.adapter;

	WGPUAdapterInfo info = {};
	if (wgpuAdapterGetInfo(adapter, &info) == WGPUStatus_Success) {
		device.name = _sv_to_string(info.device);
		if (device.name.is_empty()) {
			device.name = _sv_to_string(info.description);
		}
		device.vendor = info.vendorID;
		switch (info.adapterType) {
			case WGPUAdapterType_DiscreteGPU:
				device.type = DEVICE_TYPE_DISCRETE_GPU;
				break;
			case WGPUAdapterType_IntegratedGPU:
				device.type = DEVICE_TYPE_INTEGRATED_GPU;
				break;
			case WGPUAdapterType_CPU:
				device.type = DEVICE_TYPE_CPU;
				break;
			default:
				device.type = DEVICE_TYPE_OTHER;
				break;
		}
		wgpuAdapterInfoFreeMembers(info);
	}

	return OK;
}

const RenderingContextDriver::Device &RenderingContextDriverWebGPU::device_get(uint32_t p_device_index) const {
	DEV_ASSERT(p_device_index == 0);
	return device;
}

uint32_t RenderingContextDriverWebGPU::device_get_count() const {
	return adapter ? 1 : 0;
}

bool RenderingContextDriverWebGPU::device_supports_present(uint32_t p_device_index, SurfaceID p_surface) const {
	return true;
}

RenderingDeviceDriver *RenderingContextDriverWebGPU::driver_create() {
	return memnew(RenderingDeviceDriverWebGPU(this));
}

void RenderingContextDriverWebGPU::driver_free(RenderingDeviceDriver *p_driver) {
	memdelete(p_driver);
}

// Surfaces are not implemented yet; the compute-only proof of concept runs on a local device without a window.

RenderingContextDriver::SurfaceID RenderingContextDriverWebGPU::surface_create(const void *p_platform_data) {
	Surface *surface = memnew(Surface);
	return SurfaceID(surface);
}

void RenderingContextDriverWebGPU::surface_set_size(SurfaceID p_surface, uint32_t p_width, uint32_t p_height) {
	Surface *surface = (Surface *)p_surface;
	surface->width = p_width;
	surface->height = p_height;
	surface->needs_resize = true;
}

void RenderingContextDriverWebGPU::surface_set_vsync_mode(SurfaceID p_surface, DisplayServerEnums::VSyncMode p_vsync_mode) {
	Surface *surface = (Surface *)p_surface;
	surface->vsync_mode = p_vsync_mode;
	surface->needs_resize = true;
}

DisplayServerEnums::VSyncMode RenderingContextDriverWebGPU::surface_get_vsync_mode(SurfaceID p_surface) const {
	return ((const Surface *)p_surface)->vsync_mode;
}

uint32_t RenderingContextDriverWebGPU::surface_get_width(SurfaceID p_surface) const {
	return ((const Surface *)p_surface)->width;
}

uint32_t RenderingContextDriverWebGPU::surface_get_height(SurfaceID p_surface) const {
	return ((const Surface *)p_surface)->height;
}

void RenderingContextDriverWebGPU::surface_set_needs_resize(SurfaceID p_surface, bool p_needs_resize) {
	((Surface *)p_surface)->needs_resize = p_needs_resize;
}

bool RenderingContextDriverWebGPU::surface_get_needs_resize(SurfaceID p_surface) const {
	return ((const Surface *)p_surface)->needs_resize;
}

void RenderingContextDriverWebGPU::surface_destroy(SurfaceID p_surface) {
	memdelete((Surface *)p_surface);
}

#endif // WEBGPU_ENABLED
