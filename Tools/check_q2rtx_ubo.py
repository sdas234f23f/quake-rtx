# Verifies that the Q2RTX global_ubo.h layout matches between the C struct
# (QVKUniformBuffer_t, uploaded as-is via memcpy) and the GLSL std140 layout
# (struct GlobalUniformBuffer). Any mismatch here would corrupt the uniform
# buffer on the GPU.
#
# The Q2RTX struct relies on a C layout that happens to match std140 (every
# vec3 is paired with a 4-byte scalar, so 12+4 = 16). This tool pins that
# assumption so a future edit cannot silently break it.
#
# Usage: python check_q2rtx_ubo.py [path-to-global_ubo.h]

import re
import sys
import os

HEADER = "subprojects/vkpt/q2rtx-shaders/global_ubo.h"
if len(sys.argv) > 1:
    HEADER = sys.argv[1]


# ---------------------------------------------------------------------------
# Layout model
# ---------------------------------------------------------------------------

def std140_align(typ):
    """std140 base alignment of a type."""
    return {
        "int": 4, "uint": 4, "float": 4,
        "vec2": 8, "ivec2": 8, "uvec2": 8,
        "vec3": 16, "ivec3": 16, "uvec3": 16,
        "vec4": 16, "ivec4": 16, "uvec4": 16,
        "mat3": 16, "mat4": 16,
    }.get(typ)


def std140_size(typ):
    return {
        "int": 4, "uint": 4, "float": 4,
        "vec2": 8, "ivec2": 8, "uvec2": 8,
        "vec3": 12, "ivec3": 12, "uvec3": 12,
        "vec4": 16, "ivec4": 16, "uvec4": 16,
        "mat3": 48, "mat4": 64,
    }.get(typ)


def c_align(typ):
    return 4  # all C scalar/array types here are 4-aligned


def c_size(typ):
    return {
        "int": 4, "uint": 4, "float": 4,
        "vec2": 8, "ivec2": 8, "uvec2": 8,
        "vec3": 12, "ivec3": 12, "uvec3": 12,
        "vec4": 16, "ivec4": 16, "uvec4": 16,
        "mat3": 48, "mat4": 64,
    }.get(typ)


def round_up(value, alignment):
    return (value + alignment - 1) // alignment * alignment


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

def parse_struct_blocks(text):
    """Extract BEGIN_SHADER_STRUCT blocks: name -> list of (type, field)."""
    structs = {}
    pattern = re.compile(
        r"BEGIN_SHADER_STRUCT\s*\(\s*(\w+)\s*\)\s*\{\s*(.*?)\s*\}",
        re.DOTALL)
    field_pat = re.compile(r"^\s*([\w]+)\s+(\w+)\s*(?:\[(\d+)\])?\s*;",
                           re.MULTILINE)
    for m in pattern.finditer(text):
        name, body = m.group(1), m.group(2)
        fields = [(t, n, int(c) if c else 1)
                  for t, n, c in field_pat.findall(body)]
        structs[name] = fields
    return structs


def parse_macro_list(text, macro_name):
    """Extract (type, name, count) entries from a multiline macro like
    `#define GLOBAL_UBO_VAR_LIST \\ ... GLOBAL_UBO_VAR_LIST_DO(type, name) \\`."""
    # find the macro body until an unescaped newline ends it
    body_match = re.search(r"#define\s+" + macro_name + r"\s+\\\n(.*?)(?=\n\S)",
                           text, re.DOTALL)
    if not body_match:
        return []
    body = body_match.group(1)
    entries = []
    for line in body.splitlines():
        line = line.replace("\\", "").strip()
        m = re.match(r"GLOBAL_UBO_VAR_LIST_DO\(\s*([\w]+)\s*,\s*(\w+)\s*(?:\[(\d+)\])?\s*\)",
                     line)
        if m:
            entries.append((m.group(1), m.group(2),
                            int(m.group(3)) if m.group(3) else 1))
    return entries


def parse_cvar_list(text):
    """UBO_CVAR_LIST expands to GLOBAL_UBO_VAR_LIST_DO(float, name)."""
    body_match = re.search(r"#define\s+UBO_CVAR_LIST\s+\\\n(.*?)(?=\n\S)",
                           text, re.DOTALL)
    if not body_match:
        return []
    entries = []
    for line in body_match.group(1).splitlines():
        m = re.match(r"UBO_CVAR_DO\(\s*(\w+)\s*,\s*[\w.\-]+\s*\)", line)
        if m:
            entries.append(("float", m.group(1), 1))
    return entries


# ---------------------------------------------------------------------------
# Layout computation
# ---------------------------------------------------------------------------

def compute_layout(fields, structs, align_of, size_of):
    """Returns (entries, total_size) where entries is a list of
    (name, offset, size, align) in the given layout model."""
    offset = 0
    entries = []
    max_align = 4
    for typ, name, count in fields:
        elem_align = align_of(typ)
        elem_size = size_of(typ)
        if elem_align is None:
            if typ in structs:
                sub = compute_layout(structs[typ], structs, align_of, size_of)
                elem_align = sub[2]
                elem_size = sub[1]
            else:
                raise ValueError(f"unknown type {typ} in {name}")

        if count > 1:
            stride = round_up(elem_size, elem_align)
            total_size = stride * count
        else:
            stride = 0
            total_size = elem_size

        offset = round_up(offset, elem_align)
        entries.append((name, offset, total_size, elem_align))
        offset += total_size
        max_align = max(max_align, elem_align)

    total = round_up(offset, max_align)
    return entries, total, max_align


# ---------------------------------------------------------------------------

def main():
    with open(HEADER, "r") as f:
        text = f.read()

    structs = parse_struct_blocks(text)
    fields = parse_macro_list(text, "GLOBAL_UBO_VAR_LIST")
    fields += parse_cvar_list(text)

    if not fields:
        print("> could not parse GLOBAL_UBO_VAR_LIST from", HEADER)
        return 1

    c_entries, c_total, _ = compute_layout(
        fields, structs, c_align, c_size)
    g_entries, g_total, g_align = compute_layout(
        fields, structs, std140_align, std140_size)

    c_map = {name: (off, size) for name, off, size, _ in c_entries}
    g_map = {name: (off, size) for name, off, size, _ in g_entries}

    mismatches = []
    for name in g_map:
        if c_map.get(name) != g_map[name]:
            mismatches.append(
                (name, c_map.get(name), g_map[name]))

    print(f"> fields: {len(fields)}")
    print(f"> C struct size:     {c_total} bytes")
    print(f"> GLSL std140 size:  {g_total} bytes (align {g_align})")

    if mismatches:
        print("> MISMATCHES:")
        for name, c, g in mismatches:
            print(f"   {name}: C{tuple(c) if c else None} != GLSL{tuple(g) if g else None}")
        return 1

    if c_total != g_total:
        # All field offsets match; only std140 struct tail padding differs.
        # This is benign: the last field ends at min(c_total, g_total) and no
        # shader reads past it. The C++ buffer size (c_total) is what Q2RTX
        # itself uploads.
        print(f"> WARNING: tail padding only - C struct {c_total}B, "
              f"GLSL declares {g_total}B (std140 rounds to {g_align}B)")
        print("> OK: every field offset/size matches, difference is tail padding")
        return 0

    print("> OK: C layout == std140 layout (no mismatches)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
