# Changelog

## 17.08.2026

### Added
- **Full Q2RTX-style ray-traced renderer** (built into the executable): ASVGF denoiser, ReSTIR direct/indirect lighting, per-BSP-cluster light lists, fog volumes, god rays, procedural physical sky, Q2RTX tone mapping and TAAU upscaling
- **Q2RTX-style materials** — `.mat` definitions with automatic detection from HD texture-pack suffixes (`_norm`/`_gloss`/`_luma`), plus `.pkz` archives mounted as engine search paths
- **Q2RTX blue noise** — 16-bit PNG pack (`blue_noise.pkz`) loaded through the engine file system; all renderer files (shaders, textures, configs) now load from the game dir or `.pkz` archives
- **Brightness / sky / light controls** — master brightness, sky color and brightness, light color tint, sun presets
- **Bloom**, upscalers (FSR 2, FSR 3.1, DLSS) with quality presets and a redesigned video menu
- **Dynamic lights for explosions** in ray-traced mode
- **Crash log** written next to the executable on an unhandled exception
- **CMake build system** with automated asset deployment

### Changed
- The legacy (pre-Q2RTX) renderer was removed — the Q2RTX-style core is now the only renderer
- Classic lighting is disabled in the RT renderer — the ray tracer produces all the lighting
- World geometry carries per-vertex BSP cluster data for the per-cluster light lists

### Fixed
- Torch / muzzle flash / explosion lights no longer cut off in square patches on walls
- Sky surfaces no longer block primary rays or occlude the sun in the god-rays pass
- Sky tint and brightness now apply to every sky type
- Unsupported models and missing precache entries no longer abort the game
- Material lookup now handles BSP texture names, per-frame model skins and `#`-names correctly
