/**************************************************************************/
/*  rendering_context_driver_webgpu.h                                     */
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

#include "servers/rendering/rendering_context_driver.h"

#include <webgpu/webgpu.h>

class RenderingContextDriverWebGPU : public RenderingContextDriver {
	WGPUInstance instance = nullptr;
	WGPUAdapter adapter = nullptr;
	Device device;

	struct Surface {
		WGPUSurface surface = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
		DisplayServerEnums::VSyncMode vsync_mode = DisplayServerEnums::VSYNC_ENABLED;
		bool needs_resize = false;
	};

public:
	// What a window needs to be presented to: a canvas selector in the browser, a window handle natively.
	struct WindowPlatformData {
		const char *canvas_selector = "#canvas";
		void *window = nullptr; // HWND, or the X11 Window.
		void *instance = nullptr; // HINSTANCE, or the X11 Display *.
	};

	WGPUSurface surface_get(SurfaceID p_surface) const { return ((const Surface *)p_surface)->surface; }
	WGPUInstance instance_get() const { return instance; }
	WGPUAdapter adapter_get() const { return adapter; }

	// RenderingContextDriver overrides.
	virtual Error initialize() override;
	virtual const Device &device_get(uint32_t p_device_index) const override;
	virtual uint32_t device_get_count() const override;
	virtual bool device_supports_present(uint32_t p_device_index, SurfaceID p_surface) const override;
	virtual RenderingDeviceDriver *driver_create() override;
	virtual void driver_free(RenderingDeviceDriver *p_driver) override;
	virtual SurfaceID surface_create(const void *p_platform_data) override;
	virtual void surface_set_size(SurfaceID p_surface, uint32_t p_width, uint32_t p_height) override;
	virtual void surface_set_vsync_mode(SurfaceID p_surface, DisplayServerEnums::VSyncMode p_vsync_mode) override;
	virtual DisplayServerEnums::VSyncMode surface_get_vsync_mode(SurfaceID p_surface) const override;
	virtual void surface_set_hdr_output_enabled(SurfaceID p_surface, bool p_enabled) override {}
	virtual bool surface_get_hdr_output_enabled(SurfaceID p_surface) const override { return false; }
	virtual void surface_set_hdr_output_reference_luminance(SurfaceID p_surface, float p_reference_luminance) override {}
	virtual float surface_get_hdr_output_reference_luminance(SurfaceID p_surface) const override { return 0.0f; }
	virtual void surface_set_hdr_output_max_luminance(SurfaceID p_surface, float p_max_luminance) override {}
	virtual float surface_get_hdr_output_max_luminance(SurfaceID p_surface) const override { return 0.0f; }
	virtual void surface_set_hdr_output_linear_luminance_scale(SurfaceID p_surface, float p_linear_luminance_scale) override {}
	virtual float surface_get_hdr_output_linear_luminance_scale(SurfaceID p_surface) const override { return 0.0f; }
	virtual float surface_get_hdr_output_max_value(SurfaceID p_surface) const override { return 1.0f; }
	virtual uint32_t surface_get_width(SurfaceID p_surface) const override;
	virtual uint32_t surface_get_height(SurfaceID p_surface) const override;
	virtual void surface_set_needs_resize(SurfaceID p_surface, bool p_needs_resize) override;
	virtual bool surface_get_needs_resize(SurfaceID p_surface) const override;
	virtual void surface_destroy(SurfaceID p_surface) override;
	virtual bool is_debug_utils_enabled() const override { return false; }

	RenderingContextDriverWebGPU();
	virtual ~RenderingContextDriverWebGPU();
};

#endif // WEBGPU_ENABLED
