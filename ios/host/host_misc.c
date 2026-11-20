/*
HOST_MISC.C

The smaller host services of the iOS port: errno in Linux's numbering, the
memory write tracking by page contents (as the web build and the Android
emulator do it, port/android/host/host_watch_hash.c), and the few posix.h
functions the host gives itself rather than port/linux/src/posix_files.c.
*/

#include "host.h"
#include "host_watch_hash.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------- errno */

int host_linux_errno(int value)
{
	if (value >= 0 && value <= 34 && value != 35)
		return value;
	switch (value)
	{
	case EAGAIN: return 11;
	case EINPROGRESS: return 115;
	case EALREADY: return 114;
	case ENOTSOCK: return 88;
	case EDESTADDRREQ: return 89;
	case EMSGSIZE: return 90;
	case EPROTOTYPE: return 91;
	case ENOPROTOOPT: return 92;
	case EPROTONOSUPPORT: return 93;
	case EOPNOTSUPP: return 95;
	case EAFNOSUPPORT: return 97;
	case EADDRINUSE: return 98;
	case EADDRNOTAVAIL: return 99;
	case ENETDOWN: return 100;
	case ENETUNREACH: return 101;
	case ECONNABORTED: return 103;
	case ECONNRESET: return 104;
	case ENOBUFS: return 105;
	case EISCONN: return 106;
	case ENOTCONN: return 107;
	case ETIMEDOUT: return 110;
	case ECONNREFUSED: return 111;
	case ELOOP: return 40;
	case ENAMETOOLONG: return 36;
	case EHOSTUNREACH: return 113;
	case ENOTEMPTY: return 39;
	case ENOSYS: return 38;
	case EOVERFLOW: return 75;
	case ECANCELED: return 125;
	case EILSEQ: return 84;
#if ENOTSUP != EOPNOTSUPP
	case ENOTSUP: return 95;
#endif
	default: return 5; /* EIO */
	}
}

int host_errno(void)
{
	return host_linux_errno(errno);
}

void host_set_errno_nosys(void)
{
	errno = ENOSYS;
}

/* ---------- posix.h, the host's own */

void host_posix_random_bytes(void *buffer, unsigned int size)
{
	arc4random_buf(buffer, size);
}

unsigned int host_posix_process_id(void)
{
	return (unsigned int)getpid();
}

int host_posix_command_line_argument(int index, char *buffer, unsigned int size)
{
	(void)index;
	if (size)
		buffer[0] = 0;
	return 0;
}

unsigned int host_posix_local_ipv4_address(void)
{
	return 0;
}

unsigned int host_posix_resolve_ipv4(const char *name)
{
	(void)name;
	return 0;
}

int host_posix_register_url_scheme(const char *scheme, const char *description)
{
	(void)scheme;
	(void)description;
	return 0;
}

/* ---------- the device */

int host_android_chip_family(void)
{
	/* (android_render_policy.h's "other": no workarounds of a GPU vendor's) */
	return 0;
}

/* ---------- write tracking (port/linux/src/memory_watch.c's interface) */

#define WATCH_PAGE_COUNT (HALO_GUEST_WINDOW_SIZE / WATCH_HASH_PAGE_SIZE)

static uint8_t page_watched[WATCH_PAGE_COUNT];
static uint64_t page_hash[WATCH_PAGE_COUNT];
static uint32_t page_hashed_frame[WATCH_PAGE_COUNT];
static uint32_t page_generation[WATCH_PAGE_COUNT];
static uint8_t page_unchanged[WATCH_PAGE_COUNT];
static volatile uint32_t current_generation = 1;
static pthread_mutex_t watch_lock = PTHREAD_MUTEX_INITIALIZER;
static struct watch_hash watch =
{
	NULL,
	WATCH_PAGE_COUNT,
	page_watched,
	page_hash,
	page_hashed_frame,
	page_generation,
	&current_generation,
	1,
	page_unchanged,
};

static int watch_pages(uint32_t address, uint32_t size, uint32_t *first, uint32_t *last)
{
	uint64_t start = address, end = (uint64_t)address + size;

	if (!size || end <= HALO_GUEST_WINDOW_BASE || start >= (uint64_t)HALO_GUEST_WINDOW_BASE + HALO_GUEST_WINDOW_SIZE)
		return 0;
	if (start < HALO_GUEST_WINDOW_BASE)
		start = HALO_GUEST_WINDOW_BASE;
	if (end > (uint64_t)HALO_GUEST_WINDOW_BASE + HALO_GUEST_WINDOW_SIZE)
		end = (uint64_t)HALO_GUEST_WINDOW_BASE + HALO_GUEST_WINDOW_SIZE;
	*first = (uint32_t)((start - HALO_GUEST_WINDOW_BASE) / WATCH_HASH_PAGE_SIZE);
	*last = (uint32_t)((end - 1 - HALO_GUEST_WINDOW_BASE) / WATCH_HASH_PAGE_SIZE);
	return 1;
}

void host_memory_watch_initialize(void)
{
	watch.base = host_guest_base + HALO_GUEST_WINDOW_BASE;
}

void host_memory_watch_protect(unsigned int address, unsigned int size)
{
	uint32_t first, last;

	if (!watch.base || !watch_pages(address, size, &first, &last))
		return;
	pthread_mutex_lock(&watch_lock);
	watch_hash_protect(&watch, first, last);
	pthread_mutex_unlock(&watch_lock);
}

unsigned int host_memory_watch_generation(unsigned int address, unsigned int size)
{
	uint32_t first, last, generation;

	if (!watch.base || !watch_pages(address, size, &first, &last))
		return 0;
	pthread_mutex_lock(&watch_lock);
	generation = watch_hash_generation(&watch, first, last);
	pthread_mutex_unlock(&watch_lock);
	return generation;
}

unsigned int host_memory_watch_serial(void)
{
	return current_generation;
}

void host_memory_watch_prepare_write(unsigned int address, unsigned int size)
{
	uint32_t first, last;

	if (!watch.base || !watch_pages(address, size, &first, &last))
		return;
	pthread_mutex_lock(&watch_lock);
	watch_hash_prepare_write(&watch, first, last);
	pthread_mutex_unlock(&watch_lock);
}

void host_memory_watch_forget(unsigned int address, unsigned int size)
{
	uint32_t first, last;

	if (!watch.base || !watch_pages(address, size, &first, &last))
		return;
	pthread_mutex_lock(&watch_lock);
	watch_hash_forget(&watch, first, last);
	pthread_mutex_unlock(&watch_lock);
}

void host_memory_watch_begin_frame(void)
{
	if (!watch.base)
		return;
	pthread_mutex_lock(&watch_lock);
	watch_hash_begin_frame(&watch);
	pthread_mutex_unlock(&watch_lock);
}
