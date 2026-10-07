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
| `DisplayServerWeb` creating a WebGPU context instead of WebGL2 | **not done** |
| Windows display server using this driver (`--rendering-driver webgpu`) | **not done** |
| Built-in Forward+ shaders through Tint | **partial**: see [SHADER_COMPATIBILITY.md](SHADER_COMPATIBILITY.md) (11% as is, 50% without texture arrays) |
| Push constants in browsers | **not done** (browsers lack `immediates`; plan: emulate with a uniform buffer in the shader container) |
| Async-friendly waits | not done: waits suspend the wasm stack with JSPI (`webgpu_wait.h`) |

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
