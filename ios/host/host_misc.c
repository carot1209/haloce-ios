/*
HOST_MISC.C

The smaller host services of the iOS port: errno in Linux's numbering, the
memory write tracking by page contents (as the web build and the Android
emulator do it, port/android/host/host_watch_hash.c), and the few posix.h
functions the host gives itself rather than port/linux/src/posix_files.c.
*/

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
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

/* the local network's address and mask (network byte order): Wi-Fi's
(en0) first, else the first IPv4 interface up with broadcasts (a hotspot's
bridge), not loopback; 0 without one */
int host_local_network(uint32_t *address, uint32_t *mask)
{
	struct ifaddrs *list, *entry;
	int found = 0;

	*address = *mask = 0;
	if (getifaddrs(&list) != 0)
		return 0;
	for (int pass = 0; pass < 2 && !found; pass++)
	{
		for (entry = list; entry && !found; entry = entry->ifa_next)
		{
			if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET || !entry->ifa_netmask)
				continue;
			if (!(entry->ifa_flags & IFF_UP) || (entry->ifa_flags & IFF_LOOPBACK) || !(entry->ifa_flags & IFF_BROADCAST))
				continue;
			if (pass == 0 && strcmp(entry->ifa_name, "en0"))
				continue;
			*address = ((struct sockaddr_in *)entry->ifa_addr)->sin_addr.s_addr;
			*mask = ((struct sockaddr_in *)entry->ifa_netmask)->sin_addr.s_addr;
			found = 1;
		}
	}
	freeifaddrs(list);
	return found;
}

unsigned int host_posix_local_ipv4_address(void)
{
	uint32_t address, mask;

	return host_local_network(&address, &mask) ? address : 0;
}

unsigned int host_posix_resolve_ipv4(const char *name)
{
	struct addrinfo hints, *result = NULL;
	unsigned int address = 0;

	if (!name || !*name)
		return 0;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	if (getaddrinfo(name, NULL, &hints, &result) == 0 && result)
		address = ((struct sockaddr_in *)result->ai_addr)->sin_addr.s_addr;
	if (result)
		freeaddrinfo(result);
	return address;
}

int host_posix_register_url_scheme(const char *scheme, const char *description)
{
	(void)scheme;
	(void)description;
	return 0;
}

/* ---------- the device */

/* android_render_policy.h's families: what the renderer works around
(guest_render_policy.c). On Apple's GPU the geometry's mirror works (the
static buffers of the Adreno fix drew no level of a Custom Edition map),
but it has no S3TC, so Custom Edition maps' DXT textures must be converted
(their raw blocks drew as red and green stripes): the family that gives
just that is "mediatek". The Settings app's "Đồ hoạ tương thích" switch
takes an unknown chip's paths instead (both fixes). */
int host_android_chip_family(void)
{
	int compatible = 0;
#ifdef __APPLE__
	Boolean valid = false;

	compatible = (int)CFPreferencesGetAppBooleanValue(CFSTR("render_compat"), kCFPreferencesCurrentApplication, &valid);
	if (!valid)
		compatible = 0;
#endif
	return compatible ? 0 /* unknown */ : 2 /* mediatek: textures converted, geometry mirrored */;
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
	watch.base = GP(HALO_GUEST_WINDOW_BASE);
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
