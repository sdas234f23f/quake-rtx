# ovrd.mat — RT material overrides

This file contains **material overrides** for the RT renderer (Q2RTX style).
The repository copy is the source of truth; `build_win.ps1` deploys it to
`<build>/id1/materials/ovrd.mat` after every build. It **must not** live inside
a `.pkz`: the loader searches archives before the file system, so a stale copy
in a pkz would shadow the deployed file.

## Syntax

```
<material name>:
	key value
	key value
```

Each entry is a material name followed by `key value` lines, indented.
Within an entry, keys are ordered: `texture_*` first, then parameters.

### Material names

| Type | Name |
|------|------|
| World texture (BSP) | `textures/<name>:` (e.g. `textures/tlight11:`, `textures/#lava1:`) |
| Model | `progs/<model>.mdl:frame<N>:` (N — skin/frame number) |
| Sprite | `progs/<spr>.spr:frame<N>:` |

An empty entry (`textures/foo:` with no keys) makes the material "configurable"
but does not change its behavior (texture auto-detection is used).

## Keys

### Textures (`texture_*`)

| Key | Purpose |
|-----|---------|
| `texture_base` | Base (diffuse/albedo) texture |
| `texture_normals` | Normal map |
| `texture_emissive` | Emissive (luma) texture — glow + light source |
| `texture_mask` | Mask (alpha/material) |
| `texture_gloss` | Gloss map (roughness = 1 − gloss) |

### Parameters

| Key | Value | Default | Description |
|-----|-------|---------|-------------|
| `base_factor` | number | `1.0` | Albedo brightness multiplier |
| `roughness_override` | 0..1 | map/default | Forced roughness |
| `metalness_factor` | 0..1 | `0.0` | Metalness |
| `specular_factor` | number | `1.0` | Specular reflection multiplier |
| `emissive_factor` | number | `1.0` | Emission brightness multiplier |
| `bump_scale` | number | `1.0` | Normal/bump strength |
| `is_light` | 0/1 | `0` | Material becomes a **light source** (polygonal/spherical light from the emissive surface) |
| `synth_emissive` | 0/1 | `0` | Synthesize emission from the base texture using a threshold |
| `emissive_threshold` | 0..255 | — | Brightness threshold for `synth_emissive` |

> `light_intensity` (a number, per-material light multiplier) is recognized in
> the file for compatibility but is ignored by the parser on the current branch —
> light brightness is controlled by the `rt_plight_intensity` (world) and
> `rt_dlight_intensity` (models/sprites) cvars.

## Auto-detection

If a texture has no entry in `.mat`, the engine looks for related files next to
it: `<name>_n.png` / `<name>_bump.png` (normals), `<name>_gloss.png` (gloss),
`<name>_luma.png` / `<name>_glow.png` (emission).

## Examples

```mat
# Torch: texture + emission, a real light source
progs/flame.mdl:frame0:
	texture_base mdl/flame_skin0.png
	texture_emissive mdl/flame_luma.png
	is_light 1
	synth_emissive 1
	emissive_threshold 160

# Metal armor: normals + gloss
progs/armor.mdl:frame1:
	texture_base progs/armor/1.png
	texture_normals progs/armor/1_n.png
	texture_gloss progs/armor/1_gloss.png
	metalness_factor 1.0

# Lava ball — an emissive light source
progs/lavaball.mdl:frame0:
	texture_base mdl/lavaball_skin0.png
	texture_emissive mdl/lavaball_luma.png
	is_light 1
	emissive_threshold 160
```

## Tools

- `Tools/gen_ovrd_mat_full.py` — checks coverage and fills in missing entries
  (`--apply` writes the file and sorts parameters: `texture_*` first).
- `Tools/pkz_repack.py` — rebuilds `id1/ovrd_mat.pkz` without `ovrd.mat`
  (so the loose repository copy always takes priority).
