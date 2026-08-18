#!/usr/bin/env python3
"""Package the generated RT shaders into a .pkz (zip).

Usage: python Tools/zip_shaders.py <shader_dir> <out_pkz> [prefix]

Packs every *.spv under <shader_dir> (recursively) into <out_pkz> with
entries "<prefix>/<relpath>.spv" (prefix defaults to "shaders"). The
renderer loads the shaders through the engine file system (COM_FindFile),
which searches mounted .pkz archives BEFORE the game dir, so a loose
shaders/ folder is not needed.
"""
import os
import sys
import zipfile

PREFIX = "shaders"


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src_dir, out_pkz = sys.argv[1], sys.argv[2]
    prefix = sys.argv[3] if len(sys.argv) > 3 else PREFIX

    spvs = []
    for root, _dirs, files in os.walk(src_dir):
        for f in sorted(files):
            if f.endswith(".spv"):
                full = os.path.join(root, f)
                rel = os.path.relpath(full, src_dir)
                spvs.append((full, rel))
    spvs.sort(key=lambda p: p[1])

    if not spvs:
        print(f"no .spv files under {src_dir}")
        return 1

    tmp = out_pkz + ".tmp"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
        for full, rel in spvs:
            z.write(full, f"{prefix}/{rel}")

    os.replace(tmp, out_pkz)
    print(f"wrote {out_pkz}: {len(spvs)} shaders under {prefix}/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
