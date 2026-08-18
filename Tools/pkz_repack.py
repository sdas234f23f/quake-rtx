#!/usr/bin/env python3
"""Repack id1/ovrd_mat.pkz WITHOUT the ovrd.mat entry.

The repo keeps ovrd.mat as the VCS source of truth at
subprojects/vkpt/Source/materials/ovrd.mat and build_win.ps1 deploys it to
the game dir after each build. The material loader searches .pkz archives
BEFORE the filesystem, so ovrd.mat must NOT be inside the pkz, otherwise the
stale pkz copy wins over the deployed loose one.
"""
import sys
import zipfile

PKZ_PATH = "id1/ovrd_mat.pkz"
REMOVE = {"ovrd.mat", "materials/ovrd.mat"}


def main():
    src = zipfile.ZipFile(PKZ_PATH, "r")
    entries = [(i.filename, i.file_size) for i in src.infolist()]
    print(f"{PKZ_PATH}: {len(entries)} entries")

    removed = [name for name, _ in entries if name in REMOVE]
    kept = [(name, size) for name, size in entries if name not in REMOVE]

    for name in removed:
        print(f"  removing: {name}")

    if not removed:
        print("  nothing to remove")
        return 0

    tmp = PKZ_PATH + ".new"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as out:
        for name, _ in kept:
            out.writestr(name, src.read(name))

    src.close()
    import os
    os.replace(tmp, PKZ_PATH)
    print(f"done: {PKZ_PATH} now has {len(kept)} entries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
