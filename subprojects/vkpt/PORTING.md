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
  (113 files, ~37k lines, GPL-2.0). Reference only; nothing is edited there.

`C:\Users\f1am3d\repos\vkquake-rt` is an **earlier experimental fork**, kept only
for reference. Nothing is ported from it — the cluster / light-list groundwork it
pioneered is already committed in this repository (see `Quake/gl_rlight.c`,
`Quake/r_brush.c`, `Source/LightManager.cpp`).

See `ARCHITECTURE.md` for the structural map between the two engines: frame
lifecycle, data-structure correspondence, the descriptor-set contract, and the
invariants the Q2RTX shaders depend on.

## Runtime assets (blue noise)

The Q2RTX chain requires `blue_noise.pkz` (≈100 MB, 128 `HDR_RGBA_*.png` under
`blue_noise/256_256/`) at startup. Without it the device aborts with
`RG_ERROR_CANT_FIND_BLUE_NOISE` (a bare `abort()` under MSVC). It is a Q2RTX
asset and is **not** checked into this repository; copy it once from the Q2RTX
source tree:

```
Copy-Item C:\Users\f1am3d\repos\Q2RTX\baseq2\blue_noise.pkz .\id1\
```

`build_win.ps1` copies every `id1\*.pkz` into `<build>\id1\` (the mounted search
path) at build time, so the file survives clean builds. The smaller
`ovrd\BlueNoise_LDR_RGBA_128.ktx2` is a different (LDR) asset and does **not**
satisfy this requirement.

## Why shaders cannot be swapped in isolation

Every current shader was not only renamed but substantially adapted to the
RTGL binding conventions (generated UBO layout, framebuffer image indices,
descriptor-set numbering). Diff sizes between the renamed pairs are in the
hundreds of lines. The Q2RTX shaders use `global_ubo.h`, `global_textures.h`,
`shader_structs.h` and `vertex_buffer.h` — checked-in files whose layouts the
Q2RTX C++ code fills in.

Therefore the port has two axes, run in sequence:

- **S — shader/binding axis**: adopt the Q2RTX dual C/GLSL headers as the
  binding contract, then swap shaders one by one. Done (S1–S3).
- **G — geometry/lighting axis**: feed those bindings with real scene data
  until the Q2RTX chain is self-sufficient, then delete the legacy renderer.
  In progress.

## Stage S — binding layer and shader swaps (done)

- **S1**: vendor the Q2RTX shader set verbatim + the mapping table below.
- **S2a**: shader toolchain validated. All 46 compilable files in
  `q2rtx-shaders/` build with `glslc --target-env=vulkan1.2 -DVKPT_SHADER`
  (Q2RTX compiles them the same way via glslangValidator, see
  `Q2RTX/cmake/compileShaders.cmake`). The FSR shaders additionally need the
  `fsr/` include path (vendored: `ffx_a.h`, `ffx_fsr1.h`, AMD MIT).
  `GenerateShadersQ2RTX.py` (our own tool) compiles the set into
  `Build/q2rtx/*.spv` with an mtime cache.
- **S2b**: the dual C++/GLSL headers are the binding contract and the C++ side
  fills them:
  - `global_ubo.h` — flat `QVKUniformBuffer_t` of the `UBO_CVAR_LIST` cvars
    (std140, set = `GLOBAL_UBO_DESC_SET_IDX`, binding 0), filled by iterating
    the same list (Q2RTX main.c does this). Handled by `GlobalUniformQ2`.
  - Instance SSBO — binding 1 (`GLOBAL_INSTANCE_BUFFER_BINDING_IDX`) in the
    same buffer, offset `align(sizeof(QVKUniformBuffer_t), 256)`, like Q2RTX
    `uniform_buffer.c`. `InstanceBuffer`/`ModelInstance` layout verified by
    `check_q2rtx_ubo.py` (C == std140, 1736832 B / 192 B). Handled by
    `GlobalUniformQ2`.
  - `global_textures.h` — `LIST_IMAGES`/`LIST_IMAGES_A_B` define every
    framebuffer image/texture and the global texture array
    (`GLOBAL_TEXTURES_DESC_SET_IDX`, bindings offset by `BINDING_OFFSET_IMAGES`
    / `BINDING_OFFSET_TEXTURES`). Handled by `FramebuffersQ2`.
  - `vertex_buffer.h`, `constants.h`, `shader_structs.h` — set 3
    (`VERTEX_BUFFER_DESC_SET_IDX`): primitive array, position / light /
    light-counts-history / IQM / readback / tonemap / sun-color / light-stats
    bindings, mirroring Q2RTX `vertex_buffer.c`. Handled by `VertexBufferQ2`.
- **S3**: post-processing swapped onto the Q2RTX shaders — checkerboard
  interleave, bloom (downscale + blur h/v + composite), compositing, ASVGF
  (temporal, gradient image, LF, a-trous), tone mapping (histogram + curve +
  apply), TAA upscale, sky buffer resolve, blue noise. `BridgeQ2` (`rt_q2bridge`)
  copies the Q2RTX result into the legacy `FB_IMAGE_INDEX_FINAL` so it reaches
  the screen.

## Stage G — scene data (in progress)

Done:

- **G1**: `GeometryQ2` builds the Q2 world primitive buffer (`VboPrimitive` +
  BLAS source positions) from the legacy static geometry uploads.
- **G2**: `ASManagerQ2` builds BLAS + TLAS and fills the `InstanceBuffer`.
- **G3**: `PathTracerQ2` creates the Q2RTX ray tracing pipeline (9 shader
  groups, Q2RTX SBT layout) and traces primary rays into the G-buffer.
- **G4**: the on-screen source is the Q2RTX chain (`rt_q2bridge 1`).
- **G5**: `direct_lighting.rgen` on screen. Lit by a **placeholder sun**
  hard-coded in `GlobalUniformQ2.cpp` — Quake has no sun; this is scaffolding,
  removed in G6c.
- **G6a**: per-surface Q2 material table from the `.mat` system
  (roughness / metalness / specular / base factor), no textures yet.
- **G1b (implemented; visual validation pending)**: solid dynamic geometry
  (pickups/ammo/health, monsters, world and view weapons, moving brush models)
  is submitted through the Q2 ray tracing chain. See the dedicated section
  below for its deliberate omissions (sprites, particles, beams, dynamic
  emissive lights).

Frame today: primary rays → ASVGF → compositing → interleave → TAAU → bloom +
tone mapping → screen. The legacy renderer still renders its own full frame in
parallel; the bridge only displays the Q2 one.

### Remaining work, in priority order

The order below was revised after G6c. **Point lights come first** (done:
they are what lights the game today), then **textures**, because the
remaining half of the lighting work - light polys from emissive surfaces -
takes its colour from `texture_emissive` and cannot be done correctly
without them. Indirect lighting comes last, once there is direct light and
albedo worth bouncing.

#### G6c - lights (point lights done; emissive surfaces blocked on G6b)

Point lights work and are what currently lights the game. Spherical uploads
(dlights, entity lights, `world_custom_lights.txt`) are packed into
`DynLightData` and reach the shaders through `ubo.dyn_light_data` /
`num_dyn_lights`; `light_lists.h` samples them independently of the
per-cluster polygon lists. Verified on `start` (5 lights) and `e1m1`
(18 lights): correct path-traced lighting with soft shadows.

Still open in this stage:

- **Emissive surfaces produce no light polys, and the blocker is textures.**
  There are two independent "this surface emits" mechanisms in the tree:
  the `@POLY_LIGHT` list in `ovrd/texture_custom_info.txt` (10 curated
  textures with explicit hex colours, drives `rt_wldlights_tri`) and
  `is_light` in `ovrd.mat` (51 materials, drives `MATERIAL_FLAG_LIGHT` and
  `emissive_factor`). The curated list is far too narrow - `start` matches
  zero surfaces, `e1m1` matches exactly one, and that one is a button, i.e.
  a brush model rather than static geometry, so it never reaches the
  triangle array. Q2RTX derives light polys from the **material** flag
  (`bsp_mesh.c collect_light_polys`), so the port should follow `is_light`.
  But those materials take their colour from `texture_emissive`, so correct
  light polys need the texture port first. **G6b now comes before the rest
  of G6c.**
- `.mat is_light` static surfaces now join the existing polygonal-light path.
  The material synthesis records average linear emissive RGB while source
  pixels are available; `RT_FlushBatch` uses it to populate the atomic world
  triangle list. Dynamic emissive brush/model surfaces still produce no
  light polys (G1b renders them as solid geometry only; see the G1b section
  above) — that remains open, deliberately deferred alongside sprites,
  particles, and beams.
- ~~The 32 point lights were chosen arbitrarily.~~ Fixed: all spherical
  uploads are ranked against the current camera by Q2 luminance / distance
  squared, and only the best `MAX_LIGHT_SOURCES` reach the UBO. Selected
  entries retain upload order to keep stochastic sampler indices stable.
- **`pt_light_stats` is still 0** and the light stats buffers do not exist.
- **Spot lights are not mapped.** `RgSphericalLightUploadInfo.normal` marks a
  one-sided emitter but `DYNLIGHT_SPOT` needs real cone angles in
  `spot_data`; everything is uploaded as `DYNLIGHT_SPHERE` for now.
- ~~Temporary diagnostics~~ removed: `Q2LightLog`, `Q2G6Log`, the surface
  and custom-info counters in `r_world.c` and `gl_texmgr.c`, and the
  `q2crash.txt` mirror in `vkpt.cpp`. The widened `catch (std::exception)`
  in `vkpt.cpp` stays - a C API boundary must not let exceptions escape.

#### G6c leftovers (original notes)


The Q2 `LightBuffer` is currently empty: `light_polys`, `light_list_offsets`
and `light_list_lights` are zero-filled and `ubo.num_static_lights` /
`ubo.num_dyn_lights` are never assigned. This is a format conversion, not new
engine work — the structures already match one to one:

| Game side (already in this repo) | Q2RTX target |
|---|---|
| `RgPolygonalLightUploadInfo` = `{uniqueID, color, positions[3]}` | `LightPolygon` = `{mat3 positions, vec3 color, 2x style_scale}` |
| `RgSphericalLightUploadInfo` = `{color, position, radius, normal}` | `DynLightData` = `{center, radius, color, type, spot_direction, spot_data}` (normal != 0 gives a spot) |
| `RT_ClusterLightListsUpload` (prefix-sum offsets + concatenated ids, PVS via `Mod_LeafPVS`) | `light_list_offsets` + `light_list_lights` |
| `RgVertex.cluster` into `VboPrimitive.cluster` | `TEX_PT_CLUSTER_A`, read by `direct_lighting.rgen` |

Constants already line up: `RT_CLUSTER_MAX_CLUSTERS * RT_CLUSTER_MAX_PER_LIST`
= 8192 * 64 = 524288 = `MAX_LIGHT_LIST_NODES`.

Notes that decide the implementation:

- World lights must become **light polys, not dyn lights**: `MAX_LIGHT_SOURCES`
  is 32, which is right for Quake dlights (muzzle flashes, rockets) but
  hopeless for a whole map. The emissive world triangles live in
  `rt_wldlights_tri[]` (`Quake/r_world.c`, up to 2048, against
  `MAX_LIGHT_POLYS` 4096).
- `RT_USE_SPHERE_INSTEAD_OF_POLY` merges coplanar emissive triangles into
  sphere lights for the legacy sampler. Q2RTX samples the triangles directly
  (`spherical_tri_area`, Arvo 1995), so the Q2 path takes the raw triangles.
  Only the sphere branch currently calls `RT_ClusterLightAdd`, so cluster
  registration has to follow the triangles.
- `sample_polygonal_lights` reads the per-cluster light count from
  **`light_counts_history[frame % LIGHT_COUNT_HISTORY]`**, not from the offsets
  array. That buffer is currently the 4-byte null buffer, so it must be
  implemented or no light is ever sampled.
- `light_stats` (3 buffers, `num_clusters * num_light_polys * 6 * 2` uints,
  zeroed per frame) drives the adaptive sampler and is **written** by
  `direct_lighting.rgen`; it is also on the null buffer today. Size it with a
  cap — the Q2RTX formula is unbounded.
- Remove the placeholder sun once real lights land.

`light_lists.h` stays verbatim: it already implements sphere and spot lights
(`compute_dynlight_sphere` / `compute_dynlight_spot`), so Quake point lights
need no shader change.

#### G6b — textures

Point `GLOBAL_TEXTURES_TEX_ARR` at the image views the existing bindless
`TextureDescriptors` already maintains, instead of the white placeholder in
`FramebuffersQ2.cpp`, and write real texture indices into the material entries
(`entry[0]`/`entry[1]`) in `GeometryQ2.cpp`. Then drop the forced
`MATERIAL_KIND_REGULAR` and enable WATER / GLASS / LAVA kinds, and the emissive
factor that currently has no texture to modulate.

G6b is wired end to end. `FramebuffersQ2` owns one set per frame, copies the
initialized runtime-sized range from `TextureManager` after its descriptor
submission, and leaves the unused tail of Q2RTX's 8192-entry array on the white
fallback. `GeometryQ2` resolves each `RgMaterial` handle to bindless
albedo/RME/normal indices and generates tangents. The Q2 shader adapter reads
roughness/metalness/emission from legacy RME and alpha tests from albedo alpha;
this bridge is intentionally temporary while both renderers share the material
system.

#### G6f — water / liquid material kinds

Water, slime, and lava surfaces now carry the real Q2RTX material kind instead
of the forced `MATERIAL_KIND_REGULAR`:

- `GeometryQ2::AppendGeometry` maps the game's `RT_MAT_KIND_*` ordinal
  (`Quake/rt_material.h`) to the Q2RTX `MATERIAL_KIND_*` nibble via an explicit
  `MapMaterialKind` table. The two enums do **not** align numerically —
  `rt_material.h` omits `EXPLOSION`/`TRANSPARENT`, so `SCREEN` and `CAMERA`
  are shifted by two — so a shift of the enum value would be wrong.
- `r_world.c` overrides the resolved material kind from the surface flags for
  Quake 1 liquids, which are flagged by `SURF_DRAWWATER`/`SURF_DRAWSLIME`
  rather than a `.mat` kind: `is_water -> RT_MAT_KIND_WATER`,
  `is_acid -> RT_MAT_KIND_SLIME`. The surface flag wins even when a `.mat`
  exists, and it forces the `pQ2Material` upload when no `.mat` is defined.
- `GlobalUniformQ2` maps the legacy `cameraMediaType` (`MEDIA_TYPE_*`,
  `ShaderCommonC.h`) to the Q2RTX `global_ubo.medium` (`MEDIUM_*`,
  `constants.h`). The numbering differs (`MEDIA_TYPE_GLASS = 2` vs
  `MEDIUM_GLASS = 4`, `MEDIA_TYPE_ACID = 3` vs `MEDIUM_SLIME = 2`), so an
  explicit switch is used. This turns on primary-ray extinction underwater.

With the correct kind, the existing Q2RTX shader paths already wired in the
vendored `water.glsl` / `path_tracer_rgen.h` / `reflect_refract.rgen` take
over: animated water normals (`get_water_normal` + `global_ubo.time`),
refraction (`PT_REFRACT` SBT from `RG_GEOMETRY_PASS_THROUGH_TYPE_*_REFLECT_
REFRACT`), caustics, and underwater fog.

#### G6d — per-frame UBO correctness

Small but blocking for temporal quality; see the defect list below.

#### G6e — finish ASVGF

Add `asvgf_gradient_reproject` (trace command buffer, before lighting) and
`asvgf_gradient_atrous` (7 iterations, inside the filter). See the defect list.

#### G1b — dynamic geometry (implemented; visual validation pending)

Solid dynamic geometry (pickups/ammo/health, monsters, world weapons, the
first-person view weapon, and moving brush models — doors, plats, trains)
is now routed through the Q2 ray tracing chain. One world-space aggregate
buffer contains separate contiguous ranges for world, view-weapon, and
viewer-model geometry; each non-empty range gets its own BLAS/TLAS instance.
Sprites, particles, beams, and the effects TLAS remain explicitly deferred
(see below).

- **Collection**: `GeometryQ2::AddDynamicGeometry` receives every
  solid `RG_GEOMETRY_TYPE_DYNAMIC` upload (`r_alias.c`
  monsters/pickups/weapons, `r_world.c` moving brush surfaces, `r_brush.c`
  debug polys) and converts
  it with the exact same per-triangle pipeline as static geometry (shared
  `GeometryQ2::AppendGeometry` helper, extracted from the old
  `AddStaticGeometry` body) — octahedral normals, tangent/handedness, the same
  material-table dedup capped at `MAX_PBR_MATERIALS`. Vertices are transformed
  to world space on the CPU with `uploadInfo.transform`, so no per-entity BLAS
  or `ModelInstance` transform is required for visibility.
  - **Sky and sprites are filtered out**: sky needs its deferred material and
    `AS_FLAG_SKY` path; treating it as solid world geometry would block rays.
    Every geometry
    `uniqueID` encodes a stable "kind" in its top 4 bits
    (`Quake/gl_rmisc.c`: `RT_Get{BrushSurf,AliasModel,SpriteModel,CustomObject}UniqueId`
    use 1/2/3/4). `AddDynamicGeometry` drops kind 3 (sprite) uploads; particles
    and beams never call `rgUploadGeometry` at all, so no other game-side
    change was needed. This is Q2 GeometryQ2-local — the legacy renderer's
    sprite/particle/beam handling is untouched.
  - **View weapon**: uploads with `visibilityType ==
    RG_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON` get `MATERIAL_FLAG_WEAPON`
    OR'd into `material_id`, an existing flag the Q2RTX rgen shaders already
    special-case (checkerboard weapon rendering, brightness, handedness) —
    no shader change needed.
- **Motion vectors**: `GeometryQ2` keeps a `uniqueID -> previous world-space
  positions` map (`dynamicHistory`). Each frame, if an incoming upload's
  triangle count matches its previous frame's history, the per-vertex
  `previous - current` delta is packed into `VboPrimitive.custom0/1/2` with
  `PackHalf2x16` (matching `packHalf4x16(vec4(delta, 0))` in
  `vertex_buffer.h`); new or topology-changed geometry gets a zero delta.
  IDs that stop uploading (freed pickups, dead monsters, doors that finished
  moving) are pruned after each frame's submission so a reused `uniqueID`
  slot never inherits a stale delta.
- **Aggregation & upload**: all of one frame's dynamic triangles land in one
  `VboPrimitive` + BLAS-source-position buffer
  (`GeometryQ2::SubmitDynamic(frameIndex)`), uploaded to a host-visible
  coherent `Buffer` and bound at `PRIMITIVE_BUFFER_BINDING_IDX` array element
  `VERTEX_BUFFER_INSTANCED` (1) via `VertexBufferQ2::SetDynamicBufferInfo`.
  World, `FIRST_PERSON`, and `FIRST_PERSON_VIEWER` uploads occupy contiguous
  ranges whose primitive and position offsets are retained for AS construction
  and hit-shader addressing.
  The material table is re-uploaded to the GPU every frame it is non-empty
  (not just once at load), since dynamic uploads can introduce new material
  entries (monster/pickup skins never seen in the static level geometry).
- **Acceleration structures**: `ASManagerQ2` builds one dynamic BLAS for each
  non-empty range and a combined geometry TLAS with the static world instance
  plus those active category instances. All use `SBTO_MASKED`: materials
  without an alpha mask accept immediately, while alpha-tested albedo can
  discard. Dynamic instances use identity transforms and
  `instance_id = VERTEX_BUFFER_INSTANCED`; their exclusive masks are
  `AS_FLAG_OPAQUE`, `AS_FLAG_VIEWER_WEAPON`, and `AS_FLAG_VIEWER_MODELS`.
  Every active dynamic TLAS entry keeps
  `tlas_instance_model_indices[instance] = -1` and records its category's
  `primitiveOffset`, so `get_model_index_and_prim_offset` takes the world path
  while addressing the correct range in the aggregate primitive buffer.
- **Frame safety**: everything that must change every frame (`GeometryQ2`'s
  dynamic buffer, `ASManagerQ2`'s category BLAS / combined TLAS / TLAS instance
  buffer / descriptor set 0 / scratch allocator, and `VertexBufferQ2`'s
  descriptor set 3) is
  ring-buffered with `MAX_FRAMES_IN_FLIGHT` slots selected by `frameIndex`,
  mirroring `FramebuffersQ2`'s existing `descSets[MAX_FRAMES_IN_FLIGHT]` +
  `activeFrameIndex` + no-arg `GetDescSet()` pattern, so `PathTracerQ2` and the
  other `GetDescSet()` consumers need no changes. `SubmitDynamic` resets only
  the current frame slot's scratch allocator after `BeginFrame` has waited that
  slot's fence. A build-stage write-to-read barrier orders all BLAS builds
  before the combined TLAS build; the existing build-to-ray barrier remains
  after TLAS construction. `VulkanDevice::BeginFrame` calls
  `geometryQ2->BeginDynamicUpload()`; `UploadGeometry` dispatches
  `AddDynamicGeometry`/`AddStaticGeometry` by `geomType`; `DrawFrame` calls
  `geometryQ2->SubmitDynamic(frameIndex)`,
  `vertexBufferQ2->SetActiveFrame(frameIndex)`, then
  `asManagerQ2->SubmitDynamic(cmd, frameIndex)`. The latter updates the
  CPU-side `InstanceBuffer`, so `uniformQ2->Upload` follows it before the first
  `GetDescSet()` consumer of the frame (`SkyBufferResolveQ2`).
- **Explicitly deferred** (out of scope for this stage): dynamic sky, sprites,
  particles, and beams are not uploaded to the Q2 chain at all (no texel buffers were
  added; the effects TLAS remains the pre-existing never-hit placeholder
  instance built once in `ASManagerQ2::SubmitStatic`). Dynamic emissive
  brush/model lights (Q2RTX `instance_model_lights`) are not implemented —
  moving light-emitting brushes/models render as solid geometry only, with no
  light contribution beyond what static polygon lights already provide.

#### G7 — indirect lighting

`indirect_lighting.rgen` replacing the legacy `TraceQ2Indirect`.

#### Later swaps

`reflect_refract.rgen`, god rays / fog, physical sky, `animate_materials`,
`instance_geometry` / `normalize_normal_map`.

#### State A — remove the legacy renderer

`PathTracer`, `ASManager`, `Rasterizer`, `Source/Shaders/`, the `RG_*` API.

### Confirmed defects (fix alongside the stages above)

1. **The ASVGF gradient is identically zero — antilag is off.**
   `asvgf_gradient_img.comp` writes `GRAD_LF_PING` / `GRAD_HF_SPEC_PING`, but
   `asvgf_temporal.comp` reads the **PONG** images. Only
   `asvgf_gradient_atrous.comp` moves PING to PONG, and it is never dispatched,
   so the temporal filter always reads zeros. `asvgf_gradient_reproject.comp`
   is missing too, and it is what writes `ASVGF_GRAD_SMPL_POS_A` — read by both
   `path_tracer_rgen.h` and `asvgf_gradient_img.comp`. Invisible today (static
   scene, one fixed sun); it will show as ghosting the moment lights move.
2. **TAA history is invalidated every frame.** `GlobalUniformQ2.cpp` hard-sets
   `prev_taa_output_width/height` to 0 and aliases `invP_prev` to the current
   `invP`. Q2RTX carries both over from the previous frame (`main.c`
   `prepare_ubo`).
3. **Null-buffer bindings.** `LIGHT_STATS_BUFFER`, `LIGHT_COUNTS_HISTORY` and
   `IQM_MATRIX_BUFFER` point at a 4-byte buffer while `direct_lighting.rgen`
   writes light stats. Only `robustBufferAccess` is holding this together.

### Verified non-issues (do not chase)

- **LF / a-trous loop order.** `AsvgfLfQ2` runs all 4 LF iterations, then
  `AsvgfAtrousQ2` runs all 4 HF iterations; Q2RTX interleaves them per
  iteration. The results are identical: `asvgf_atrous.comp` reads the LF images
  only at `spec_iteration == 3` (compositing), and `asvgf_lf.comp` iteration 3
  leaves its result in exactly the PING images that iteration reads.
- **No separate compositing pass.** Correct — Q2RTX runs `compositing.comp`
  only when the denoiser is disabled; with `flt_enable` the last a-trous
  iteration composites into `IMG_ASVGF_COLOR`.
- **View-matrix convention.** The Q2RTX DoF branch in `primary_rays.rgen`
  assumes Q2RTX's view convention (view +Z forward), ours is the GL/Vulkan one
  (+Z backward), which is why `pt_aperture` is forced to 0. The exposure is
  contained: `invV[0..2]` appears nowhere else in the vendored set. Revisit
  only when porting DoF or god rays.

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
| `Q2LightLists.h` | `light_lists.h` | adapted (sphere/spot added to the Q2RTX sampler) |
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
- `ShadowMap.vert` maps to `shadow_map.vert` (renamed, small diff).
- `GenerateShaders.py` — ours (keep; extended later to build the Q2RTX set).
- `Build_test.spv` — stale artifact, delete.

## Debugging aids

- `rt_texinfo` — aim the crosshair at a surface and run this (bound to `t` by
  default in `default.cfg`; run `bind t rt_texinfo` once if your `config.cfg`
  predates it) to log what is underneath. Alias models report model name, skin,
  texture name, source file, `rtname`, emissive/light flags and material handle;
  the world reports the impact point and surface normal. Useful for flagging
  surfaces whose diffuse/emissive textures do not render.
- `rt_q2bridge 1` — show the Q2RTX chain instead of the legacy frame.
- `rt_q2debug N` - blit an intermediate Q2 image instead of the final one.
  Viewable (RGBA16F): 0 = TAA_OUTPUT (final), 1 = PT_BASE_COLOR_A (albedo),
  2 = ASVGF_COLOR (lighting after denoise), 3 = FLAT_COLOR,
  4 = ASVGF_TAA_A, 7 = ASVGF_TAA_B.
  **Not viewable**: 5 = PT_COLOR_HF and 6 = ASVGF_ATROUS_PING_HF are
  `R32_UINT` images holding packed values - blitting them to the RGBA8 final
  image yields meaningless colours. Judge lighting with 2 or 0.
  8 = PT_METALLIC_A is `R8G8` (red = metallic, green = roughness).

## License notes

- Q2RTX code/shaders: GPL-2.0 (NVIDIA / Christoph Schied) — headers must stay.
- vkQuake base: GPL — the mix is license-clean.
- Files we write ourselves carry no third-party copyright.
