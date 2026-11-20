/*
GUEST_START_WASM.C

Entry points of the iOS port's guest module: the game compiled for wasm32
(the Android guest's sources and ABI, port/android/guest), translated to C
by wasm2c and compiled into the app. The host (ios/host) creates the first
instance of the module on its main thread and calls guest_start; each
further thread gets an instance of its own over the same shared memory
(ios/host/host_wasm.c).

There is no image header here: the host finds these functions as the
module's exports, and the module's imports (every undefined function, from
"env") are the host services of port/android/guest/runtime/guest_host.h,
the GL functions (hostgl_*) and the platform layer's posix_* calls
(hostposix_*).
*/

#include "pthread_impl.h"
#include "libc.h"
#include "guest_host.h"
#include "halo_android_abi.h"
#include "android_render_policy.h"

#include <stdlib.h>
#include <string.h>

/* wasm-ld's: the module's constructors */
extern void __wasm_call_ctors(void);

void __guest_thread_initialize_main(void);

extern char **__environ;

/* the game's entry point (source/main/main.c) */
extern int main(int argc, char **argv);

__attribute__((export_name("guest_start")))
void guest_start(const struct halo_guest_boot *boot)
{
	int result;

	libc.page_size = boot->page_size;
	libc.auxv = (size_t *)"\0\0\0\0\0\0\0";
	__environ = (char **)boot->environment;
	__guest_thread_initialize_main();
	__wasm_call_ctors();
	android_render_policy_initialize();

	result = main((int)boot->argc, (char **)boot->argv);
	exit(result);
}

/* guest memory for the host: the boot block, argv, the strings it copies in */
__attribute__((export_name("guest_malloc")))
void *guest_malloc(unsigned int size)
{
	return malloc(size);
}

__attribute__((export_name("guest_free")))
void guest_free(void *pointer)
{
	free(pointer);
}

/* SDL's audio callback (host_sdl_open_audio_stream): void (*)(void *userdata,
unsigned int stream, int additional, int total), through the table */
__attribute__((export_name("guest_call_audio")))
void guest_call_audio(unsigned int function, unsigned int userdata, unsigned int stream, int additional, int total)
{
	((void (*)(void *, unsigned int, int, int))function)((void *)userdata, stream, additional, total);
}
