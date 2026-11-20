#!/usr/bin/env python3
"""Makes the low-memory variant of wasm2c's output (the build for 2 GB phones).

The guest's memory is 0x88000000 bytes as it sees it, its Xbox window the top
128 MB (0x80000000), which the game's maps and Direct3D resources address
directly. On a phone with 2 GB of RAM iOS will not reserve that much address
space for an app. This variant keeps the guest's addresses but folds the
window down to follow the low part (HOST_LOW_LIMIT bytes), so the memory is
one block of HOST_LOW_LIMIT + 128 MB (ios/host/host.h, host_wasm.c):

- every access's address goes through HALO_FOLD (a compare and a subtract);
- the bounds checks also trap on the hole between the low part and the
  window, which has no memory behind it.

It writes out_dir/guest-impl.h, patched, and links the other files of
w2c_dir there (their #include "guest-impl.h" finds the patched one first).

usage: fold_memory.py w2c_dir out_dir low_limit
"""

import os
import sys


def fold(text, low):
    prelude = (
        "\n/* the low-memory build (ios/tools/fold_memory.py) */\n"
        f"#define HALO_LOW_LIMIT {low:#x}ull\n"
        "#define HALO_WINDOW 0x80000000ull\n"
        "#define HALO_FOLD(a) ((u64)(a) - (((u64)(a) >> 31) & 1) * (HALO_WINDOW - HALO_LOW_LIMIT))\n"
        "/* an access from a to end that touches the hole */\n"
        "#define HALO_HOLE(a, end) ((u64)(end) > HALO_LOW_LIMIT && (u64)(a) < HALO_WINDOW)\n"
    )
    replacements = [
        # (the memory's accesses)
        ("#define MEM_ADDR_MEMOP(mem, addr) (&wasm_rt_local_memory_base[(addr)])",
         "#define MEM_ADDR_MEMOP(mem, addr) (&wasm_rt_local_memory_base[HALO_FOLD(addr)])"),
        ("#define MEM_ADDR_MEMOP(mem, addr) (&(mem)->data[addr])",
         "#define MEM_ADDR_MEMOP(mem, addr) (&(mem)->data[HALO_FOLD(addr)])"),
        ("load_data(&(m).data[o], i, s);", "load_data(&(m).data[HALO_FOLD(o)], i, s);"),
        ("memset(&mem->data[d], val, n);", "memset(&mem->data[HALO_FOLD(d)], val, n);"),
        ("memmove(&dest->data[dest_addr], &src->data[src_addr], n);",
         "memmove(&dest->data[HALO_FOLD(dest_addr)], &src->data[HALO_FOLD(src_addr)], n);"),
        ("&mem->data[addr]", "&mem->data[HALO_FOLD(addr)]"),
        # (the bounds checks)
        ("    if (UNLIKELY(res > (mem)->size))             \\",
         "    if (UNLIKELY(res > (mem)->size || HALO_HOLE(offset, res))) \\"),
        ("  if (UNLIKELY(a + (uint64_t)sizeof(t) > local_memory_size)) \\",
         "  if (UNLIKELY(a + (uint64_t)sizeof(t) > local_memory_size || HALO_HOLE(a, a + (uint64_t)sizeof(t)))) \\"),
        ("  if (UNLIKELY(a + (uint64_t)sizeof(t) > mem->size))     \\",
         "  if (UNLIKELY(a + (uint64_t)sizeof(t) > mem->size || HALO_HOLE(a, a + (uint64_t)sizeof(t)))) \\"),
    ]
    for old, new in replacements:
        if old not in text:
            sys.exit(f"fold_memory.py: wasm2c's output has changed, not found: {old}")
        text = text.replace(old, new)
    # the prelude after the runtime's own definitions (u64)
    anchor = "#define TRAP(x)"
    if anchor not in text:
        sys.exit("fold_memory.py: no TRAP definition to put the fold before")
    return text.replace(anchor, prelude + anchor, 1)


def main():
    w2c_dir, out_dir, low = sys.argv[1], sys.argv[2], int(sys.argv[3], 0)
    os.makedirs(out_dir, exist_ok=True)
    for name in os.listdir(w2c_dir):
        source = os.path.abspath(os.path.join(w2c_dir, name))
        target = os.path.join(out_dir, name)
        if os.path.lexists(target):
            os.remove(target)
        if name == "guest-impl.h":
            with open(source) as f:
                text = fold(f.read(), low)
            with open(target, "w") as f:
                f.write(text)
        elif os.path.isfile(source):
            os.symlink(source, target)


if __name__ == "__main__":
    main()
