#!/bin/bash
# Builds and runs the Linux harness of the iOS host runtime (smoke_main.c).
# usage: run.sh OPENCE W2C WABT DATA_ROOT [frames]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
IOS="$HERE/../../ios"
UP="$1"; W2C="$2"; WABT="$3"; DATA="$4"; FRAMES="${5:-120}"
OUT="${SMOKE_OUT:-/tmp/haloce-smoke}"
mkdir -p "$OUT/obj" "$OUT/shim/OpenGLES/ES3"
printf '#include <GLES3/gl3.h>\n' > "$OUT/shim/OpenGLES/ES3/gl.h"
printf '#include <GLES2/gl2ext.h>\n' > "$OUT/shim/OpenGLES/ES3/glext.h"
RT="${EXTRA_CFLAGS:-} -DWASM_RT_MEMCHECK_BOUNDS_CHECK=1 -DWASM_RT_USE_MMAP=1 -DWASM_RT_NONCONFORMING_UNCHECKED_STACK_EXHAUSTION=1 -DWASM_RT_TRAP_HANDLER=host_wasm_trap"
INC="-I$OUT/shim -I$IOS/host -I$W2C -I$WABT/wasm2c -I$UP/port/android/include -I$UP/port/android/host -I$UP/port/linux/src -I$UP/build/android/third_party/SDL3/include"
CC="${CC:-clang}"
OPT="${OPT:--O1}"
objs=()
build() { local src="$1"; shift; local obj="$OUT/obj/$(basename "$(dirname "$src")")_$(basename "$src").o"
  if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then echo "CC $src"; "$CC" $OPT -g -c "$@" "$src" -o "$obj"; fi; objs+=("$obj"); }
export -f build 2>/dev/null || true
for f in "$W2C"/guest_*.c "$WABT/wasm2c/wasm-rt-impl.c" "$WABT/wasm2c/wasm-rt-mem-impl.c"; do
  build "$f" $RT -I"$W2C" -I"$WABT/wasm2c" -w &
  while [ "$(jobs -r | wc -l)" -ge "${JOBS:-3}" ]; do sleep 0.2; done
done
wait
objs=()
for f in "$W2C"/guest_*.c "$WABT/wasm2c/wasm-rt-impl.c" "$WABT/wasm2c/wasm-rt-mem-impl.c"; do
  objs+=("$OUT/obj/$(basename "$(dirname "$f")")_$(basename "$f").o"); done
for f in "$W2C/ios_imports.c" "$IOS/host/host_wasm.c" "$IOS/host/host_syscall.c" "$IOS/host/host_misc.c" \
         "$IOS/host/host_gl.c" "$IOS/host/host_xiso.c" "$UP/port/android/host/host_watch_hash.c" "$HERE/smoke_main.c"; do
  build "$f" $RT $INC -include "$HERE/compat.h" -w
done
build "$UP/port/linux/src/posix_files.c" -w
"$CC" -g -o "$OUT/smoke" "${objs[@]}" -lEGL -lGLESv2 -lpthread -lm
echo "== run"
exec "$OUT/smoke" "$DATA" "$FRAMES"
