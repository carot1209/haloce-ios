/*
HOST_WASM.C

The guest module's runtime on iOS.

- Memory: the module imports one shared memory of HOST_GUEST_MEMORY_BYTES
  (the Xbox window, 0x80000000, is its top 128 MB, as in the web build).
  wasm2c's runtime reserves it with mmap; pages are only backed once
  touched. Below the window the host hands out pages for the guest's mmap,
  thread stacks and the boot block (host_guest_map), above what the linker
  placed (the module's data and the main thread's stack, up to __heap_base).
- Threads: every thread that runs guest code has an instance of its own
  (its own stack pointer and TLS base globals, over the shared memory), as
  WebAssembly threads do. The first instance, the game's main thread's,
  initialises the memory (wasm-ld's __wasm_init_memory, its start function);
  the others find it done.
- Futexes: the guest's musl waits and wakes through SYS_futex
  (host_syscall.c), which lands here, as do memory.atomic.wait/notify
  (wasm2c's, which the iOS port adds).
- Traps: an out-of-bounds access, an unreachable or a bad indirect call in
  the guest stops the app with the reason.
*/

#include "host.h"
#include "guest.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

struct w2c_env
{
	int unused;
};

static struct w2c_env environment;
static wasm_rt_shared_memory_t guest_memory;
uint8_t *host_guest_base;

wasm_rt_shared_memory_t *w2c_env_memory(struct w2c_env *env)
{
	(void)env;
	return &guest_memory;
}

/* ---------- traps */

static const char *trap_name(wasm_rt_trap_t trap)
{
	switch (trap)
	{
	case WASM_RT_TRAP_OOB: return "out-of-bounds memory access (or exhausted stack)";
	case WASM_RT_TRAP_INT_OVERFLOW: return "integer overflow";
	case WASM_RT_TRAP_DIV_BY_ZERO: return "integer division by zero";
	case WASM_RT_TRAP_INVALID_CONVERSION: return "invalid conversion to integer";
	case WASM_RT_TRAP_UNREACHABLE: return "unreachable code";
	case WASM_RT_TRAP_CALL_INDIRECT: return "bad indirect call";
	case WASM_RT_TRAP_NULL_REF: return "null reference";
	case WASM_RT_TRAP_UNALIGNED: return "unaligned atomic access";
	default: return "trap";
	}
}

/* wasm2c's runtime calls this (WASM_RT_TRAP_HANDLER) for every trap */
void host_wasm_trap(wasm_rt_trap_t trap)
{
	host_fatal("The game stopped: %s in the game code (trap %d).", trap_name(trap), (int)trap);
}

/* ---------- guest pages below the window */

#define PAGE 0x1000u
#define WINDOW_BASE HALO_GUEST_WINDOW_BASE
#define WINDOW_END (HALO_GUEST_WINDOW_BASE + HALO_GUEST_WINDOW_SIZE)
#define PAGE_COUNT (WINDOW_BASE / PAGE)
/* the lowest address handed out: above the module's data and main stack
(checked against __heap_base at start-up) */
#define LOW_START 0x04000000u

static uint8_t page_used[PAGE_COUNT / 8];
static uint32_t page_hint = LOW_START / PAGE;
static pthread_mutex_t page_lock = PTHREAD_MUTEX_INITIALIZER;

static int page_is_used(uint32_t page)
{
	return page_used[page >> 3] & (1u << (page & 7));
}

static void page_set(uint32_t first, uint32_t count, int used)
{
	uint32_t page;

	for (page = first; page < first + count; page++)
	{
		if (used)
			page_used[page >> 3] |= (uint8_t)(1u << (page & 7));
		else
			page_used[page >> 3] &= (uint8_t)~(1u << (page & 7));
	}
}

/* fresh zero pages: whole host pages are replaced by new anonymous memory
(which also gives their RAM back), the edges cleared */
static void zero_range(uint32_t address, uint32_t size)
{
	uintptr_t host_page = (uintptr_t)getpagesize();
	uintptr_t start = (uintptr_t)(host_guest_base + address);
	uintptr_t end = start + size;
	uintptr_t inner_start = (start + host_page - 1) & ~(host_page - 1);
	uintptr_t inner_end = end & ~(host_page - 1);

	if (inner_end > inner_start + host_page)
	{
		memset((void *)start, 0, inner_start - start);
		if (mmap((void *)inner_start, inner_end - inner_start, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED)
		{
			memset((void *)inner_start, 0, inner_end - inner_start);
		}
		memset((void *)inner_end, 0, end - inner_end);
	}
	else
	{
		memset((void *)start, 0, size);
	}
}

uint32_t host_guest_map(uint32_t address, uint32_t size, int fixed)
{
	uint32_t count = (size + PAGE - 1) / PAGE;
	uint32_t first, page, run;

	if (!count)
		return 0;
	if (fixed)
	{
		/* the Xbox window is the guest's own to place things in */
		if (address >= WINDOW_BASE && (uint64_t)address + size <= WINDOW_END)
		{
			/* (fresh mappings read as zero, as the Android host's do) */
			zero_range(address, count * PAGE <= WINDOW_END - address ? count * PAGE : size);
			return address;
		}
		if (address < LOW_START || address % PAGE || (uint64_t)address + size > WINDOW_BASE)
			return 0;
		pthread_mutex_lock(&page_lock);
		page_set(address / PAGE, count, 1);
		pthread_mutex_unlock(&page_lock);
		zero_range(address, count * PAGE);
		return address;
	}
	pthread_mutex_lock(&page_lock);
	for (int pass = 0; pass < 2; pass++)
	{
		first = pass ? LOW_START / PAGE : page_hint;
		run = 0;
		for (page = first; page < PAGE_COUNT; page++)
		{
			if (page_is_used(page))
			{
				run = 0;
				continue;
			}
			if (++run == count)
			{
				uint32_t start = page + 1 - count;

				page_set(start, count, 1);
				page_hint = page + 1;
				pthread_mutex_unlock(&page_lock);
				zero_range(start * PAGE, count * PAGE);
				return start * PAGE;
			}
		}
	}
	pthread_mutex_unlock(&page_lock);
	return 0;
}

void host_guest_unmap(uint32_t address, uint32_t size)
{
	uint32_t count = (size + PAGE - 1) / PAGE;

	if (address < LOW_START || (uint64_t)address + size > WINDOW_BASE)
		return;
	pthread_mutex_lock(&page_lock);
	page_set(address / PAGE, count, 0);
	if (address / PAGE < page_hint)
		page_hint = address / PAGE;
	pthread_mutex_unlock(&page_lock);
}

/* ---------- futexes */

#define FUTEX_BUCKETS 256

struct futex_waiter
{
	uint32_t address;
	int woken;
	struct futex_waiter *next;
};

struct futex_bucket
{
	pthread_mutex_t lock;
	pthread_cond_t wake;
	struct futex_waiter *waiters;
};

static struct futex_bucket futex_buckets[FUTEX_BUCKETS];

static struct futex_bucket *futex_bucket(uint32_t address)
{
	return &futex_buckets[(address >> 2) * 2654435761u % FUTEX_BUCKETS];
}

/* 0 when woken, -EAGAIN if the word was not expected, -ETIMEDOUT */
int host_futex_wait(uint32_t address, uint32_t expected, int64_t timeout_ns)
{
	struct futex_bucket *bucket = futex_bucket(address);
	struct futex_waiter waiter = { address, 0, NULL };
	struct futex_waiter **link;
	struct timespec deadline;
	int result = 0;

	if (address & 3 || (uint64_t)address + 4 > HOST_GUEST_MEMORY_BYTES)
		return -EINVAL;
	if (timeout_ns >= 0)
	{
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_sec += timeout_ns / 1000000000;
		deadline.tv_nsec += timeout_ns % 1000000000;
		if (deadline.tv_nsec >= 1000000000)
		{
			deadline.tv_sec++;
			deadline.tv_nsec -= 1000000000;
		}
	}
	pthread_mutex_lock(&bucket->lock);
	if (atomic_load((_Atomic uint32_t *)(host_guest_base + address)) != expected)
	{
		pthread_mutex_unlock(&bucket->lock);
		return -EAGAIN;
	}
	waiter.next = bucket->waiters;
	bucket->waiters = &waiter;
	while (!waiter.woken)
	{
		int error = timeout_ns >= 0 ? pthread_cond_timedwait(&bucket->wake, &bucket->lock, &deadline) :
			pthread_cond_wait(&bucket->wake, &bucket->lock);

		if (error == ETIMEDOUT && !waiter.woken)
		{
			result = -ETIMEDOUT;
			break;
		}
	}
	for (link = &bucket->waiters; *link; link = &(*link)->next)
	{
		if (*link == &waiter)
		{
			*link = waiter.next;
			break;
		}
	}
	pthread_mutex_unlock(&bucket->lock);
	return result;
}

int host_futex_wake(uint32_t address, int count)
{
	struct futex_bucket *bucket = futex_bucket(address);
	struct futex_waiter *waiter;
	int woken = 0;

	pthread_mutex_lock(&bucket->lock);
	for (waiter = bucket->waiters; waiter && woken < count; waiter = waiter->next)
	{
		if (waiter->address == address && !waiter->woken)
		{
			waiter->woken = 1;
			woken++;
		}
	}
	if (woken)
		pthread_cond_broadcast(&bucket->wake);
	pthread_mutex_unlock(&bucket->lock);
	return woken;
}

static void futex_initialize(void)
{
	int index;

	for (index = 0; index < FUTEX_BUCKETS; index++)
	{
		pthread_mutex_init(&futex_buckets[index].lock, NULL);
		pthread_cond_init(&futex_buckets[index].wake, NULL);
	}
}

/* memory.atomic.wait32/64 and notify, which wasm2c (as the iOS port patches
it) leaves to the embedder: 0 "ok", 1 "not-equal", 2 "timed-out" */
uint32_t wasm_rt_atomic_wait32(wasm_rt_shared_memory_t *mem, uint64_t addr, uint32_t expected, int64_t timeout)
{
	int result;

	(void)mem;
	result = host_futex_wait((uint32_t)addr, expected, timeout);
	return result == -EAGAIN ? 1 : result == -ETIMEDOUT ? 2 : 0;
}

uint32_t wasm_rt_atomic_wait64(wasm_rt_shared_memory_t *mem, uint64_t addr, uint64_t expected, int64_t timeout)
{
	(void)mem;
	if (atomic_load((_Atomic uint64_t *)(host_guest_base + addr)) != expected)
		return 1;
	return wasm_rt_atomic_wait32(mem, addr, (uint32_t)expected, timeout);
}

uint32_t wasm_rt_atomic_notify(wasm_rt_shared_memory_t *mem, uint64_t addr, uint32_t count)
{
	(void)mem;
	return (uint32_t)host_futex_wake((uint32_t)addr, count > 0x7fffffff ? 0x7fffffff : (int)count);
}

/* ---------- threads */

struct host_thread
{
	w2c_guest instance;
	/* the guest's struct pthread for this thread (host_get_tp) */
	uint32_t thread_pointer;
	uint32_t stack, stack_size;
	/* host_native_thread_create's */
	void *(*function)(void *);
	void *argument;
	/* host_thread_create's */
	uint32_t guest_thread;
};

static __thread struct host_thread *current_thread;
static pthread_mutex_t instance_lock = PTHREAD_MUTEX_INITIALIZER;

/* the module's imports of the thread pointer */
u32 w2c_env_host_get_tp(struct w2c_env *env)
{
	(void)env;
	return current_thread ? current_thread->thread_pointer : 0;
}

void w2c_env_host_set_tp(struct w2c_env *env, u32 thread)
{
	(void)env;
	if (current_thread)
		current_thread->thread_pointer = thread;
}

/* the guest stacks: 1 MB at least, as the Android host's */
#define GUEST_STACK_MINIMUM (1024u * 1024u)
/* the host side's C stack, on which the translated code runs: generous, as
each guest call is a C call there */
#define NATIVE_STACK_BYTES (32u * 1024u * 1024u)

static void instance_create(struct host_thread *thread)
{
	wasm_rt_init_thread();
	pthread_mutex_lock(&instance_lock);
	wasm2c_guest_instantiate(&thread->instance, &environment);
	pthread_mutex_unlock(&instance_lock);
	current_thread = thread;
}

static void instance_destroy(struct host_thread *thread)
{
	current_thread = NULL;
	wasm2c_guest_free(&thread->instance);
	wasm_rt_free_thread();
	if (thread->stack)
		host_guest_unmap(thread->stack, thread->stack_size);
	free(thread);
}

static int stack_allocate(struct host_thread *thread, size_t size)
{
	if (size < GUEST_STACK_MINIMUM)
		size = GUEST_STACK_MINIMUM;
	size = (size + PAGE - 1) & ~(size_t)(PAGE - 1);
	thread->stack = host_guest_map(0, (uint32_t)size, 0);
	thread->stack_size = (uint32_t)size;
	return thread->stack != 0;
}

static int native_thread_start(void *(*body)(void *), struct host_thread *thread)
{
	pthread_attr_t attributes;
	pthread_t handle;
	int error;

	pthread_attr_init(&attributes);
	pthread_attr_setstacksize(&attributes, NATIVE_STACK_BYTES);
	pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
	error = pthread_create(&handle, &attributes, body, thread);
	pthread_attr_destroy(&attributes);
	return error;
}

/* a guest pthread (guest_thread_wasm.c's pthread_create) */
static void *guest_thread_body(void *context)
{
	struct host_thread *thread = context;

	pthread_setname_np("halo guest");
	instance_create(thread);
	w2c_guest_guest_thread_entry(&thread->instance, thread->guest_thread, thread->stack + thread->stack_size);
	instance_destroy(thread);
	return NULL;
}

u32 w2c_env_host_thread_create(struct w2c_env *env, u32 guest_thread, u32 stack_size)
{
	struct host_thread *thread = calloc(1, sizeof(*thread));
	int error;

	(void)env;
	if (!thread)
		return ENOMEM;
	thread->guest_thread = guest_thread;
	if (!stack_allocate(thread, stack_size))
	{
		free(thread);
		return ENOMEM;
	}
	error = native_thread_start(guest_thread_body, thread);
	if (error)
	{
		host_guest_unmap(thread->stack, thread->stack_size);
		free(thread);
		return (u32)host_linux_errno(error);
	}
	return 0;
}

/* a host thread that calls guest code (SDL's audio callback, host_sdl.c) */
static void *native_thread_body(void *context)
{
	struct host_thread *thread = context;

	instance_create(thread);
	w2c_guest_guest_attach_entry(&thread->instance, thread->stack + thread->stack_size);
	thread->function(thread->argument);
	w2c_guest_guest_thread_detach(&thread->instance);
	instance_destroy(thread);
	return NULL;
}

int host_native_thread_create(void *(*function)(void *), void *argument, size_t stack_size)
{
	struct host_thread *thread = calloc(1, sizeof(*thread));
	int error;

	if (!thread)
		return ENOMEM;
	thread->function = function;
	thread->argument = argument;
	if (!stack_allocate(thread, stack_size))
	{
		free(thread);
		return ENOMEM;
	}
	error = native_thread_start(native_thread_body, thread);
	if (error)
	{
		host_guest_unmap(thread->stack, thread->stack_size);
		free(thread);
	}
	return error;
}

uint32_t host_call_guest(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	if (!current_thread)
		host_fatal("guest code called on a thread without an instance");
	w2c_guest_guest_call_audio(&current_thread->instance, function, a, b, c, d);
	return 0;
}

/* ---------- start-up */

static void *main_thread_body(void *context)
{
	struct host_thread *thread = context;
	uint32_t heap_base;

	pthread_setname_np("halo main");
	instance_create(thread);
	heap_base = *w2c_guest_0x5F_heap_base(&thread->instance);
	if (heap_base > LOW_START)
		host_fatal("the game module's data (%08x) reaches into the memory the host hands out", heap_base);
	w2c_guest_guest_start(&thread->instance, thread->guest_thread);
	/* guest_start ends with exit(), which ends the process */
	host_exit(0);
}

void host_start_guest_main(uint32_t boot)
{
	struct host_thread *thread = calloc(1, sizeof(*thread));

	if (!thread)
		host_fatal("out of memory");
	thread->guest_thread = boot;
	if (native_thread_start(main_thread_body, thread) != 0)
		host_fatal("cannot start the game's thread");
}

/* called once, on the app's main thread, before anything else here */
void host_wasm_initialize(void)
{
	uint64_t pages = HOST_GUEST_MEMORY_BYTES / 65536;

	futex_initialize();
	wasm_rt_init();
	wasm_rt_allocate_memory_shared(&guest_memory, pages, pages, false, 65536);
	host_guest_base = (uint8_t *)guest_memory.data;
	if (!host_guest_base)
		host_fatal("cannot reserve the game's memory (%llu MB)", HOST_GUEST_MEMORY_BYTES >> 20);
	/* the module's data and the main thread's stack (host pages below
	LOW_START are never handed out) */
	page_set(0, LOW_START / PAGE, 1);
}
