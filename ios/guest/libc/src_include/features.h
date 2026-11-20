/* musl's internal features.h as it is: WebAssembly objects take alias
attributes, so weak_alias needs none of the Android guest's assembler
spelling (port/android/guest/libc/src_include/features.h) */
#include_next "features.h"
