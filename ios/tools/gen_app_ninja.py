#!/usr/bin/env python3
"""Ninja graph for the iOS app's executable (scripts/build_ios.sh).

It compiles, for arm64 iOS with Xcode's clang:

- the guest module as wasm2c translated it (guest_*.c), and wasm2c's runtime;
- ios_imports.c (ios/tools/gen_imports.py) and the host (ios/host);
- from OpenCE: the page hashing of the write tracking
  (port/android/host/host_watch_hash.c) and the file helpers of posix.h
  (port/linux/src/posix_files.c);

and links them with SDL3 (a static library) and the system frameworks.
"""

import argparse
import shlex
import subprocess
from pathlib import Path

FRAMEWORKS = [
    "AVFoundation", "AudioToolbox", "CoreAudio", "CoreBluetooth", "CoreFoundation", "CoreGraphics",
    "CoreHaptics", "CoreMedia", "CoreMotion", "CoreVideo", "Foundation", "GameController", "IOKit", "Metal",
    "OpenGLES", "QuartzCore", "UIKit", "UniformTypeIdentifiers",
]

# wasm2c's runtime: explicit bounds checks (no 8 GB guard regions, which an
# app's address space may not have), its memory reserved with mmap, no
# stack depth counting (the host gives guest threads large C stacks), and
# traps to the host
WASM_RT_DEFINES = [
    "-DWASM_RT_MEMCHECK_BOUNDS_CHECK=1",
    "-DWASM_RT_USE_MMAP=1",
    "-DWASM_RT_NONCONFORMING_UNCHECKED_STACK_EXHAUSTION=1",
    "-DWASM_RT_TRAP_HANDLER=host_wasm_trap",
]


def esc(path):
    return str(path).replace("$", "$$").replace(" ", "$ ").replace(":", "$:")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--opence", type=Path, required=True)
    parser.add_argument("--w2c", type=Path, required=True)
    parser.add_argument("--wabt", type=Path, required=True)
    parser.add_argument("--sdl", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--ios-min", default="15.0")
    args = parser.parse_args()

    sdk = subprocess.run(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], capture_output=True, text=True,
                         check=True).stdout.strip()
    clang = subprocess.run(["xcrun", "--sdk", "iphoneos", "--find", "clang"], capture_output=True, text=True,
                           check=True).stdout.strip()
    target = ["-target", f"arm64-apple-ios{args.ios_min}", "-isysroot", sdk]
    host_dir = args.root / "ios" / "host"
    runtime = args.wabt / "wasm2c"
    common = target + ["-O2", "-g0", "-fno-strict-aliasing"]
    includes = [
        f"-I{host_dir}", f"-I{args.w2c}", f"-I{runtime}", f"-I{args.opence / 'port/android/include'}",
        f"-I{args.opence / 'port/android/host'}", f"-I{args.opence / 'port/linux/src'}",
        f"-I{args.sdl / 'include'}",
    ]

    lines = [
        "ninja_required_version = 1.7",
        f"cc = {clang}",
        "rule cc",
        "  command = $cc -MMD -MF $out.d $flags -c $in -o $out",
        "  depfile = $out.d",
        "  deps = gcc",
        "  description = $kind $out",
        "rule link",
        "  command = $cc $flags -o $out @$out.rsp $libs",
        "  rspfile = $out.rsp",
        "  rspfile_content = $in_newline",
        "  description = LINK $out",
    ]
    objects = []

    def compile_unit(source, flags, kind):
        obj = args.out / "obj" / (source.parent.name + "_" + source.name + ".o")
        lines.append(f"build {esc(obj)}: cc {esc(source)}")
        lines.append("  flags = " + " ".join(shlex.quote(f) for f in flags).replace("$", "$$"))
        lines.append(f"  kind = {kind}")
        objects.append(obj)

    guest_flags = common + WASM_RT_DEFINES + [f"-I{args.w2c}", f"-I{runtime}", "-w", "-fno-optimize-sibling-calls"]
    for source in sorted(args.w2c.glob("guest_*.c")):
        compile_unit(source, guest_flags, "W2C")
    for name in ("wasm-rt-impl.c", "wasm-rt-mem-impl.c"):
        compile_unit(runtime / name, guest_flags, "RT")

    host_flags = common + WASM_RT_DEFINES + includes + ["-Wall", "-Wno-unused-function", "-Wno-unused-parameter"]
    compile_unit(args.w2c / "ios_imports.c", host_flags + ["-Wno-incompatible-pointer-types-discards-qualifiers",
                                                            "-DGL_SILENCE_DEPRECATION"], "CC")
    for source in sorted(host_dir.glob("*.c")):
        compile_unit(source, host_flags + ["-DGL_SILENCE_DEPRECATION"], "CC")
    for source in sorted(host_dir.glob("*.m")):
        compile_unit(source, host_flags + ["-fobjc-arc", "-DGL_SILENCE_DEPRECATION"], "OBJC")
    compile_unit(args.opence / "port/android/host/host_watch_hash.c", host_flags, "CC")
    compile_unit(args.opence / "port/linux/src/posix_files.c",
                 common + [f"-I{args.opence / 'port/linux/src'}", "-w", "-Dst_mtim=st_mtimespec",
                           "-Dst_atim=st_atimespec", "-Dst_ctim=st_ctimespec"], "CC")

    executable = args.out / "HaloCE"
    link_flags = target + ["-fobjc-arc", "-dead_strip"]
    libs = [str(args.sdl / "lib" / "libSDL3.a")] + [f"-framework {name}" for name in FRAMEWORKS] + ["-lc++"]
    lines.append(f"build {esc(executable)}: link " + " ".join(esc(o) for o in objects))
    lines.append("  flags = " + " ".join(link_flags))
    lines.append("  libs = " + " ".join(libs))
    lines.append(f"default {esc(executable)}")
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "build.ninja").write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
