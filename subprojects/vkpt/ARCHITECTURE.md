# Architecture map: Q2RTX to quake-rtx

One place that says what corresponds to what. Until this file existed the
correspondence lived only in scattered code comments, which is why a failure
like "no light reaches the screen" had to be bisected with printf instead of
being read off a table.

Companion to `PORTING.md`: that file tracks *what is done and what is next*,
this one tracks *how the two engines line up*. Keep them in sync.

Reference source: `C:\Users\f1am3d\repos\Q2RTX\src\refresh\vkpt` (read-only).

---

## 1. Frame lifecycle

### Q2RTX

One `R_RenderFrame_RTX` builds the whole frame and submits it in four command
buffers (`main.c`):

| Phase | What happens |
|---|---|
| transfer cmd buf | `vkpt_light_buffer_upload_staging`, `vkpt_iqm_matrix_buffer_upload_staging` |
| trace cmd buf #1 | `update_transparency`, UBO copy from staging, `physical_sky`, `instance_geometry`, `pt_create_all_dynamic` + `pt_create_toplevel` (BLAS/TLAS), `shadow_map_render`, `pt_trace_primary_rays` |
| trace cmd buf #2 | god rays, `pt_trace_reflections`, `asvgf_gradient_reproject`, `pt_trace_lighting` |
| post cmd buf | `asvgf_filter` (or `compositing`), `interleave`, `taa`, `bloom`, `tone_mapping`, FSR, readback copy |

Scene data is pushed into Q2RTX by the game *before* the frame starts, and the
BSP-derived data is built once at map load, not per frame.

### quake-rtx

The frame is bracketed by the two RG_* entry points, with the game's rendering
in between:

| Phase | Call chain |
|---|---|
| frame open | `GL_BeginRendering` -> `RT_GL_BeginRenderingTask` -> `rgStartFrame` -> `VulkanDevice::BeginFrame` |
| scene upload | `R_RenderScene`: geometry, then `RT_UploadAllWorldModelLights`, then `RT_ClusterLightListsUpload` |
| frame close | `GL_EndRendering` -> `RT_GL_EndRenderingTask` -> `rgDrawFrame` -> `VulkanDevice::DrawFrame` |

`VulkanDevice::DrawFrame` records the Q2 chain into a single command buffer:
per-frame dynamic geometry finalization (G1b — `GeometryQ2::SubmitDynamic`,
`ASManagerQ2::SubmitDynamic`, see `PORTING.md` and section 5's "Dynamic
geometry" invariants), sky buffer resolve, primary rays, reflection/refraction
rays, direct lighting, ASVGF (gradient image, temporal, LF, a-trous),
checkerboard interleave, TAA upscale, then `BridgeQ2` (bloom + tone mapping +
blit into the legacy final image).

**Key structural difference:** Q2RTX builds its scene data once per map; we
rebuild most of it every frame from RG_* upload calls. See section 4.

---

## 2. Data structures

| Q2RTX | quake-rtx | Built where |
|---|---|---|
| `bsp_mesh_t` (whole BSP mesh) | *no equivalent* — replaced by per-frame uploads | — |
| `VboPrimitive` (world primitive array) | same struct, vendored | `GeometryQ2::AppendGeometry` (shared by static + dynamic) |
| BLAS source positions | same layout, tail of the same buffer | `GeometryQ2::UploadToDevice` (static), `GeometryQ2::SubmitDynamic` (dynamic, ring-buffered) |
| *no equivalent (per-instance ModelInstance)* | dynamic aggregate `VboPrimitive`/position buffer at `VERTEX_BUFFER_INSTANCED`, split into contiguous world/view-weapon/viewer-model ranges and rebuilt every frame | `GeometryQ2::AddDynamicGeometry`/`SubmitDynamic` (G1b) |
| `bsp_mesh->clusters` (per-primitive cluster) | `VboPrimitive.cluster`, fed from `RgVertex.cluster` | `r_brush.c` `rt_surfcluster[]` |
| `light_poly_t` / `LightPolygon` | `RgPolygonalLightUploadInfo` -> `light_polys` | `r_world.c` `rt_wldlights_tri[]`, packed by `LightManagerQ2` |
| `cluster_light_offsets` / `cluster_lights` | `light_list_offsets` / `light_list_lights` | `gl_rlight.c` `RT_ClusterLightListsUpload` (PVS), resolved by `LightManagerQ2` |
| `buf_light_counts_history[3]` | same, 3 buffers | `VertexBufferQ2::SetLightCounts` |
| `buf_light_stats[3]` | *not implemented* — `pt_light_stats` forced to 0 | — |
| `DynLightData` (sphere; spot pending) | `RgSphericalLightUploadInfo` -> best `MAX_LIGHT_SOURCES` by luminance / distance squared | `LightManagerQ2::Submit` -> `GlobalUniformQ2` |
| `QVKUniformBuffer_t` | same struct, vendored | `GlobalUniformQ2::Upload` |
| `InstanceBuffer` / `ModelInstance` | same structs, vendored; the static world and every active G1b category instance use `tlas_instance_model_indices == -1`, with per-range primitive offsets instead of per-entity `ModelInstance` entries | `ASManagerQ2` |
| `MAX_RIMAGES` texture array | `GLOBAL_TEXTURES_TEX_ARR` — currently all white | `FramebuffersQ2` (G6b will point it at `TextureDescriptors`) |
| `material_table` (PBR materials) | same layout; legacy RME index occupies the emissive slot during the dual-renderer bridge | `GeometryQ2` from `RgMaterial` + `.mat` factors |
| framebuffer images (`LIST_IMAGES`) | same names/formats | `FramebuffersQ2` |

Q2RTX ping-pongs the `_A`/`_B` images by swapping descriptor sets each frame
(`qvk_get_current_desc_set_textures`). `FramebuffersQ2` uses per-frame
descriptor sets for safe bindless texture updates, but `IMG_*_A` and
`TEX_*_A` still resolve to the same image in both sets.
Reads and writes stay consistent within a frame, but there is no cross-frame
ping-pong; anything that genuinely needs last frame's image will need the swap.

---

## 3. Descriptor set contract

Identical to Q2RTX; the set indices come from the vendored `constants.h`.

| Set | Q2RTX name | Owner here | Contents |
|---|---|---|---|
| 0 | `RAY_GEN_DESC_SET_IDX` | `ASManagerQ2` | TLAS + texel buffers; **G1b**: one descriptor set per `MAX_FRAMES_IN_FLIGHT` slot (`descSets[frameIndex]`), because the geometry TLAS entry (static world + active dynamic category instances) is rebuilt every frame — the effects TLAS entry and texel buffer placeholders are written into every ring slot once at load and never change |
| 1 | `GLOBAL_UBO_DESC_SET_IDX` | `GlobalUniformQ2` | binding 0 UBO, binding 1 instance SSBO |
| 2 | `GLOBAL_TEXTURES_DESC_SET_IDX` | `FramebuffersQ2` | per-frame sets: bindless `TextureManager` range, framebuffer images/textures, blue noise |
| 3 | `VERTEX_BUFFER_DESC_SET_IDX` | `VertexBufferQ2` | primitives, positions, light buffer, light counts history, IQM, readback, tone mapping, sun color, light stats; **G1b**: one descriptor set per `MAX_FRAMES_IN_FLIGHT` slot, because binding 0 array element `VERTEX_BUFFER_INSTANCED` (1) points at that frame's dynamic aggregate buffer — every other binding is written into all ring slots identically |

All four sets expose a no-arg `GetDescSet()` that returns `descSets[activeFrameIndex]` (or an unchanged single set, for `GlobalUniformQ2`); `activeFrameIndex` is set once per frame (`VertexBufferQ2::SetActiveFrame`, `ASManagerQ2::SubmitDynamic`) before anything binds it, so `PathTracerQ2` and every other consumer needed no signature changes for G1b.

Compute passes that need only sets 0-1 bind a two-set layout; the ray tracing
pipeline binds all four in the order above (`PathTracerQ2::CreatePipeline`).

---

## 4. Where we deviate: the RG_* upload API

Q2RTX reads the BSP directly (`bsp_mesh.c`) and resolves clusters, light polys,
PVS-based cluster lists and materials in one place, once per map. We instead go
through the RTGL upload API as an intermediary, and rebuild per frame.

Three costs, all of which we have now hit:

1. **Per-frame rebuild of static data.** `RT_ClusterLightListsUpload` walks
   every registered light against the PVS twice, every frame, to produce lists
   that only change on map load. Q2RTX does this once.
2. **Information loss at the boundary.** `RgQ2Material` carries no texture
   handles, so G6b needs a new channel. `RgVertex.cluster` had to be retrofitted
   into a padding field. `bsp_mesh_t` has no equivalent at all.
3. **Two sources of truth joined by `uniqueID`.** The same surface arrives once
   as geometry (`rgUploadGeometry`) and once as a light
   (`rgUploadPolygonalLight`), and the cluster lists reference lights by
   `uniqueID`. Every one of those joins is a place where the two halves can
   silently disagree.

The end state in `PORTING.md` ("remove the `RG_*` API") means replacing this
with a direct BSP reader — a `bsp_mesh` equivalent built at map load. The
current per-frame translation is scaffolding, not the target.

---

## 5. Invariants

These must hold for the Q2RTX shaders to produce anything. Each one is a real
failure mode, not a theoretical one — check them first when the image is wrong.

**Clusters**

- `VboPrimitive.cluster` and `light_list_offsets` index the **same space**:
  vkQuake BSP leaf indices, `[0 .. worldmodel->numleafs)`.
- `primary_rays.rgen` writes the cluster to `IMG_PT_CLUSTER_A` as **uint16**;
  `direct_lighting.rgen` maps `0xffff` to "invalid". Cluster indices must stay
  below 65535.
- A cluster of 0 is the solid leaf. Geometry resolving to it gets no light.

**Lights**

- The world is drawn by `NUM_WORLD_CBX` (6) **parallel** `RT_R_DrawWorldTask`
  invocations that all append emissive surfaces to the same global arrays.
  Anything those tasks write must use an atomic counter, must be reset once per
  frame (`RT_ResetWorldModelLights`) rather than inside `RT_DrawWorld`, and must
  only be consumed after the world tasks complete — `RT_R_DrawViewModelTask`,
  which uploads the lights, needs an explicit `Task_AddDependency` on
  `draw_world_task`. All three were wrong at once, and the symptom was simply
  zero world lights.
- Static `.mat is_light` surfaces use the average **linear RGB** captured when
  `TexMgr_ApplyMaterialFromMat` synthesizes their textures. They append through
  the same atomic `rt_wldlights_tri` list as `@POLY_LIGHT`; averaging sRGB bytes
  directly would over-brighten the emitted radiance.
- The `uniqueID` passed to `rgUploadPolygonalLight` must be the **same value**
  registered via `RT_ClusterLightAdd*`, or the list entry resolves to nothing.
- The point registered for clustering must lie in a **non-solid leaf**. A
  triangle centroid lies on the face plane and resolves to the solid leaf, whose
  PVS is empty — the light then appears in no cluster at all. Offset along the
  face normal (`RT_TriangleLightOrigin`, `RT_GetSurfaceCluster`,
  and Q2RTX's own model-light handling all do this).
- `light_counts_history[frame % 3][c]` must equal
  `light_list_offsets[c+1] - light_list_offsets[c]`.
  `sample_polygonal_lights` reads the count from the **history buffer, not the
  offsets**; a correct offsets array with a zeroed history samples nothing.
- `ubo.num_static_lights` must equal the number of entries written to
  `light_polys`. It also scales the light stats addressing.
- Indices in `light_list_lights` must be `< MAX_LIGHT_POLYS`; the shader treats
  anything larger as an overflow marker and gives it pdf 0.

**Uniform buffer**

- `ubo.current_frame_idx` is what `primary_rays.rgen` packs into the RNG seed at
  `RNG_SEED_SHIFT_FRAME`, and what the light-count history slot is derived from.
  The CPU-side slot choice must use the same counter.
- `pt_light_stats != 0` requires real light stats buffers of
  `num_clusters * num_static_lights * 6 * 2` uints. It is forced to 0 until
  those exist.
- `pt_aperture` must stay 0 — the DoF branch in `primary_rays.rgen` assumes
  Q2RTX's view convention (view +Z forward) and ours is the GL/Vulkan one.

**Framebuffers**

- Every framebuffer image stays in `VK_IMAGE_LAYOUT_GENERAL` for the whole
  frame, including the sampled descriptors. Q2RTX relies on this.

**Dynamic geometry (G1b)**

- Every dynamic-geometry `uniqueID`'s top 4 bits are a stable "kind" tag
  (`Quake/gl_rmisc.c`: 1 = brush surface, 2 = alias model, 3 = sprite,
  4 = custom object). `GeometryQ2::AddDynamicGeometry` relies on this to drop
  kind-3 (sprite) uploads — the only game-side signal used to keep sprites
  out of scope; particles and beams never call `rgUploadGeometry` at all.
- Dynamic `RG_GEOMETRY_VISIBILITY_TYPE_SKY` uploads must stay out of the solid
  ranges until the dedicated sky material and `AS_FLAG_SKY` path is ported.
- Dynamic primitives and BLAS positions must stay in matching contiguous
  world, view-weapon, and viewer-model ranges. Each non-empty range gets a
  separate TLAS instance with an exclusive `AS_FLAG_OPAQUE`,
  `AS_FLAG_VIEWER_WEAPON`, or `AS_FLAG_VIEWER_MODELS` mask; combining these
  bits on one aggregate instance defeats Vulkan visibility filtering because
  instance masks match on any shared bit.
- Dynamic TLAS entries must keep `tlas_instance_model_indices[instance] == -1`
  and set `tlas_instance_prim_offsets[instance]` to their range's primitive
  offset. `get_model_index_and_prim_offset` in
  `path_tracer_hit_shaders.h` then takes the world path while reading the
  correct part of `VERTEX_BUFFER_INSTANCED`. Vertices are already world-space,
  so every category instance's transform must stay identity.
- `VboPrimitive.custom0/1/2` must hold `previous - current` world-space
  position deltas packed with `packHalf4x16` semantics (two `PackHalf2x16`
  calls per vertex), or the motion-vector-consuming passes (ASVGF temporal
  reprojection) see stale/garbage motion for dynamic geometry. New or
  topology-changed geometry (different triangle count than last frame for
  the same `uniqueID`) must get a zero delta, not a delta against unrelated
  vertices.
- Anything mutable that `GetDescSet()` or a ray tracing dispatch reads this
  frame — `GeometryQ2`'s dynamic buffer, `ASManagerQ2`'s dynamic BLAS/
  combined TLAS/TLAS instance buffer/descriptor set/scratch allocator,
  `VertexBufferQ2`'s descriptor set — must be rebuilt in ring slot `frameIndex`
  *before*
  `SkyBufferResolveQ2::Dispatch` (the first per-frame consumer) runs in
  `VulkanDevice::DrawFrame`. The current frame's fence is waited before
  `BeginFrame` returns, so recreating only that one ring slot is safe even
  though frame N-1 may still be executing on the GPU.
- Every BLAS build must be followed by an acceleration-structure build-stage
  write-to-read dependency before TLAS construction. The build-to-ray barrier
  after the TLAS does not establish this dependency. `GlobalUniformQ2::Upload`
  must run after `ASManagerQ2::SubmitDynamic`, because the latter writes this
  frame's category primitive offsets into the CPU-side `InstanceBuffer`.
- `ASManagerQ2::HasTLAS()` must stay false until the *first*
  `SubmitDynamic(cmd, frameIndex)` call for the active ring slot has run
  (not merely after `SubmitStatic()` at level load) — the geometry TLAS
  handle for a never-yet-built ring slot is `VK_NULL_HANDLE`, and dispatching
  a ray trace against it is invalid.
