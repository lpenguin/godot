/**************************************************************************/
/*  webgpu_wait.h                                                         */
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

#include <webgpu/webgpu.h>
#ifndef __EMSCRIPTEN__
#include <webgpu/wgpu.h>
#endif

// Waiting for asynchronous WebGPU operations.
//
// wgpu-native does not implement wgpuInstanceWaitAny, so natively the callback flag is polled while the
// device and instance events are pumped. In the browser the main thread must not block: wgpuInstanceWaitAny
// suspends the WebAssembly stack through JSPI (or Asyncify) until the promise behind the future settles.

#ifdef __EMSCRIPTEN__
#define WEBGPU_CALLBACK_MODE WGPUCallbackMode_WaitAnyOnly
#else
#define WEBGPU_CALLBACK_MODE WGPUCallbackMode_AllowProcessEvents
#endif

inline void webgpu_wait(WGPUInstance p_instance, WGPUDevice p_device, WGPUFuture p_future, const volatile bool &p_done) {
#ifdef __EMSCRIPTEN__
	WGPUFutureWaitInfo info = {};
	info.future = p_future;
	wgpuInstanceWaitAny(p_instance, 1, &info, UINT64_MAX);
#else
	while (!p_done) {
		if (p_device) {
			wgpuDevicePoll(p_device, true, nullptr);
		}
		wgpuInstanceProcessEvents(p_instance);
	}
#endif
}

#endif // WEBGPU_ENABLED
