# Godot's built-in Forward+ shaders on WebGPU

Result of converting the SPIR-V of every shader module that the Forward+ renderer compiles for a feature-rich scene
(Godot 4.7.2, SPIR-V 1.3 / Vulkan 1.1 semantics, Tint from Dawn v20261006 after the container's own passes: write-only
buffers, vertex Y flip): **181 of 920 modules convert, 739 do not.**

## Blockers, by number of modules

| Modules | Tint error | Where | What it takes |
|---|---|---|---|
| 624 | arrays of handle types are not supported | `voxel_gi_textures[]`, `lightmap_textures[]` (scene shader), SDFGI cascade arrays, `sdf_vec_textures[]` (particles) | WebGPU has no binding arrays. Replace the arrays with individual bindings plus a selecting `switch`, a single `texture2DArray`, or drop the feature on WebGPU. This is what stops every scene shader variant. |
| 24 | arrays cannot be used in the `<immediate>` address space | push constant blocks with array members (tonemap, DoF, SSS, particles copy, VoxelGI) | Emulate push constants with a uniform buffer in the container (needed for browsers anyway). |
| 23 | `textureSample`/barrier must only be called from uniform control flow | canvas (2D) shaders, blit, SMAA | Derivatives inside non-uniform branches: hoist the sample out of the branch or use `textureSampleLevel`. |
| 22 | unhandled storage class `Image` | texture atomics in the SDF render mode of the scene shader, volumetric fog | WebGPU has no image atomics; use buffers (only SDFGI and sdf-based features need it). |
| 16 | unhandled `OpMemoryBarrier` | copy, luminance reduce, SS effects downsample, sort, TAA | Map to `storageBarrier()`/`workgroupBarrier()` or drop the standalone barrier. |
| 3 + 1 | `IsInf` / `IsNan` unsupported | copy, motion vectors | Rewrite the checks with comparisons. |
| 3 | writable storage buffer in the vertex stage | VoxelGI debug | Mark the buffer `readonly`. |
| 2 | `gl_PointSize` store is not a constant | canvas | Constant point size. |
| 2 | `gl_HelperInvocation` | cluster render | No WGSL equivalent. |
| 2 | capabilities `Int16`, `StorageImageWriteWithoutFormat` | FSR upscale, SSR filter | Avoid 16-bit ints; give the written image a format qualifier. |

## Families that already convert

SSAO (`Ssao*`), SSIL (`Ssil*`), SSR downsample / hiz / resolve / main pass, sky (24/24), cluster store, octmap and
specular merge, SMAA weights, VRS, copy-to-framebuffer, shadow frustum, skeleton, canvas SDF and occlusion.

Not converting yet: the whole scene shader (`SceneForwardClusteredShaderRD`, 0 of 616), tonemap (4 of 8), canvas (8 of 30),
SSR filter, DoF, subsurface scattering, luminance reduce, TAA resolve.

## Reproducing

1. Dump the SPIR-V of a real Forward+ run (the Vulkan driver, but compiling SPIR-V 1.3 like WebGPU would):

       GODOT_VULKAN_SPIRV_1_3=1 GODOT_DUMP_SPIRV_DIR=<dir> godot --rendering-driver vulkan --path <project>

2. Convert every dumped module with the WebGPU container and Tint:

       WEBGPU_SPIRV_DIR=<dir> GODOT_TINT_PATH=<tint> godot --headless --test --test-case="*Convert*"

   The failing modules are listed as `CONVERT_FAIL <name>`; the Tint message precedes each of them.

## The WoodWorks project, whole set

Same measurement over the main menu, `viewer.tscn` (free and guided modes, bowl workpiece) and the probe scene, with the
shader cache disabled so that every module is compiled: **2034 modules**, of which `SceneForwardClusteredShaderRD` is
1714 (the engine's 3D shader plus every variant produced by the project's own spatial shaders).

| | modules | convert |
|---|---|---|
| as is | 2034 | about 11% (183 of 1637 in the first run) |
| texture arrays temporarily replaced by single textures (experiment, not committed) | 2034 | **1011 (50%)** |

Arrays of textures hide the next layer of problems, because Tint stops at the first error. With them out of the way the
scene shader (822 of 1714 convert) fails on:

| Variants | Tint error | Note |
|---|---|---|
| 278 | `textureSample` must be called from uniform control flow | sampling inside non-uniform branches |
| 104 | `dpdx` / `dpdy` (+3 `fwidth`) | same rule for derivatives |
| 24 | `subgroupBroadcastFirst` outside subgroup-uniform control flow | cluster light loop |
| 76 | texture atomics (`Image` storage class) | SDF render mode, SDFGI only |

So the scene shader's work is the uniform-control-flow rule plus the texture arrays; everything else is a short list
of small fixes.

Other findings:

* The three project compute shaders that come from imported `RDShaderFile` resources carry SPIR-V 1.4 when the editor imports
  them with the Vulkan driver, and Tint rejects them. They must be imported (or converted) as SPIR-V 1.3.
* Tint hits internal errors on non-finite constants (`ParticlesCopy`, 14 modules) and on a multisample resolve texture
  type (`Resolve`, 3 modules).
* Browsers do not enable Tint's `chromium_disable_uniformity_analysis` for pages, so the uniformity errors cannot be
  suppressed and have to be fixed in the shaders.
