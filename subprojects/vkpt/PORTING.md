# Porting plan: replace the vkpt renderer with Q2RTX

Goal: `subprojects/vkpt` must contain **only Q2RTX code and our own code**.
Process: module-by-module replacement (each step builds and runs).
End state: the `RG_*` library API is removed entirely.

## Where things live

- `Source/` — current renderer (RTGL-style library from the vkquake-rt work; MIT).
- `Source/Shaders/` — current shader set (adapted by the RTGL author from Q2RTX).
- `q2rtx-shaders/` — **verbatim** copy of `Q2RTX/src/refresh/vkpt/shader`
  (70 files, GPL-2.0, NVIDIA / Christoph Schied headers intact). Staged here so
  that `Source/Shaders/GenerateShaders.py` does not see it (it compiles only
  the files in `Source/Shaders/` itself).
- `C:\Users\f1am3d\repos\Q2RTX\src\refresh\vkpt` — the Q2RTX renderer source
  (113 files, ~37k lines, GPL-2.0).

## Why shaders cannot be swapped in isolation

Every current shader was not only renamed but substantially adapted to the
RTGL binding conventions (generated UBO layout, framebuffer image indices,
descriptor-set numbering). Diff sizes between the renamed pairs are in the
hundreds of lines. The Q2RTX shaders use `global_ubo.h`, `global_textures.h`,
`shader_structs.h` and `vertex_buffer.h` — checked-in files whose layouts the
Q2RTX C++ code fills in.

Therefore the shader swap has two stages:

- **S1 (done)**: vendor the Q2RTX shader set verbatim + this mapping document.
- **S2**: port the binding layer (uniform buffer layout, framebuffer indices,
  descriptor sets) to the Q2RTX conventions, then swap shaders one by one.

## Shader mapping table

Status legend:
- `adapted` — current file is an adaptation of the Q2RTX file (rename + binding changes)
- `renamed`  — same module, different name; diff shows real changes
- `ours`     — written by us for the quake-rtx integration, no Q2RTX equivalent
- `rtgl-only` — RTGL-specific, has no Q2RTX counterpart (dropped in the end state)

### Denoiser / ASVGF

| Current | Q2RTX | Status |
|---|---|---|
| `CmQ2Atrous.comp` | `asvgf_atrous.comp` | adapted |
| `CmQ2AtrousLF.comp` | `asvgf_lf.comp` | adapted |
| `CmQ2Temporal.comp` | `asvgf_temporal.comp` | adapted |
| `CmQ2TAAU.comp` | `asvgf_taau.comp` | adapted |
| `CmQ2GradientAtrous.comp` | `asvgf_gradient_atrous.comp` | adapted |
| `CmQ2GradientImg.comp` | `asvgf_gradient_img.comp` | adapted |
| `CmQ2GradientReproject.comp` | `asvgf_gradient_reproject.comp` | adapted |
| `CmQ2Interleave.comp` | `checkerboard_interleave.comp` | adapted |
| `CmCheckerboard.comp` | (part of `checkerboard_interleave.comp`) | rtgl-only |
| `Q2Asvgf.h` | `asvgf.glsl` | adapted |
| `SphericalHarmonics.h` | (inside `asvgf.glsl`) | rtgl-only |

### Ray tracing stages

| Current | Q2RTX | Status |
|---|---|---|
| `RtRaygenPrimary.rgen` | `primary_rays.rgen` | adapted |
| `RtRaygenDirect.rgen` | `direct_lighting.rgen` | ours (Q2RTX-style port) |
| `RtQ2Indirect.rgen` | `indirect_lighting.rgen` | ours (Q2RTX-style port) |
| `RtQ2ReflectRefract.rgen` | `reflect_refract.rgen` | ours (Q2RTX-style port) |
| `RtClsOpaque.rchit` | `path_tracer.rchit` | adapted |
| `RtAlphaTest.rahit` | `path_tracer_masked.rahit` | adapted |
| `RtMiss.rmiss` | `path_tracer.rmiss` | adapted |
| `RtMissShadowCheck.rmiss` | (inside `path_tracer.rmiss`) | rtgl-only |
| `HitInfo.inl` | `path_tracer_hit_shaders.h` | adapted |
| `RaygenCommon.h` | `path_tracer_rgen.h` | adapted |
| `RaygenPrimary.inl` | (inside `path_tracer_rgen.h`) | adapted |
| `Light.h` | `light_lists.h` | adapted |
| `Q2LightLists.h` | `light_lists.h` | adapted |
| `Surface.inl` | `path_tracer_transparency.glsl` | adapted |
| — | `path_tracer_particle.rahit` | new (Q2RTX) |
| — | `path_tracer_beam.rahit` / `path_tracer_beam.rint` | new (Q2RTX) |
| — | `path_tracer_explosion.rahit` / `path_tracer_sprite.rahit` | new (Q2RTX) |
| — | `path_tracer.h` | new (Q2RTX) |

### Shared GLSL helpers

| Current | Q2RTX | Status |
|---|---|---|
| `BRDF.h` | `brdf.glsl` | adapted |
| `Utils.h` | `utils.glsl` | adapted |
| `Structs.h` | `shader_structs.h` | adapted |
| `Random.h`, `RayCone.h`, `Media.h`, `Exposure.h` | (split across `utils.glsl`) | rtgl-only |
| `TonemappingUtils.glsl` | `tone_mapping_utils.glsl` | renamed (small diff) |
| `ShaderCommonGLSLFunc.h` | (generated from `shader_structs.h`) | rtgl-only |
| `VertexData.inl`, `VertexPreprocessPartial.inl` | `vertex_buffer.h` | adapted |
| `Volumetric.h`, `Q2Fog.h` | `god_rays_shared.h` | adapted |
| — | `constants.h`, `projection.glsl`, `sky.h`, `water.glsl`, `precomputed_sky.glsl`, `precomputed_sky_params.h`, `tiny_encryption_algorithm.h` | new (Q2RTX) |

### Post-processing

| Current | Q2RTX | Status |
|---|---|---|
| `CmBloomApply.comp` | `bloom_composite.comp` | renamed (small diff) |
| `CmBloomDownsample.comp` | `bloom_downscale.comp` | adapted |
| `CmBloomUpsample.comp` | `bloom_blur.comp` | adapted |
| `CmPrepareFinal.comp` | `tone_mapping_apply.comp` + `final_blit.*` | adapted |
| `CmLuminanceHistogram.comp` | `tone_mapping_histogram.comp` | adapted |
| `CmLuminanceAvg.comp` | `tone_mapping_curve.comp` | adapted |
| `CmCas.comp` | `fsr_rcas_fp16.comp` / `fsr_rcas_fp32.comp` | adapted |
| `CmCullLensFlares.comp` | — | ours (keep until effects ported) |
| `CmVolumetricProcess.comp` | `god_rays.comp` / `god_rays_filter.comp` | adapted |
| `CmGodRays.comp` | `god_rays.comp` | adapted |
| `CmGodRaysFilter.comp` | `god_rays_filter.comp` | adapted |
| `Ef*.comp` / `EfCommon.inl` / `EfSimple.inl` (12 files) | (done inside `compositing.comp`) | ours (keep until effects ported) |
| — | `compositing.comp`, `animate_materials.comp`, `normalize_normal_map.comp`, `instance_geometry.comp`, `stretch_pic.*`, `debug_line.*`, `sky_buffer_resolve.comp` | new (Q2RTX) |

### Sky

| Current | Q2RTX | Status |
|---|---|---|
| `CmProceduralSky.comp` | `physical_sky.comp` | adapted |
| — | `physical_sky_space.comp` | new (Q2RTX) |

### Raster passes (RTGL-only; dropped in the end state)

| Current | Q2RTX |
|---|---|
| `RsWorld.frag`, `RsSky.frag`, `RsSwapchain.frag`, `RsDecal.*`, `RsDepthCopying.frag`, `RsFullscreenQuad.vert`, `RsRasterizer.vert`, `RsRasterizerLensFlare.*`, `RsRasterizerMultiview.vert` | none (Q2RTX has no rasterized-world pipeline) |

### Other

- `CmVertexPreprocess.comp` — RTGL geometry preprocessing; Q2RTX does this in
  `instance_geometry.comp` / `normalize_normal_map.comp`.
- `ShadowMap.vert` ↔ `shadow_map.vert` (renamed, small diff).
- `GenerateShaders.py` — ours (keep; extended later to build the Q2RTX set).
- `Build_test.spv` — stale artifact, delete.

## Stage S2 outline (binding layer)

1. Adopt Q2RTX's checked-in headers as the single source of truth:
   `global_ubo.h`, `global_textures.h`, `shader_structs.h`, `vertex_buffer.h`,
   `constants.h`, `light_lists.h`.
2. Align the C++ side (`GlobalUniform.cpp`, `Framebuffers.cpp`,
   `ShaderManager.cpp`, `VertexCollector*`) with those layouts.
3. Replace `GenerateShaderCommon.py` generation with the Q2RTX headers where
   possible.
4. Then swap shader files one by one; each swap is a testable step.

## License notes

- Q2RTX code/shaders: GPL-2.0 (NVIDIA / Christoph Schied) — headers must stay.
- vkQuake base: GPL — the mix is license-clean.
- Files we write ourselves carry no third-party copyright.
