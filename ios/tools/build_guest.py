#!/usr/bin/env python3
"""Build the iOS port's guest module: build/ios/halo_guest.wasm.

The guest is the Android port's guest image (port/android/guest in OpenCE:
the game, the platform layer shared with Linux, a subset of musl and the
guest runtime) compiled for wasm32 instead of arm64_32. wasm32 keeps what
the game's data needs (32-bit pointers and longs), and a WebAssembly module
can be translated ahead of time into C (wasm2c) and compiled into an iOS
app, which may not map code of its own at run time nor use the low 4 GB of
its address space, the two things the Android loader does.

How it is made: OpenCE's configure.py generates the Android graph (with a
stand-in NDK: only its OpenGL ES headers and tools are used here), the
Android graph's generated sources and headers are built, and every guest
compile is rewritten for wasm32:

- clang from wasi-sdk (a recent LLVM; the system's may crash on this code),
  --target=wasm32-unknown-unknown with threads (atomics, bulk memory);
- the guest C library's arch directory is ios/guest/libc/arch/wasm32;
- -fno-common (WebAssembly objects have no common symbols; the game
  defines each tentative definition once);
- the guest runtime's guest_start.c and guest_thread.c are replaced by
  ios/guest/runtime's, and the Android import stubs (imports.s) are
  dropped: every undefined function becomes an import from "env".

Usage:
  build_guest.py --opence DIR --wasi-sdk DIR --gl-include DIR --out DIR [--jobs N]
"""

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
IOS = HERE.parent
GUEST = IOS / "guest"

ANDROID_ARCH = "port/android/guest/libc/arch/arm64_32"
ANDROID_SRC_INCLUDE = "port/android/guest/libc/src_include"
REPLACED_RUNTIME = {
    "port/android/guest/runtime/guest_start.c": GUEST / "runtime" / "guest_start_wasm.c",
    "port/android/guest/runtime/guest_thread.c": GUEST / "runtime" / "guest_thread_wasm.c",
}
# flags of the arm64_32 guest that wasm32 has no use for
DROPPED = {
    "--target=arm64_32-apple-watchos",
    "-mcpu=cortex-a53",
    "-femulated-tls",
    "-fno-asynchronous-unwind-tables",
    "-fno-unwind-tables",
    "-MMD",
    "-S",
}
DROPPED_WITH_VALUE = {"-mllvm", "-MF", "-o"}
WASM_FLAGS = [
    "--target=wasm32-unknown-unknown",
    "-matomics",
    "-mbulk-memory",
    "-mmutable-globals",
    "-msign-ext",
    "-mnontrapping-fptoint",
    "-ftls-model=local-exec",
    "-DHALO_IOS=1",
]

# The guest's linear memory: as the web build's, the Xbox window
# (0x80000000, port/linux/src/platform.h) is its top 128 MB.
MEMORY_BYTES = 0x88000000
MAIN_STACK_BYTES = 8 * 1024 * 1024


def run(command, **kwargs):
    print("+", " ".join(shlex.quote(str(c)) for c in command), flush=True)
    subprocess.run([str(c) for c in command], check=True, **kwargs)


def make_fake_ndk(root: Path, gl_include: Path) -> Path:
    """What configure.py's Android graph needs of an NDK: its LLVM tools and
    the OpenGL ES headers of its sysroot (the guest's GL declarations)."""
    tag = "darwin-x86_64" if sys.platform == "darwin" else "linux-x86_64"
    toolchain = root / "toolchains" / "llvm" / "prebuilt" / tag
    (toolchain / "bin").mkdir(parents=True, exist_ok=True)
    include = toolchain / "sysroot" / "usr" / "include"
    include.mkdir(parents=True, exist_ok=True)
    for name in ("GLES2", "GLES3", "KHR"):
        link = include / name
        if link.is_symlink() or link.exists():
            link.unlink()
        link.symlink_to((gl_include / name).resolve())
    (root / "build" / "cmake").mkdir(parents=True, exist_ok=True)
    (root / "build" / "cmake" / "android.toolchain.cmake").touch()
    return root


def configure(opence: Path, fake_ndk: Path) -> None:
    run([sys.executable, "configure.py", "--release", "--android-ndk", fake_ndk], cwd=opence)


def guest_commands(opence: Path):
    text = subprocess.run(["ninja", "-t", "commands", "build/android/halo_guest.elf"], cwd=opence,
                          capture_output=True, text=True, check=True).stdout
    commands = []
    for line in text.splitlines():
        if "android_asm_convert.py" not in line:
            continue
        first = shlex.split(line.split(" && ")[0])
        while first and not first[0].endswith("clang") and "clang" not in Path(first[0]).name:
            first = first[1:]
        source = first[first.index("-S") + 1]
        output = first[first.index("-o") + 1]
        commands.append((source, output, first[1:]))
    return commands


def rewrite(arguments, source):
    result = list(WASM_FLAGS)
    skip = False
    for argument in arguments:
        if skip:
            skip = False
            continue
        if argument in DROPPED:
            continue
        if argument in DROPPED_WITH_VALUE:
            skip = True
            continue
        if argument == source:
            continue
        if argument == "-fcommon":
            argument = "-fno-common"
        argument = argument.replace(ANDROID_ARCH, str(GUEST / "libc" / "arch" / "wasm32"))
        argument = argument.replace(ANDROID_SRC_INCLUDE, str(GUEST / "libc" / "src_include"))
        result.append(argument)
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--opence", type=Path, required=True)
    parser.add_argument("--wasi-sdk", type=Path, required=True)
    parser.add_argument("--gl-include", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    parser.add_argument("--skip-configure", action="store_true")
    args = parser.parse_args()

    opence = args.opence.resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    clang = args.wasi_sdk.resolve() / "bin" / "clang"
    wasm_ld = args.wasi_sdk.resolve() / "bin" / "wasm-ld"
    llvm_ar = args.wasi_sdk.resolve() / "bin" / "llvm-ar"
    builtins = sorted(args.wasi_sdk.resolve().glob("lib/clang/*/lib/wasm32-unknown-wasip1-threads/libclang_rt.builtins*.a")) or \
        sorted(args.wasi_sdk.resolve().glob("lib/clang/*/lib/wasm32-unknown-wasi/libclang_rt.builtins*.a"))
    if not builtins:
        raise SystemExit("wasi-sdk has no compiler-rt builtins for wasm32")

    if not args.skip_configure:
        configure(opence, make_fake_ndk(out / "fake-ndk", args.gl_include.resolve()))

    commands = guest_commands(opence)
    if not commands:
        raise SystemExit("configure.py made no Android guest graph (is musl/SDL3 fetched?)")

    # the Android graph's generated headers and sources the guest compiles need
    generated = sorted({source for source, _, _ in commands if source.startswith("build/")})
    headers = [
        "build/android/guest/libc_include/bits/alltypes.h",
        "build/android/guest/libc_include/bits/syscall.h",
        "build/android/guest/libc_internal/version.h",
        "build/android/guest/gl_include/stamp",
        "build/linux/halo_msvc_semantics.h",
        "build/linux/platform_msvc_semantics.h",
    ]
    run(["ninja", "-j", str(args.jobs), *headers, *generated], cwd=opence)

    # the wasm32 graph
    lines = [
        "ninja_required_version = 1.7",
        f"clang = {clang}",
        f"opence = {opence}",
        "rule cc",
        "  command = cd $opence && $clang --no-default-config $flags -MMD -MF $out.d -c $in -o $out",
        "  depfile = $out.d",
        "  deps = gcc",
        "  description = WASM CC $out",
        "rule as",
        "  command = $clang --no-default-config --target=wasm32-unknown-unknown -matomics -mbulk-memory "
        "-mmutable-globals -c $in -o $out",
        "  description = WASM AS $out",
        "rule ar",
        f"  command = rm -f $out && {llvm_ar} rcs $out @$out.rsp",
        "  rspfile = $out.rsp",
        "  rspfile_content = $in_newline",
        "  description = WASM AR $out",
        "rule link",
        f"  command = {wasm_ld} --no-entry --allow-undefined --shared-memory --import-memory "
        f"--initial-memory={MEMORY_BYTES} --max-memory={MEMORY_BYTES} -z stack-size={MAIN_STACK_BYTES} "
        "--stack-first --export=__wasm_call_ctors --export=__heap_base --export-table "
        "-Map $out.map -o $out @$out.rsp $libs",
        "  rspfile = $out.rsp",
        "  rspfile_content = $in_newline",
        "  description = WASM LINK $out",
    ]

    def esc(path):
        return str(path).replace("$", "$$").replace(" ", "$ ").replace(":", "$:")

    musl_objects, objects = [], []
    record = []
    for source, output, arguments in commands:
        flags = rewrite(arguments, source)
        source_path = Path(source)
        if source in REPLACED_RUNTIME:
            source_path = REPLACED_RUNTIME[source]
        src = source_path if source_path.is_absolute() else opence / source_path
        obj = out / "obj" / Path(output.replace("build/android/guest/obj/", "")).with_suffix(".o")
        lines.append(f"build {esc(obj)}: cc {esc(src)}")
        lines.append("  flags = " + " ".join(shlex.quote(f).replace("$", "$$") for f in flags))
        (musl_objects if "/musl/" in output else objects).append(obj)
        record.append({"source": str(src), "object": str(obj), "flags": flags})
    entry = out / "obj" / "ios" / "guest_thread_entry.o"
    lines.append(f"build {esc(entry)}: as {esc(GUEST / 'runtime' / 'guest_thread_entry.s')}")
    objects.append(entry)
    libguestc = out / "libguestc.a"
    lines.append(f"build {esc(libguestc)}: ar " + " ".join(esc(o) for o in musl_objects))
    wasm = out / "halo_guest.wasm"
    lines.append(f"build {esc(wasm)}: link " + " ".join(esc(o) for o in objects) + f" | {esc(libguestc)}")
    lines.append(f"  libs = {esc(libguestc)} {esc(builtins[0])}")
    lines.append(f"default {esc(wasm)}")
    (out / "build.ninja").write_text("\n".join(lines) + "\n")
    (out / "compile_commands_wasm.json").write_text(json.dumps(record, indent=1))
    run(["ninja", "-j", str(args.jobs), "-k", "0"], cwd=out)
    print(f"guest module: {wasm} ({wasm.stat().st_size // 1024} KB)")


if __name__ == "__main__":
    main()
