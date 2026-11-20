/*
HOST.H

Internals of the iOS port's host: the app around the guest module (the
game compiled to wasm32 and translated to C by wasm2c, ios/README.md).

The guest's addresses are offsets into its linear memory, a single shared
WebAssembly memory every thread's instance of the module uses; GP() turns
one into a host pointer. Everything else follows the Android host
(port/android/host in OpenCE), whose services the guest imports with the
same names and meanings (port/android/guest/runtime/guest_host.h).
*/

#ifndef HALO_IOS_HOST_H
#define HALO_IOS_HOST_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "halo_android_abi.h"

/* ---------- guest memory (host_wasm.c) */

/* the base of the guest's linear memory, and its size */
extern uint8_t *host_guest_base;
#define HOST_GUEST_MEMORY_BYTES 0x88000000ull

/* a guest address as a host pointer; 0 stays NULL */
#define GP(address) ((void *)((uint32_t)(address) ? host_guest_base + (uint32_t)(address) : NULL))
/* a host pointer into guest memory as a guest address */
#define GA(pointer) ((uint32_t)((const uint8_t *)(pointer) - host_guest_base))

/* page-granular guest memory below the Xbox window, for the guest's mmap
and the host's own needs (thread stacks, the boot block); 0 on failure */
uint32_t host_guest_map(uint32_t address, uint32_t size, int fixed);
void host_guest_unmap(uint32_t address, uint32_t size);
/* the guest's mmap/munmap/mprotect/madvise (host_syscall.c) */
long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset);
long host_guest_munmap(uint64_t address, uint64_t size);

/* ---------- futexes (host_wasm.c): guest addresses */
int host_futex_wait(uint32_t address, uint32_t expected, int64_t timeout_ns);
int host_futex_wake(uint32_t address, int count);

/* ---------- threads and guest calls (host_wasm.c) */
/* sets up the guest's memory (once, first of all) */
void host_wasm_initialize(void);
/* starts the game's main thread, which runs the guest's main with boot (a
struct halo_guest_boot in guest memory) */
void host_start_guest_main(uint32_t boot);
/* starts a host thread that can call guest code (it gets an instance of
the module and a guest thread); 0 or an errno value */
int host_native_thread_create(void *(*function)(void *), void *argument, size_t stack_size);
/* calls a guest function of type void (*)(void *, unsigned int, int, int)
on this thread (one host_native_thread_create made): SDL's audio callback */
uint32_t host_call_guest(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d);

/* ---------- write tracking (host_misc.c): call once the memory exists */
void host_memory_watch_initialize(void);

/* ---------- logging and termination (host_main.m) */
void host_logf(int priority, const char *format, ...) __attribute__((format(printf, 2, 3)));
void host_log(int priority, const char *text);
#define HOST_LOG_INFO 4
#define HOST_LOG_WARN 5
#define HOST_LOG_ERROR 6
void host_fatal(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));
void host_exit(int code) __attribute__((noreturn));
void host_abort(const char *reason) __attribute__((noreturn));

/* the Linux value of a Darwin errno (the guest's musl and the game speak
Linux) */
int host_linux_errno(int darwin_errno);
int host_errno(void);

/* ---------- SDL on the main thread (host_main.m) */
/* runs function(argument) on the main thread and waits */
void host_on_main_thread(void (*function)(void *), void *argument);
/* the events SDL delivered to the app, for the guest (host_sdl.c) */
int host_event_pop(void *event);
/* 1 while the app is in the background: no GL work then */
int host_in_background(void);
void host_wait_foreground(void);

/* ---------- the on-screen controls (host_touch.m) */
/* puts the overlay over the SDL window's view (main thread) */
void host_touch_install(void *sdl_window);

/* ---------- the disc image (host_xiso.c) */
typedef void (*host_xiso_progress)(void *context, unsigned long long done, unsigned long long total);
/* copies the image's maps folder into destination/maps; 1, or 0 with the
reason in error */
int host_xiso_extract_maps(const char *image, const char *destination, host_xiso_progress progress,
	void *context, char *error, size_t error_size);

#endif
