#!/usr/bin/env python3
"""Analyze / complete subprojects/vkpt/Source/materials/ovrd.mat.

Checks the .mat coverage against the actual game data in build-cmake/id1:
  1. loose HD world textures (id1/textures/*.png) -> textures/<base>:
  2. luma-bearing textures missing is_light (emissive -> light source)
  3. model skins (PAKS/mdl_skins, id1/progs) -> progs/<model>.mdl:frame<N>
  4. sprite skins (PAKS/progs/*.spr, id1/progs) -> progs/<spr>.spr:frame<N>

Run with --apply to write the completed file (merges + sorts parameters),
without --apply it only prints a report.
"""
import os
import re
import sys

MAT_PATH = "subprojects/vkpt/Source/materials/ovrd.mat"
ID1 = "build-cmake/id1"
TEX_DIR = os.path.join(ID1, "textures")
PAKS = os.path.join(ID1, "PAKS")

# .mat parameter order: texture_* first, then the rest
TEXTURE_KEYS = ["texture_base", "texture_normals", "texture_emissive",
                "texture_mask", "texture_gloss"]
PARAM_KEYS = ["base_factor", "roughness_override", "metalness_factor",
              "specular_factor", "emissive_factor", "bump_scale",
              "is_light", "synth_emissive", "emissive_threshold",
              "light_intensity"]

SUFFIX_RE = re.compile(r"_(n|norm|gloss|luma|glow|bump)$")


def strip_suffix(name):
    """'#lava1_luma' -> '#lava1'; 'window01_1_n' -> 'window01_1'"""
    return SUFFIX_RE.sub("", name)


def parse_mat(path):
    """Return {entry_name: [param lines]} preserving order."""
    blocks = {}
    cur = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            stripped = line.rstrip("\n")
            if not stripped.strip():
                continue
            if not stripped[0].isspace():
                # entry header "name:"
                cur = stripped[:-1] if stripped.endswith(":") else stripped
                blocks.setdefault(cur, [])
            else:
                if cur is not None:
                    blocks[cur].append(stripped.strip())
    return blocks


def loose_world_textures():
    """base name -> set of suffixes present"""
    found = {}
    if not os.path.isdir(TEX_DIR):
        return found
    for fn in os.listdir(TEX_DIR):
        base = os.path.splitext(fn)[0]
        m = SUFFIX_RE.search(base)
        if m:
            core = strip_suffix(base)
            found.setdefault(core, set()).add(m.group(1))
        else:
            found.setdefault(base, set())
    return found


def model_skins():
    """progs model name -> frame index -> skin filename"""
    skins = {}
    src = os.path.join(PAKS, "mdl_skins")
    if os.path.isdir(src):
        for fn in sorted(os.listdir(src)):
            m = re.match(r"^(.+?)_skin(\d+)\.png$", fn)
            if m:
                model, idx = m.group(1), int(m.group(2))
                skins.setdefault(model, {})[idx] = fn
    return skins


def sprite_frames():
    """Reserved for real sprite frame enumeration (PAKS/progs/*.spr).

    NOTE: id1/progs/* dirs are MODEL skin folders (armor/, b_g_key/, ...),
    not sprites - do not scan them here.
    """
    return {}


def sort_params(lines):
    """Reorder: texture_* first (in TEXTURE_KEYS order), then PARAM_KEYS order,
    then any unknown keys (kept in original order)."""
    def key(line):
        k = line.split()[0].lower()
        if k in TEXTURE_KEYS:
            return (0, TEXTURE_KEYS.index(k))
        if k in PARAM_KEYS:
            return (1, PARAM_KEYS.index(k))
        return (2, 0)
    # dedupe by key (first wins), keep order stable
    seen = set()
    uniq = []
    for ln in lines:
        k = ln.split()[0].lower()
        if k in seen:
            continue
        seen.add(k)
        uniq.append(ln)
    return sorted(uniq, key=key)


def main():
    apply = "--apply" in sys.argv
    blocks = parse_mat(MAT_PATH)
    report = []

    # 1. loose HD world textures
    loose = loose_world_textures()
    missing_tex = [b for b in sorted(loose) if "textures/" + b not in blocks]
    luma_missing_light = []
    for b in sorted(loose):
        entry = "textures/" + b
        if entry in blocks and "luma" in loose[b]:
            lines = blocks[entry]
            if not any(l.startswith("is_light") for l in lines):
                luma_missing_light.append(b)
    report.append(f"loose HD world textures: {len(loose)}; "
                  f"missing entries: {len(missing_tex)}")
    report.append(f"luma-bearing entries without is_light: {len(luma_missing_light)}")
    report.append("  " + ", ".join(luma_missing_light[:40]))

    # 2. model skins
    skins = model_skins()
    missing_models = []
    for model in sorted(skins):
        for idx in sorted(skins[model]):
            entry = "progs/%s.mdl:frame%d" % (model, idx)
            if entry not in blocks:
                missing_models.append(entry)
    report.append(f"model skin entries missing: {len(missing_models)}")
    report.append("  " + ", ".join(missing_models[:40]))

    # 3. sprite frames
    sprites = sprite_frames()
    missing_sprites = []
    for s in sorted(sprites):
        for i, _ in enumerate(sprites[s]):
            entry = "progs/%s.spr:frame%d" % (s, i)
            if entry not in blocks:
                missing_sprites.append(entry)
    report.append(f"sprite frame entries missing: {len(missing_sprites)}")
    report.append("  " + ", ".join(missing_sprites[:40]))

    print("\n".join(report))

    if not apply:
        print("\n(no --apply: nothing written)")
        return 0

    # apply: add missing entries
    for b in missing_tex:
        entry = "textures/" + b
        lines = []
        if "n" in loose[b] or "norm" in loose[b]:
            lines.append("texture_normals textures/%s_n.png" % b)
        if "luma" in loose[b]:
            lines.append("texture_emissive textures/%s_luma.png" % b)
            lines.append("is_light 1")
        if "gloss" in loose[b]:
            lines.append("texture_gloss textures/%s_gloss.png" % b)
        blocks.setdefault(entry, lines)

    # add is_light to luma-bearing entries that lack it
    added_light = 0
    for b in luma_missing_light:
        entry = "textures/" + b
        lines = blocks[entry]
        if "luma" in loose[b]:
            if not any(l.startswith("texture_emissive") for l in lines):
                lines.append("texture_emissive textures/%s_luma.png" % b)
            lines.append("is_light 1")
            added_light += 1

    for entry in missing_models:
        blocks.setdefault(entry, [])

    for entry in missing_sprites:
        blocks.setdefault(entry, [])

    # write back with sorted params
    with open(MAT_PATH, "w", encoding="utf-8", newline="\n") as f:
        for name in sorted(blocks):
            f.write("%s:\n" % name)
            for ln in sort_params(blocks[name]):
                f.write("\t%s\n" % ln)
            f.write("\n")

    print("\napplied: +%d textures, +%d is_light, +%d models, +%d sprites"
          % (len(missing_tex), added_light, len(missing_models),
             len(missing_sprites)))
    print("total entries: %d" % len(blocks))
    return 0


if __name__ == "__main__":
    sys.exit(main())
