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

## Raw shadow depth reads

WebGPU forbids a `texture_depth_2d` from being statically sampled with a `filtering` sampler binding.
Chrome/Dawn rejects the whole pipeline even when the corresponding soft-shadow or transmission branch is
inactive. Forward+ uses the shared `SAMPLER_LINEAR_CLAMP` for both color textures and raw shadow-atlas depth;
changing its layout to non-filtering would also disable color filtering and require different sampler objects.

`shadow_depth_inc.glsl` instead implements linear-clamp raw depth reads with four clamped `texelFetch` loads and
bilinear interpolation under `WEBGPU`. Tint emits `textureLoad`, which has no texture/sampler pairing. Blocker
searches, directional transmission and volumetric-fog shadow fades use this helper. Comparison shadow sampling
continues to use the comparison sampler, and other backends retain the hardware LOD-zero linear sample.
Rebake shader containers when changing this helper; replacing only the web template leaves old WGSL in the PCK.

## Small geometry buffers

The driver suballocates GPU-only vertex/index buffers up to 64 KiB from 4 MiB
pages with matching usage flags. Storage, uniform, indirect, CPU-mapped,
persistently dynamic and larger buffers retain individual allocations. Logical
buffer sizes and IDs remain independent; draw bindings, clears, texture transfers
and readback copies apply each buffer's page offset and retain logical bounds.
Same-page buffer copies use a temporary copy buffer because
[WebGPU prohibits the same GPUBuffer as copy source and destination](https://www.w3.org/TR/webgpu/#dom-gpucommandencoder-copybuffertobuffer).

RD disposal already requires the final resource use to have been submitted.
Freed ranges in a still-live page are quarantined against that submission serial
and become reusable only when the existing real queue-completion tracker retires
them. Reuse restores zero initialization. Allocations, disposal and submission
serials share the submission mutex; no per-allocation/per-frame wait or new
completion callback is introduced. Empty pages are destroyed as ordinary WebGPU
resources, whose submitted uses remain retained by the implementation.

Local GPU regressions cover partial same-page copies, clears, neighboring vertex
and index contents, actual-completion gating, zero initialization on reuse and
page release. The existing queued snapshot, independent readback, dynamic-buffer
and draw tests also exercise the changed bindings/copy paths. These tests remain
local validation; the release workflow builds artifacts without GPU profiling.

## CPU uploads

v17 adds `API_TRAIT_TRANSFER_WORKER_RESOURCES_RELEASE_ON_SUBMIT`, enabled only by WebGPU. Its upload workers
recycle CPU staging offsets and create fresh encoders after actual queue submission, without registering a
per-upload completion fence. CPU shadows have already been copied by queue writes; later writes to reused GPU
staging storage are ordered after the preceding copy submission on the same queue. Offsets remain distinct
while a worker is still recording. Worker dependencies compare submitted operations for this trait, without
reporting GPU completion early. Vulkan and other drivers retain the previous completion/barrier path.

Initial texture uploads use the same rule, including immediate-flush loads. Explicit sync still waits on a
real draw fence. Finalization drains a real queue-tail fence after submitting any remaining uploads, including
uploads made after the last draw fence. No per-upload callback storage is recycled while callbacks are pending;
the existing bounded submission-progress tracker remains responsible for queue backpressure.

This addresses the 4K WoodWorks cutting stall from changing Line2D indicator polygons: the old upload-worker
reuse path synchronously waited on queue notifications. Local GPU regressions check 144 immutable buffer
snapshots across 48 frame cycles, staging growth/reuse/source disposal, 24 initial 3D texture uploads, absence
of upload fence waits and preservation of explicit synchronization. Native selected WebGPU tests pass 2913
assertions in 12 cases. Firefox validation is a separate local step; CI continues building artifacts only.

### Browser readback and shader startup

The v15 path makes `RenderingDevice::buffer_get_data_async` independent of frame fences. Each request owns a
dedicated staging buffer; `mapAsync` is registered only after its graph copy is submitted. Browser callbacks only
publish a completion flag. RenderingDevice consumes ready results on its render thread without waiting, then frees
the staging buffer and invokes the callable. Pending requests are bounded to 256 / 64 MiB and return `ERR_BUSY`
on overload. Explicit sync/finalize drains real completions before destroying callback state.

`API_TRAIT_FRAME_RESOURCES_RELEASE_ON_SUBMIT` applies only to WebGPU: CPU upload shadows are copied by queue writes,
writes are ordered between submissions, and submitted commands retain GPU objects. A resource destroyed after its
last submission is no longer available to new commands, while the implementation keeps prior submitted uses alive.
Thus CPU frame bookkeeping can cycle without a GPU-completion notification per frame. Dedicated readback buffers
are excluded from that recycling until mapped data have actually been consumed. Explicit submit/sync, shutdown and
legacy asynchronous texture downloads still use real fences; Vulkan and other drivers retain their existing paths.

The driver bounds outstanding command submissions to 32. It coalesces one actual work-done marker for batches of
at least eight submissions, continues rendering while that marker is pending, and waits only when the budget is
exhausted. No fence is reported complete early and no periodic empty-submit timer is added. This bounds queued
resources while avoiding the two-frames-per-delayed-notification bottleneck.

v16 follows the independent code review: a submission mutex serializes native render/loader queue submissions and
their shared progress marker state. Work-done callbacks publish through `SafeFlag` with acquire/release semantics,
including explicit fence completion. This prevents concurrent transfer-worker submissions from overwriting a pending
future or misattributing an older callback to a newer marker. Browser rendering remains single-threaded; the ordinary
frame path and batching budgets are unchanged. The concurrent-submission regression runs two native threads through
96 submissions each and then checks an actual completion fence.

`RenderingServer.mesh_surface_get_arrays_async(mesh, surface, callback)` snapshots surface metadata and schedules
the vertex/attribute/skin/index reads together. It decodes the same arrays as the synchronous API, then defers the
user callback to the main thread. Invalid surfaces/readback errors produce an empty array. It excludes LOD and blend
shape arrays. RenderingDevice renderers use asynchronous downloads; the fallback for other renderers is synchronous
data retrieval followed by deferred delivery. The WoodWorks tool retains its model while preparation is pending,
rejects stale selection callbacks and caches immutable geometry for repeat selection.

Native validation passes 2424 assertions across 15 selected GPU cases, including the concurrent submission test and 48 ordinary frame cycles with
independent partial readbacks, buffer overwrites and source disposal before all callbacks arrive. Tool pose/cache
and all eight blade-clearance checks pass, including rapid replacement while requests are outstanding. Browser
performance validation is required separately; no Firefox FPS improvement is claimed from these native results.

The preceding v14 change remains available for synchronous callers:
Synchronous `RenderingDevice::buffer_get_data` submits pending uploads, compute and the staging copy, then
waits for that staging buffer via `mapAsync`. WebGPU opts into this path through
`API_TRAIT_BUFFER_READBACK_WITHOUT_FRAME_STALL`; other drivers retain their explicit frame stall.
Recording resumes in the same frame without recycling resources or advancing staging allocations. No temporary
queue-wide fence is registered for each readback. The v14 release retained a normal fence for each full frame.
It does not fix Firefox's notification delivery itself: in the v14 Firefox 158 workshop test, mapping callbacks
also reach 100 ms, tool readbacks still take 1.5-2.6 seconds and some selections retain the 20 FPS plateau.
See WoodWorks PR #133, `docs/web_webgpu.md` section 11,
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

## Explicit scene preparation (v18)

Single-threaded browser Forward+ can opt into
`rendering/driver/webgpu/precompile_scene_pipelines.web=true`. Until the first
`RenderingServer.prepare_scene_pipelines()` request, scene updates and 2D loading UI continue,
but 3D surface draws and surface pipeline preparation are deferred. Add/configure the scene's
instances, material overrides, lighting and hidden future tools, allow instance/lightmap pairing
to settle, then request preparation under the loading overlay and await `frame_post_draw`.
The request is idempotent; later dirty instances prepare automatically in a rendered viewport's
context. It is not an enumeration of resources in a folder or a general asynchronous scene loader.

Preparation uses instantiated surfaces rather than speculative resource-only mesh combinations.
It retains the actual instance lightmap choice, while reflection/multiview contexts keep the
conservative desktop combinations. Opaque surfaces keep a stable velocity attachment with TAA
either enabled or disabled; transparent passes omit velocity writes. The viewport motion-vector
count also stays enabled so previous-frame vertex/skin data remain valid. This increases potential
render-target memory/bandwidth. Avoid enabling the option without a client preparation request.

The browser driver creates the actual render pipeline with `wgpuDeviceCreateRenderPipelineAsync`.
Its existing JSPI future wait suspends the loading stack until readiness, instead of treating an
immediate synchronous handle or an unrelated queue acknowledgment as compiler completion.
Gameplay continues using these universal pipelines, without requesting specialized pipelines on
Firefox's shared canvas/compositor path. New materials, vertex layouts or render features can
still need new pipelines; retain the prepared instances and validate the intended transitions.
Native Vulkan/WebGPU and the default-disabled browser mode retain their existing behavior.

Local Firefox 158 / RTX 4070 SUPER / Windows, controlled 1920x1080 canvas and diagnostic
frame-rate override 160: preparation 39.337 s, 171 async plus six synchronous render pipelines,
275 shader modules. Three 18-second camera orbits created no pipelines or modules, measuring
77.66/77.75/87.48 FPS. All eight authored WoodWorks tools selected successfully; first half-round
selection created six pipelines from already prepared shaders in about 11 ms total, with a
34.24 ms maximum frame gap. This is narrower than the rejected 97.494 s / 264-pipeline broad
preparation prototype, but slower than the previous 25.974 s / 96-pipeline camera-view warmup,
which covered fewer tool states. It is not a startup improvement over that published build.
The same 14 pre-existing unsupported/unused-feature startup errors remain; orbit/tool phases
had no device loss or new validation errors. These scheduling measurements are not maximum
foreground throughput. Full results and local reproductions live in WoodWorks `docs/web_webgpu.md`.

The matching 3840x2160 run prepares in 39.288 s and measures 58.60/58.24/61.60 FPS during
the same orbits, again without new pipelines/modules. All eight tool selections measure
65.20–65.52 FPS over their five-second windows, with maximum gaps 31.34–39.10 ms. The first
half-round tool creates six cached-shader pipelines in 11.30 ms total. The lathe-to-chisel
station transition creates no pipelines but still has a 175.48 ms frame gap; this is a remaining
non-compilation hitch, not evidence that every interaction is free of stalls.

The optimized Windows editor build, native hidden-instance/material-override fixture and
official Godot 4.7.2 Vulkan reset/cut probe pass. Existing unused-feature diagnostics and fixture
shader RID leak warnings remain. Release CI still only builds and publishes artifacts;
shader baking belongs to the consuming project's build, and browser profiling stays local.

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

### Particle bindings in combined Vulkan/WebGPU builds

Particle vector-field/collision texture binding counts follow the active device
family. WebGPU binds one fallback texture; Vulkan retains the seven-element array
its particle shader declares, even when the binary was built with `webgpu=yes`.
A compile-time `WEBGPU_ENABLED` check alone applies the browser layout to Vulkan
and produces rejected particle compute passes during completion/reset effects.

### Anonymous buffers beside combined samplers

Tint splits a combined texture/sampler and can shift later resource bindings to
resolve conflicts. Anonymous GLSL blocks have no reflected variable name, so the
name-based WGSL fixup cannot restore their slots. A readonly input can then occupy
the output slot and invalidate a compute pipeline. Pass explicit sampler mappings
to Tint before its conflict resolution; the existing WGSL fixup places the split
sampler at the engine's offset while anonymous storage buffers keep their slots.
Container format 12 invalidates stale baked WGSL.

The local `[WebGPU][Compute] Combined sampler preserves anonymous storage bindings`
regression requires `GODOT_TINT_PATH`. It checks read/write reflection, WGSL binding
slots and actual GPU results. It fails before the fix, and both WoodWorks brick
classifiers also execute with the corrected container. Unsupported unused built-in
shader diagnostics in a full native WebGPU renderer remain separate.
