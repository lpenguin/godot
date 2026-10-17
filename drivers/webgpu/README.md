# Experimental WebGPU backend for RenderingDevice

Branch `feature/webgpu-driver-poc` (fork `lpenguin/godot`, base `4.7.2-stable`, pull request lpenguin/godot#2).
Goal: run Godot's Forward+ renderer, and the WoodWorks project that uses it, in a browser through WebGPU.

## Status

| Area | State |
|---|---|
| RenderingDevice driver (buffers, textures, samplers, bind groups, compute, copies, fences) | done, runs natively (wgpu-native) and in the browser (Emscripten + JSPI) |
| WoodWorks compute shaders (carve, shaving, surface probe, initial fill/classify) | identical to Vulkan natively and in Chrome 154 (`test_webgpu_parity`) |
| Render path (render pass, framebuffer, vertex formats, render pipeline, draws) | done; triangle matches Vulkan pixel for pixel (`test_webgpu_triangle`) |
| Surface and swap chain, presenting to a `<canvas>` | done, triangle shown in the browser through `RenderingDevice` (`test_webgpu_screen`) |
| `DisplayServerWeb` creating a WebGPU context instead of WebGL2 | done: the WoodWorks web build runs in Chrome and Firefox (see the WoodWorks repository, `docs/web_webgpu.md`) |
| Windows display server using this driver (`--rendering-driver webgpu`) | implemented: creates HWND surfaces for the main window and subwindows; Vulkan remains the default when enabled |
| Built-in Forward+ shaders through Tint | **partial**: see [SHADER_COMPATIBILITY.md](SHADER_COMPATIBILITY.md) (11% as is, 50% without texture arrays) |
| Push constants in browsers | emulated with a uniform ring and a dynamic offset per draw/dispatch; uploads batched before queue submission |
| Async-friendly waits | JSPI suspends the wasm stack (`webgpu_wait.h`); Firefox can delay idle-queue callbacks by about 100 ms |

Not planned for now: raytracing, multiview, subpasses, VRS, SDFGI, VoxelGI.

## Layout

* `rendering_context_driver_webgpu.*` instance, adapter, surfaces (canvas selector in the browser, HWND natively).
* `rendering_device_driver_webgpu.*` the driver. Opaque IDs are pointers to the `*Info` structs in the header.
* `rendering_shader_container_webgpu.*` shader container. SPIR-V passes: strips `NonReadable` from storage buffers (WGSL has no
  write-only buffers) and flips Y of the vertex output like the D3D12 driver does. With `GODOT_TINT_PATH` set it converts
  SPIR-V to WGSL at bake time (browsers only accept WGSL).
* `webgpu_wait.h` how asynchronous WebGPU calls are waited for: polling on wgpu-native (no `wgpuInstanceWaitAny`), `WaitAny`
  with JSPI in the browser.
* `tools/` browser test harness, probe project, analysis scripts. `SHADER_COMPATIBILITY.md` measurement of the shaders.
* `tests/servers/rendering/test_webgpu_*.cpp` doctest cases (compute, parity, triangle, screen, convert).

Debug aids added to the engine: `GODOT_WEBGPU_DUMP_SPIRV=<dir>` (driver: dump what is handed to WebGPU),
`GODOT_DUMP_SPIRV_DIR=<dir>` (RenderingDevice: dump every SPIR-V module that is compiled), `GODOT_VULKAN_SPIRV_1_3=1`
(Vulkan driver compiles SPIR-V 1.3 like WebGPU would; Tint cannot read 1.4).

## CPU uploads

### Browser readback and shader startup

Synchronous `RenderingDevice::buffer_get_data` submits pending uploads, compute and the staging copy, then
waits for that staging buffer via `mapAsync`. WebGPU opts into this path through
`API_TRAIT_BUFFER_READBACK_WITHOUT_FRAME_STALL`; other drivers retain their explicit frame stall.
Recording resumes in the same frame without recycling resources or advancing staging allocations. No temporary
queue-wide fence is registered for each readback; the normal frame fence still protects the full frame.
This avoids Firefox's approximately 100 ms queue-completion notification delay for every mesh buffer.
It does not fix Firefox's notification delivery itself: in the v14 Firefox 158 workshop test, mapping callbacks
also reach 100 ms, tool readbacks still take 1.5-2.6 seconds and some selections retain the 20 FPS plateau.
No periodic callback pump or frame-fence removal is included. See WoodWorks PR #133, `docs/web_webgpu.md` section 11,
for the eight-switch measurements; v14's artifacts are built from engine commit `e60a1a9626`.
The mid-frame readback regression checks repeated unsynchronized uploads/dispatches, partial/full reads,
mesh-style vertex reads and continued recording before the ordinary submit/sync.

After requesting a buffer map, the browser driver submits an empty command list before waiting through JSPI.
Firefox can otherwise delay the map while the queue is idle and the suspended render loop cannot submit again.
GPU completion/readback semantics stay intact; no timer or background polling is required.
In a 15-second lathe drag at 1920×1080, Firefox 157 / RTX 4070 SUPER rendered 12–18 FPS before this change and
60 FPS with the driver fix. Average map completion fell from 79–93 ms to 4 ms. This is a local controlled drag,
not a claim about all carving workloads or the published deployment.

Single-thread browser builds retain WGSL and create shader modules, including their override-ID lists, when a
pipeline first needs the stage. Cached source remains available if modules are destroyed and recreated.
The local workshop created 241 modules instead of 1992. This reduces unused work, but an improvement in total
startup time has not yet been demonstrated; needed pipeline compilation and resource loading still take time.

The WebGPU driver keeps a CPU shadow for persistent buffers. `RenderingDevice::buffer_flush` accepts an optional
byte range; buffer updates, Forward+ instance lists (including shadow passes), and canvas batches pass the range they
actually filled. WebGPU uploads only that range, rounded to four-byte boundaries. Existing calls without a range still
upload the entire current frame region. The default driver implementation delegates range flushes to its existing
whole-allocation flush, preserving Vulkan's mapped-memory behavior.

Push constants take separate 256-byte slots immediately when recorded, including partial updates. The driver copies
those slots to CPU staging and sends one `queue.writeBuffer` before submission instead of one per draw. It resets the
slot index only after submission, preserving queue ordering when the ring is reused. The draw/dispatch test and the
dynamic-buffer range test check independent values and preservation of unflushed GPU bytes.

In WoodWorks Free Mode on Firefox 157.0.1 / Windows / RTX 4070 SUPER at 1920×1080, the v11 debug template measured
35.84 FPS and 393.96 MiB/s of uploads. The matching debug template/bake at `52dd92604d` measured 60.00 FPS and
32.13 MiB/s, with the same instrumentation and thirty-second sample. Uploads per frame fell from 10.98 to 0.535 MiB.
These are stationary workshop measurements; carving and other views need their own checks.

## Single-thread browser pipelines

Forward+ WebGPU in a `threads=no` web build skips speculative mesh/surface ubershader pipeline compilation.
On the first actual draw it waits for the specialized pipeline and collects the completed worker task immediately.
Without worker threads that task runs synchronously anyway; returning an empty RID unnecessarily requests the fallback
ubershader as well. Native/threaded builds keep the existing asynchronous compilation and fallback behavior.

Build native and web targets sequentially in one checkout: module registration files and enabled-module headers
are generated at shared paths. Simultaneous targets can leave the web registration object referencing native-only modules.
Regenerate for the intended target and rebuild its confirmed stale generated object after a collision.
Native shader bakes must use the same engine commit as the web template, because `ShaderRD` hashes the engine commit
into its cache path. Repinning a manifest version without rebuilding the shader bake is insufficient.

## Forward+ tangent frames

WebGPU uses bounded reciprocal-square-root normalization for the unpacked binormal, interpolated TBN vectors, and
normal-map result. This keeps a degenerate vector finite rather than propagating `normalize(0)` NaNs into lighting;
Vulkan retains its original normalization. In WoodWorks the chisel view previously had black static surfaces in both
native WGSL and Firefox. Disabling normal maps removed the defect; substituting a flat normal map did not.
With bounded normalization, the native capture keeps normal maps, baked lightmaps and shadows enabled and matches
the Vulkan reference with an average RGB difference of 0.57 byte values. The precise GPU vector that degenerates has
not been captured; the CPU sample contained valid tangents. See WoodWorks `docs/web_webgpu.md` for the controls.

## WGSL post-processing (browser builds)

`rendering_shader_container_webgpu.cpp` edits the WGSL that Tint wrote, before it is baked into the shader container:

* `var<immediate>` (push constants) becomes a uniform buffer at group 0, binding 900 (dynamic offset per draw),
* storage texture formats browsers lack are replaced by a wider one (the driver allocates the wider format),
* `_wgsl_matrix_fields_to_vectors`: every matrix field of every struct (3 or 4 rows) is declared as an array of its
  column vectors and rebuilt at each read. Firefox on Windows (wgpu, Direct3D 12) scrambles matrices that are struct
  fields (naga writes them as `row_major` HLSL matrices); Chrome does not. The memory layout is the same, so buffers
  need no change. A Python reference is `tools/web_dev/wgsl_matrix_fields.py` in the WoodWorks repository; check a change
  by running naga over a baked shader cache (`tools/web_dev/wgsl_check.py` there).

Releases of the browser build are tagged `godot-4.7.2-webgpu-vN`; see `.github/workflows/webgpu_release.yml`.

## Setting up a machine

Everything goes under `thirdparty_local/` of the Godot checkout (git-ignored).

1. wgpu-native (native driver): download `wgpu-windows-x86_64-msvc-release.zip` from gfx-rs/wgpu-native (v29.0.1.1 was used),
   unzip to `thirdparty_local/wgpu-native` (needs `include/webgpu/*.h` and `lib/wgpu_native.lib`, `lib/wgpu_native.dll`).
2. Tint: download `Dawn-<hash>-windows-latest-Release.tar.gz` from google/dawn releases (v20261006 was used), unpack to
   `thirdparty_local/dawn`; the binary is `bin/tint.exe`.
3. Emscripten: `git clone https://github.com/emscripten-core/emsdk`, `./emsdk install latest && ./emsdk activate latest`
   (6.0.11 was used; the port `emdawnwebgpu` is bundled), then `source emsdk_env.sh`.
4. Browser tests: `cd drivers/webgpu/tools/harness && npm install puppeteer-core` (uses the installed Chrome).

## Building

Desktop with tests (Windows, MSVC):

    scons platform=windows target=editor dev_build=yes webgpu=yes d3d12=no vulkan=yes tests=yes

Copy `thirdparty_local/wgpu-native/lib/wgpu_native.dll` next to the executable.

For an optimized Windows editor without debug symbols, use:

    scons platform=windows target=editor dev_build=no webgpu=yes d3d12=no vulkan=yes tests=yes optimize=speed debug_symbols=no -j6

`wgpu_native_path=<directory>` can point to an existing dependency installation outside this worktree.
The build links `lib/wgpu_native.dll.lib`; the matching DLL must accompany the executable.
If AccessKit or ANGLE dependencies are not installed, use `accesskit=no angle=no` to disable those optional drivers explicitly.

Run a project through the Windows display server in PowerShell:

    $env:GODOT_TINT_PATH = (Resolve-Path thirdparty_local/dawn/bin/tint.exe).Path
    & ./bin/godot.windows.editor.x86_64.console.exe --path C:/path/to/WoodWorks-Gd --rendering-driver webgpu

Tint enables the same SPIR-V-to-WGSL conversion and matrix-field rewrite used by the browser shader bake.
For browser-limit shader baking, also set `$env:GODOT_WEBGPU_BROWSER_LIMITS = '1'` before launching the scenes.
Use `--editor` to open the project editor, or `--rendering-driver vulkan` for the Vulkan reference.
The Windows display server passes the HWND and HINSTANCE to the WebGPU context separately from the Vulkan/D3D12 platform-data union.
Godot initializes `OS_Windows` with a null HINSTANCE, so WebGPU uses `GetModuleHandle(nullptr)` when needed.
Without a module handle, wgpu-native cannot create the Vulkan surface backing the swap chain.
Parallel Tint conversions use a process ID and atomic request counter in their temporary filenames;
using only the shader name lets stages/variants overwrite or lock each other's SPIR-V and WGSL files on Windows.

Verified on Windows with an RTX 4070 SUPER and WoodWorks PR #133 (`25e51a57`):
six GPU tests pass with native SPIR-V (1057 assertions), and with Tint plus browser limits (1079 assertions).
The `still-mat` scene renders 120 frames and all 80 sampled cells match the Vulkan reference.
The full exported project PCK loads the workshop and live SDF from a directory without project sources.
Unused SSIL/VoxelGI/SDFGI shader validation errors, LumAlpha8 conversion warnings, and test resource-leak warnings remain.

Browser template (single threaded, tests included):

    scons platform=web target=template_debug threads=no webgpu=yes tests=yes optimize=size debug_symbols=no

Unzip `bin/godot.web.template_debug.wasm32.nothreads.zip` to `thirdparty_local/webtpl`.

## Running the tests

Natively (`WOODWORKS_SHADERS_DIR` points to the project's `godot/shaders`):

    godot... --headless --test --test-case="*WebGPU*"

Bake the shader containers (WGSL through Tint) and the Vulkan reference outputs, then replay them natively:

    WEBGPU_BAKE_DIR=<dir> WOODWORKS_SHADERS_DIR=<shaders> GODOT_TINT_PATH=<tint> godot... --headless --test --test-case="*Bake*"
    WEBGPU_BAKED_DIR=<dir> godot... --headless --test --test-case="*Browser*"

In the browser (copy the bake output to `thirdparty_local/baked`):

    cd drivers/webgpu/tools/harness
    HEADLESS=0 TIMEOUT_MS=240000 node run.js ../../../../thirdparty_local/webtpl ../../../../thirdparty_local/baked "*Browser*"
    HEADLESS=0 KEEP_OPEN=1 node run.js ... "*Screen*"        # the triangle stays on the canvas

`SCREENSHOT=<file>` saves a screenshot. The page logs the adapter limits (useful for the texture array work below).

## Measuring shader compatibility

See "Reproducing" in `SHADER_COMPATIBILITY.md`; `tools/shader_probe` is a project that touches many Forward+ features, and
`tools/families.py` / `tools/analyze.py` summarize `--test-case="*Convert*"` logs.

## Next: texture arrays (what the measurement found)

WebGPU has no arrays of textures, and the scene shader declares two (plus SDFGI cascades in `environment/gi.glsl`):

* `forward_clustered/scene_forward_clustered_inc.glsl`, set 1: `lightmap_textures[MAX_LIGHTMAP_TEXTURES * 2]` (binding 7,
  `texture2DArray`: lightmaps, then shadowmasks) and `voxel_gi_textures[MAX_VOXEL_GI_INSTANCES]` (binding 8, `texture3D`).
* Uses: `scene_forward_clustered.glsl` (around lines 1846-1869 and 2314-2316, lightmaps), `scene_forward_gi_inc.glsl` (lines
  88 and 101, VoxelGI), `environment/gi.glsl` (SDFGI and VoxelGI pass).
* C++: `render_forward_clustered.cpp` builds those uniforms in two places (around lines 3484-3520 for the real sets, and
  3848-3870 for the "no lightmaps / no VoxelGI" fallback set); `MAX_LIGHTMAPS` and the `MAX_LIGHTMAP_TEXTURES` define are at
  about line 5129.

Plan: a `WEBGPU` shader define set from the driver name; lightmaps become individual bindings selected by a `switch` in
small wrapper functions (sampling already uses explicit LOD, so it is legal in non-uniform control flow); VoxelGI and SDFGI
are compiled out (the project does not use them). Check `maxSampledTexturesPerShaderStage` of the target browsers first
(the harness prints it), since 16 lightmap textures plus the existing bindings can exceed 16.

After the arrays, the scene shader is blocked by `textureSample`/`dpdx`/`dpdy` in non-uniform control flow (see the report),
then push constants in browsers, `memoryBarrier`, and the smaller items in the table.
