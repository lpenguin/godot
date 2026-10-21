/**************************************************************************/
/*  webgpu_compilation_trace.h                                            */
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

#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/templates/safe_refcount.h"

// Diagnostic CPU/API timeline, not GPU timings or an acknowledgment of browser compilation.
namespace WebGPUCompilationTrace {
inline bool enabled() {
	static const bool requested = OS::get_singleton()->get_environment("GODOT_WEBGPU_TRACE_COMPILATION") == "1" || OS::get_singleton()->get_cmdline_user_args().find("--trace-webgpu-compilation") != nullptr;
	return requested;
}

class Scope {
	uint64_t id = 0;
	uint64_t started = 0;
	String kind;
	String shader;
	bool accepted = false;

public:
	Scope(const String &p_kind, const String &p_shader, uint64_t p_related_id = 0) {
		if (!enabled()) {
			return;
		}
		static SafeNumeric<uint64_t> next_id{ 0 };
		id = next_id.increment();
		kind = p_kind;
		shader = p_shader;
		print_line("WEBGPU_COMPILE {\"event\":\"begin\",\"id\":" + uitos(id) + ",\"related_id\":" + uitos(p_related_id) + ",\"t_us\":" + uitos(OS::get_singleton()->get_ticks_usec()) + ",\"kind\":\"" + kind.json_escape() + "\",\"shader\":\"" + shader.json_escape() + "\"}");
		// Exclude the begin log itself from the API duration.
		started = OS::get_singleton()->get_ticks_usec();
	}

	uint64_t get_id() const { return id; }
	void set_accepted(bool p_accepted) { accepted = p_accepted; }

	~Scope() {
		if (id == 0) {
			return;
		}
		const uint64_t now = OS::get_singleton()->get_ticks_usec();
		print_line("WEBGPU_COMPILE {\"event\":\"end\",\"id\":" + uitos(id) + ",\"t_us\":" + uitos(now) + ",\"elapsed_us\":" + uitos(now - started) + ",\"handle_returned\":" + (accepted ? "true" : "false") + ",\"kind\":\"" + kind.json_escape() + "\",\"shader\":\"" + shader.json_escape() + "\"}");
	}
};
} // namespace WebGPUCompilationTrace
