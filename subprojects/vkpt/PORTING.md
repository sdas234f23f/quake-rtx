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

- **S2a (done)**: shader toolchain validated for the Q2RTX set. All 46
  compilable files in `q2rtx-shaders/` build with `glslc
  --target-env=vulkan1.2 -DVKPT_SHADER` (Q2RTX compiles them the same way via
  glslangValidator, see `Q2RTX/cmake/compileShaders.cmake`). The FSR shaders
  additionally need the `fsr/` include path (vendored: `ffx_a.h`,
  `ffx_fsr1.h`, AMD MIT). `GenerateShadersQ2RTX.py` (our own tool) compiles
  the set into `Build/q2rtx/*.spv` with an mtime cache — validation only, the
  game does not load these shaders yet.
- **S2b (in progress)**: adopt the dual C++/GLSL headers as the binding
  contract and rework the C++ side to fill them:
  - `global_ubo.h` — flat `QVKUniformBuffer_t` of the `UBO_CVAR_LIST` cvars
    (std140, set = `GLOBAL_UBO_DESC_SET_IDX`, binding 0). C++ side fills it
    by iterating the same list (Q2RTX main.c does this). Done: `GlobalUniformQ2`.
  - Instance SSBO — binding 1 (`GLOBAL_INSTANCE_BUFFER_BINDING_IDX`) in the
    same buffer and descriptor set, offset
    `align(sizeof(QVKUniformBuffer_t), 256)`, like Q2RTX `uniform_buffer.c`.
    `InstanceBuffer`/`ModelInstance` layout verified by `check_q2rtx_ubo.py`
    (C == std140, 1736832 B / 192 B). The data is zeroed until the geometry
    port feeds real instances. Done: `GlobalUniformQ2`.
  - `global_textures.h` — `LIST_IMAGES`/`LIST_IMAGES_A_B` define every
    framebuffer image/texture (format + size) and the global texture array
    (`GLOBAL_TEXTURES_DESC_SET_IDX`, bindings offset by `BINDING_OFFSET_IMAGES`
    / `BINDING_OFFSET_TEXTURES`). Done: `FramebuffersQ2`.
  - `vertex_buffer.h`, `constants.h`, `shader_structs.h` — shared structs.
    Set 3 (`VERTEX_BUFFER_DESC_SET_IDX`): primitive array (binding 0,
    `VERTEX_BUFFER_FIRST_MODEL + MAX_MODELS` entries), position / light /
    light-counts-history / IQM / readback / tonemap / sun-color / light-stats
    bindings, layout mirroring Q2RTX `vertex_buffer.c`. All bindings point at
    a 4-byte null buffer until the geometry port feeds data. Done:
    `VertexBufferQ2`.
  This replaces `GenerateShaderCommon.py` output (`ShaderCommonC.h` etc.) and
  reworks `GlobalUniform.cpp`, `Framebuffers.cpp`, `TextureDescriptors.cpp`,
  `ShaderManager.cpp`, `VertexCollector*`.

## Stage S3 (shader swaps, in progress)

- Q2RTX `.spv` are compiled by `GenerateShadersQ2RTX.py` into
  `Build/q2rtx/` and packed into `shaders.pkz` under `shaders/q2rtx/...`
  (`zip_shaders.py` now recurses into subfolders).
- **First swap (done)**: `checkerboard_interleave.comp` runs every frame on
  the Q2 descriptor sets via `ShaderSwapQ2` (set 0 = `GlobalUniformQ2`,
  set 1 = `FramebuffersQ2`; no geometry). Output images (`IMG_FLAT_*`) are
  not displayed yet - the pass validates the Q2RTX shader pipeline and the
  UBO/image bindings end to end.
- **Bloom (done)**: `bloom_downscale.comp` + `bloom_blur.comp` (h + v, push
  constants per Q2RTX `compute_push_constants`) + `bloom_composite.comp`
  via `BloomQ2`. `FramebuffersQ2` sampled descriptors now declare GENERAL
  (Q2RTX keeps every framebuffer image in GENERAL for the whole frame);
  `GlobalUniformQ2` fills the bloom/taa UBO fields.
- **Compositing (done)**: `compositing.comp` (denoiser-disabled path) via
  `CompositingQ2` - combines the lighting channels with the surface
  parameters into `IMG_ASVGF_COLOR`.
- **ASVGF temporal (done)**: `asvgf_temporal.comp` via `AsvgfTemporalQ2` -
  temporal accumulation/filtering of the lighting channels into the history
  images and the atrous ping-pong buffers (15-pixel groups, no blue noise).
- **ASVGF gradient image (done)**: `asvgf_gradient_img.comp` via
  `AsvgfGradientImgQ2` - builds the low-res gradient image (GRAD_DWN res).
- **ASVGF LF filter (done)**: `asvgf_lf.comp` via `AsvgfLfQ2` - 4 LF wavelet
  iterations driven by an iteration push constant, ping-ponging the
  ATROUS_PING/PONG LF images.
- **Blue noise (done)**: `FramebuffersQ2` now has the
  `BINDING_OFFSET_BLUE_NOISE` binding, fed from the existing `BlueNoise`
  texture (Q2RTX-style 256x256 R16 x512 array). This unblocks the remaining
  set 0+1 shaders that sample `TEX_BLUE_NOISE` (asvgf_atrous, god_rays,
  tone_mapping_apply).
- **ASVGF a-trous (done)**: `asvgf_atrous.comp` via `AsvgfAtrousQ2` - 4
  spatial wavelet iterations, each a pipeline specialized on
  `spec_iteration` 0..3 (with `spec_enable_lf = 1`); the last iteration
  also composites into `IMG_ASVGF_COLOR`.
- **Tone mapping (done)**: `tone_mapping_histogram.comp` +
  `tone_mapping_curve.comp` + `tone_mapping_apply.comp` (SDR
  specialization) via `ToneMappingQ2`. First pass using all three Q2RTX
  descriptor sets; `VertexBufferQ2` gained a real `ToneMappingBuffer`
  (histogram accumulator + tone curve) for `TONE_MAPPING_BUFFER_BINDING_IDX`.
- **ASVGF TAA upscale (done)**: `asvgf_taau.comp` via `AsvgfTaaQ2` -
  temporal anti-aliasing + upscale (flat color/motion + previous TAA frame
  into `IMG_TAA_OUTPUT`). Uses all three Q2RTX sets; `VertexBufferQ2` gained
  a real `ReadbackBuffer` for `READBACK_BUFFER_BINDING_IDX` (written by the
  shader).
- **Sky buffer resolve (done)**: `sky_buffer_resolve.comp` via
  `SkyBufferResolveQ2` - converts the fixed-point sun/sky accumulation in
  `SunColorBuffer` into float values (1x1 dispatch, set 0 + set 1).
  `VertexBufferQ2` gained a real `SunColorBuffer` bound both as storage
  (`SUN_COLOR_BUFFER_BINDING_IDX`) and UBO (`SUN_COLOR_UBO_BINDING_IDX`).
- Then swap shader files one by one; each swap is a testable step.

## License notes

- Q2RTX code/shaders: GPL-2.0 (NVIDIA / Christoph Schied) — headers must stay.
- vkQuake base: GPL — the mix is license-clean.
- Files we write ourselves carry no third-party copyright.
