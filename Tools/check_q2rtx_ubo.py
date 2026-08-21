# Verifies that the Q2RTX global_ubo.h layout matches between the C struct
# (QVKUniformBuffer_t, uploaded as-is via memcpy) and the GLSL std140 layout
# (struct GlobalUniformBuffer). Any mismatch here would corrupt the uniform
# buffer on the GPU.
#
# The Q2RTX struct relies on a C layout that happens to match std140 (every
# vec3 is paired with a 4-byte scalar, so 12+4 = 16). This tool pins that
# assumption so a future edit cannot silently break it.
#
# Also verifies the InstanceBuffer / ModelInstance structs (same C-vs-std140
# contract) and resolves their array sizes from constants.h.
#
# Usage: python check_q2rtx_ubo.py [path-to-global_ubo.h]

import re
import sys
import os

HEADER = "subprojects/vkpt/q2rtx-shaders/global_ubo.h"
CONSTANTS = "subprojects/vkpt/q2rtx-shaders/constants.h"
if len(sys.argv) > 1:
    HEADER = sys.argv[1]
    CONSTANTS = os.path.join(os.path.dirname(HEADER), "constants.h")


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

def parse_constants(path):
    """Parse `#define NAME value` from constants.h. Handles simple integer
    expressions made of already-defined constants (e.g.
    `#define MAX_TLAS_INSTANCES (MAX_MODEL_INSTANCES + MAX_RESERVED_INSTANCES)`)."""
    consts = {}
    if not os.path.exists(path):
        return consts
    with open(path, "r") as f:
        text = f.read()
    for m in re.finditer(r"#define\s+(\w+)\s+([^\n]+)", text):
        name, expr = m.group(1), m.group(2).strip()
        expr = re.sub(r"/\*.*?\*/", "", expr)
        expr = re.sub(r"//.*", "", expr).strip()
        if not re.fullmatch(r"[0-9()\s\w+*\-/]+", expr):
            continue
        consts[name] = expr
    # resolve until fixpoint
    for _ in range(16):
        changed = False
        for name, expr in list(consts.items()):
            if isinstance(expr, int):
                continue
            resolved = expr
            for other, value in consts.items():
                if other != name and isinstance(value, int):
                    resolved = resolved.replace(other, str(value))
            try:
                value = eval(resolved, {"__builtins__": {}}, {})
            except Exception:
                continue
            if isinstance(value, int) and consts[name] != value:
                consts[name] = value
                changed = True
        if not changed:
            break
    # replace any remaining expression strings with ints where possible
    for name, expr in list(consts.items()):
        if not isinstance(expr, int):
            try:
                consts[name] = eval(expr, {"__builtins__": {}}, {})
            except Exception:
                pass
    return consts


def parse_struct_blocks(text, consts=None):
    """Extract BEGIN_SHADER_STRUCT blocks: name -> list of (type, field, count).
    Array sizes may be literals or constant names resolved via `consts`."""
    consts = consts or {}
    structs = {}
    pattern = re.compile(
        r"BEGIN_SHADER_STRUCT\s*\(\s*(\w+)\s*\)\s*\{\s*(.*?)\s*\}",
        re.DOTALL)
    field_pat = re.compile(r"^\s*([\w]+)\s+(\w+)\s*(?:\[(\w+)\])?\s*;",
                           re.MULTILINE)
    for m in pattern.finditer(text):
        name, body = m.group(1), m.group(2)
        fields = []
        for t, n, c in field_pat.findall(body):
            if c:
                if c.isdigit():
                    count = int(c)
                elif c in consts and isinstance(consts[c], int):
                    count = consts[c]
                else:
                    raise ValueError(
                        f"cannot resolve array size '{c}' in struct {name}")
            else:
                count = 1
            fields.append((t, n, count))
        structs[name] = fields
    return structs


def parse_macro_list(text, macro_name, consts=None):
    """Extract (type, name, count) entries from a multiline macro like
    `#define GLOBAL_UBO_VAR_LIST \\ ... GLOBAL_UBO_VAR_LIST_DO(type, name) \\`.
    Array sizes may be literals or constant names resolved via `consts`."""
    consts = consts or {}
    # find the macro body until an unescaped newline ends it
    body_match = re.search(r"#define\s+" + macro_name + r"\s+\\\n(.*?)(?=\n\S)",
                           text, re.DOTALL)
    if not body_match:
        return []
    body = body_match.group(1)
    entries = []
    for line in body.splitlines():
        line = line.replace("\\", "").strip()
        m = re.match(r"GLOBAL_UBO_VAR_LIST_DO\(\s*([\w]+)\s*,\s*(\w+)\s*(?:\[(\w+)\])?\s*\)",
                     line)
        if m:
            count = 1
            if m.group(3):
                c = m.group(3)
                if c.isdigit():
                    count = int(c)
                elif c in consts and isinstance(consts[c], int):
                    count = consts[c]
                else:
                    raise ValueError(
                        f"cannot resolve array size '{c}' in {macro_name}")
            entries.append((m.group(1), m.group(2), count))
    return entries


def parse_cvar_list(text):
    """UBO_CVAR_LIST expands to GLOBAL_UBO_VAR_LIST_DO(float, name)."""
    body_match = re.search(r"#define\s+UBO_CVAR_LIST\s+\\\n(.*?)(?=\n\S)",
                           text, re.DOTALL)
    if not body_match:
        return []
    entries = []
    for line in body_match.group(1).splitlines():
        line = line.replace("\\", "").strip()
        m = re.match(r"UBO_CVAR_DO\(\s*(\w+)\s*,\s*([\w.\-]+)\s*\)", line)
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
        if typ in structs:
            # Nested struct (e.g. ModelInstance inside InstanceBuffer).
            sub = compute_layout(structs[typ], structs, align_of, size_of)
            elem_align = sub[2]
            elem_size = sub[1]
        elif elem_align is None:
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

def check_struct(fields, structs, label):
    """Compare C layout vs std140 layout for one struct. Returns (ok, msg)."""
    c_entries, c_total, _ = compute_layout(fields, structs, c_align, c_size)
    g_entries, g_total, g_align = compute_layout(
        fields, structs, std140_align, std140_size)

    c_map = {name: (off, size) for name, off, size, _ in c_entries}
    g_map = {name: (off, size) for name, off, size, _ in g_entries}

    mismatches = []
    for name in g_map:
        if c_map.get(name) != g_map[name]:
            mismatches.append((name, c_map.get(name), g_map[name]))

    if mismatches:
        msg = f"> {label} MISMATCHES:"
        for name, c, g in mismatches:
            msg += f"\n   {name}: C{tuple(c) if c else None} != GLSL{tuple(g) if g else None}"
        return False, msg

    if c_total != g_total:
        msg = (f"> {label}: OK (field offsets match), "
               f"C struct {c_total}B vs GLSL {g_total}B "
               f"(std140 rounds to {g_align}B) - tail padding only")
        return True, msg

    return True, f"> {label}: OK (C layout == std140, {c_total}B)"


def main():
    with open(HEADER, "r") as f:
        text = f.read()

    consts = parse_constants(CONSTANTS)
    structs = parse_struct_blocks(text, consts)
    fields = parse_macro_list(text, "GLOBAL_UBO_VAR_LIST", consts)
    fields += parse_cvar_list(text)

    if not fields:
        print("> could not parse GLOBAL_UBO_VAR_LIST from", HEADER)
        return 1

    c_entries, c_total, _ = compute_layout(fields, structs, c_align, c_size)
    print(f"> parsed {len(fields)} fields, computed UBO size = {c_total} B")

    ok, msg = check_struct(fields, structs, "UBO (QVKUniformBuffer_t)")
    print(f"> fields: {len(fields)}")
    print(msg)
    if not ok:
        return 1

    # The InstanceSSBO (set 1, binding 1) is a flat copy of the C
    # InstanceBuffer, so its layout must match std140 as well.
    for struct_name in ("ModelInstance", "InstanceBuffer"):
        if struct_name not in structs:
            print(f"> ERROR: {struct_name} not found in {HEADER}")
            return 1
        ok, msg = check_struct(structs[struct_name], structs, struct_name)
        print(msg)
        if not ok:
            return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
