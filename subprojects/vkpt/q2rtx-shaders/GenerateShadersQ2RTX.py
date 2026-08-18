# Compiles the vendored Q2RTX shader set (subprojects/vkpt/q2rtx-shaders/)
# into Build/q2rtx/*.spv for validation only. These shaders are not loaded
# by the game yet: they are the reference set that replaces the current
# Source/Shaders/ set one module at a time (see PORTING.md, stage S2).
#
# Usage: python GenerateShadersQ2RTX.py [-rebuild]
#
# Unlike GenerateShaders.py this is our own tool, so the cache format is
# deliberately simple: one line per compiled file with a timestamp.

import os
import subprocess
import sys
import pathlib

SHADER_DIR = os.path.dirname(os.path.abspath(__file__))
SHADER_EXTENSIONS = (".comp", ".vert", ".frag", ".rgen", ".rahit", ".rchit", ".rmiss", ".rint")
OUT_DIR = os.path.normpath(os.path.join(SHADER_DIR, "..", "Build", "q2rtx"))
CACHE_FILE = os.path.join(OUT_DIR, "q2rtx-cache.txt")

# Q2RTX compiles the FSR shaders with the fsr/ folder added to the include
# path (see Q2RTX src/CMakeLists.txt); replicate that here.
FSR_INCLUDE = os.path.join(SHADER_DIR, "fsr")


def load_cache():
    cache = {}
    try:
        with open(CACHE_FILE, "r") as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 2:
                    cache[parts[0]] = int(parts[1])
    except (OSError, ValueError):
        cache = {}
    return cache


def save_cache(cache):
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(CACHE_FILE, "w") as f:
        for name, mtime in sorted(cache.items()):
            f.write(f"{name} {mtime}\n")


def main():
    force = "-rebuild" in sys.argv or "-r" in sys.argv

    os.makedirs(OUT_DIR, exist_ok=True)

    files = sorted(
        f for f in os.listdir(SHADER_DIR)
        if os.path.isfile(os.path.join(SHADER_DIR, f)) and f.endswith(SHADER_EXTENSIONS)
    )

    cache = load_cache()
    ok = 0
    failed = []

    for name in files:
        src = os.path.join(SHADER_DIR, name)
        mtime = int(pathlib.Path(src).stat().st_mtime)
        out = os.path.join(OUT_DIR, name + ".spv")

        if not force and cache.get(name) == mtime and os.path.exists(out):
            ok += 1
            continue

        print(f"> Building {name}")
        result = subprocess.run(
            [
                "glslc", "--target-env=vulkan1.2", "-DVKPT_SHADER",
                "-I", SHADER_DIR, "-I", FSR_INCLUDE,
                src, "-o", out,
            ],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

        if result.returncode != 0:
            failed.append(name)
            print(result.stdout)
        else:
            cache[name] = mtime
            ok += 1

    save_cache(cache)

    print(f"\n> {ok}/{len(files)} shaders compiled OK")
    if failed:
        print("> failed: " + ", ".join(failed))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
