/*
HOST_SYSCALL.C

The Linux system calls of the guest's musl (its syscall_arch.h sends every
one to the host_syscall import), performed on iOS.

The Android host passes most of them straight to its kernel. Darwin's
numbers, flags, structures and errno values differ, so here each call the
guest makes is done with Darwin's C library and its results put the way
Linux has them:

- addresses are guest addresses (GP), and structures are the guest's: the
  AArch64 kernel's struct stat (the guest arch's kstat.h), 32-bit timespec
  (the guest's time_t is 32-bit, as in the MSVC runtime), 32-bit iovec;
- open flags, AT_* values, fcntl commands and clock ids are translated;
- errors return -<Linux errno>;
- getdents64 reads a directory opened with fdopendir;
- mmap and friends allocate guest pages (host_wasm.c); futexes are the
  host's (host_wasm.c);
- standard output and error go to the log (host_main.m).
*/

#include "host.h"
#include "guest.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

/* the generic Linux system call numbers (AArch64's, the guest's arch) */
enum
{
	LINUX_getcwd = 17, LINUX_dup = 23, LINUX_dup3 = 24, LINUX_fcntl = 25, LINUX_ioctl = 29,
	LINUX_flock = 32, LINUX_mkdirat = 34, LINUX_unlinkat = 35, LINUX_renameat = 38,
	LINUX_statfs = 43, LINUX_fstatfs = 44, LINUX_ftruncate = 46, LINUX_faccessat = 48,
	LINUX_chdir = 49, LINUX_fchmod = 52, LINUX_fchmodat = 53, LINUX_openat = 56, LINUX_close = 57,
	LINUX_pipe2 = 59, LINUX_getdents64 = 61, LINUX_lseek = 62, LINUX_read = 63, LINUX_write = 64,
	LINUX_readv = 65, LINUX_writev = 66, LINUX_pread64 = 67, LINUX_pwrite64 = 68, LINUX_preadv = 69,
	LINUX_pwritev = 70, LINUX_pselect6 = 72, LINUX_ppoll = 73, LINUX_readlinkat = 78,
	LINUX_newfstatat = 79, LINUX_fstat = 80, LINUX_fsync = 82, LINUX_fdatasync = 83,
	LINUX_utimensat = 88, LINUX_exit = 93, LINUX_exit_group = 94, LINUX_set_tid_address = 96,
	LINUX_futex = 98, LINUX_nanosleep = 101, LINUX_clock_gettime = 113, LINUX_clock_getres = 114,
	LINUX_clock_nanosleep = 115, LINUX_sched_yield = 124, LINUX_kill = 129, LINUX_tkill = 130,
	LINUX_tgkill = 131, LINUX_sigaltstack = 132, LINUX_rt_sigaction = 134, LINUX_rt_sigprocmask = 135,
	LINUX_uname = 160, LINUX_getrlimit = 163, LINUX_umask = 166, LINUX_gettimeofday = 169,
	LINUX_getpid = 172, LINUX_getppid = 173, LINUX_getuid = 174, LINUX_geteuid = 175,
	LINUX_getgid = 176, LINUX_getegid = 177, LINUX_gettid = 178, LINUX_sysinfo = 179,
	LINUX_brk = 214, LINUX_munmap = 215, LINUX_mremap = 216, LINUX_mmap = 222, LINUX_mprotect = 226,
	LINUX_madvise = 233, LINUX_prlimit64 = 261, LINUX_renameat2 = 276, LINUX_getrandom = 278,
	LINUX_membarrier = 283, LINUX_statx = 291,
};

#define LINUX_AT_FDCWD (-100)
#define LINUX_AT_SYMLINK_NOFOLLOW 0x100
#define LINUX_AT_REMOVEDIR 0x200
#define LINUX_AT_EMPTY_PATH 0x1000

#define LINUX_ENOSYS 38
#define LINUX_EINVAL 22
#define LINUX_EFAULT 14
#define LINUX_ENOMEM 12
#define LINUX_EBADF 9
#define LINUX_ENOTTY 25

/* the guest's structures */
struct guest_timespec
{
	int32_t seconds;
	int32_t nanoseconds;
};

struct guest_iovec
{
	uint32_t base;
	uint32_t length;
};

/* port/android/guest/libc/arch/arm64_32/kstat.h */
struct guest_kstat
{
	uint64_t st_dev;
	uint64_t st_ino;
	uint32_t st_mode;
	uint32_t st_nlink;
	uint32_t st_uid;
	uint32_t st_gid;
	uint64_t st_rdev;
	uint64_t pad;
	int64_t st_size;
	int32_t st_blksize;
	int32_t pad2;
	int64_t st_blocks;
	int64_t st_atime_sec;
	int64_t st_atime_nsec;
	int64_t st_mtime_sec;
	int64_t st_mtime_nsec;
	int64_t st_ctime_sec;
	int64_t st_ctime_nsec;
	uint32_t unused[2];
};

struct guest_utsname
{
	char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};

static long failed(void)
{
	return -host_linux_errno(errno);
}

static long result_of(long value)
{
	return value == -1 ? failed() : value;
}

static int fd_in(int64_t fd)
{
	return (int)fd == LINUX_AT_FDCWD ? AT_FDCWD : (int)fd;
}

static int open_flags_in(int flags)
{
	int result = flags & 3;

	if (flags & 0100) result |= O_CREAT;
	if (flags & 0200) result |= O_EXCL;
	if (flags & 0400) result |= O_NOCTTY;
	if (flags & 01000) result |= O_TRUNC;
	if (flags & 02000) result |= O_APPEND;
	if (flags & 04000) result |= O_NONBLOCK;
	if (flags & 010000) result |= O_DSYNC;
	if (flags & 040000) result |= O_DIRECTORY;
	if (flags & 0100000) result |= O_NOFOLLOW;
	if (flags & 02000000) result |= O_CLOEXEC;
	if (flags & 04000000) result |= O_SYNC;
	return result;
}

static int open_flags_out(int flags)
{
	int result = flags & 3;

	if (flags & O_APPEND) result |= 02000;
	if (flags & O_NONBLOCK) result |= 04000;
	return result;
}

static int at_flags_in(int flags)
{
	int result = 0;

	if (flags & LINUX_AT_SYMLINK_NOFOLLOW) result |= AT_SYMLINK_NOFOLLOW;
	if (flags & LINUX_AT_REMOVEDIR) result |= AT_REMOVEDIR;
	return result;
}

static int timespec_in(uint32_t address, struct timespec *result)
{
	const struct guest_timespec *value = GP(address);

	if (!value)
		return 0;
	result->tv_sec = value->seconds;
	result->tv_nsec = value->nanoseconds;
	return 1;
}

static void timespec_out(uint32_t address, const struct timespec *value)
{
	struct guest_timespec *result = GP(address);

	if (!result)
		return;
	result->seconds = (int32_t)value->tv_sec;
	result->nanoseconds = (int32_t)value->tv_nsec;
}

static clockid_t clock_in(int64_t clock)
{
	switch ((int)clock)
	{
	case 0: case 5: return CLOCK_REALTIME;
	case 1: case 6: case 7: return CLOCK_MONOTONIC;
	case 2: return CLOCK_PROCESS_CPUTIME_ID;
	case 3: return CLOCK_THREAD_CPUTIME_ID;
	case 4: return CLOCK_MONOTONIC_RAW;
	default: return CLOCK_MONOTONIC;
	}
}

static void kstat_out(uint32_t address, const struct stat *information)
{
	struct guest_kstat *result = GP(address);

	if (!result)
		return;
	memset(result, 0, sizeof(*result));
	result->st_dev = (uint64_t)information->st_dev;
	result->st_ino = (uint64_t)information->st_ino;
	/* S_IF* and permission bits are the same */
	result->st_mode = information->st_mode;
	result->st_nlink = information->st_nlink;
	result->st_uid = information->st_uid;
	result->st_gid = information->st_gid;
	result->st_rdev = (uint64_t)information->st_rdev;
	result->st_size = information->st_size;
	result->st_blksize = information->st_blksize;
	result->st_blocks = information->st_blocks;
	result->st_atime_sec = information->st_atimespec.tv_sec;
	result->st_atime_nsec = information->st_atimespec.tv_nsec;
	result->st_mtime_sec = information->st_mtimespec.tv_sec;
	result->st_mtime_nsec = information->st_mtimespec.tv_nsec;
	result->st_ctime_sec = information->st_ctimespec.tv_sec;
	result->st_ctime_nsec = information->st_ctimespec.tv_nsec;
}

/* ---------- standard output and error */

static long log_write(int fd, const char *bytes, size_t size)
{
	static char lines[2][1024];
	static size_t lengths[2];
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	int which = fd == 2;
	size_t index;

	pthread_mutex_lock(&lock);
	for (index = 0; index < size; index++)
	{
		char c = bytes[index];

		if (c == '\n' || lengths[which] == sizeof(lines[0]) - 1)
		{
			lines[which][lengths[which]] = 0;
			host_log(which ? HOST_LOG_WARN : HOST_LOG_INFO, lines[which]);
			lengths[which] = 0;
			if (c == '\n')
				continue;
		}
		lines[which][lengths[which]++] = c;
	}
	pthread_mutex_unlock(&lock);
	return (long)size;
}

static int iovec_in(uint32_t vector, int count, struct iovec *result)
{
	const struct guest_iovec *guest_vector = GP(vector);
	int index;

	if (count < 0 || count > 64 || (count && !guest_vector))
		return 0;
	for (index = 0; index < count; index++)
	{
		result[index].iov_base = GP(guest_vector[index].base);
		result[index].iov_len = guest_vector[index].length;
	}
	return 1;
}

static long guest_writev(int fd, uint32_t vector, int count, int64_t offset, int positional)
{
	struct iovec host_vector[64];
	int index;

	if (!iovec_in(vector, count, host_vector))
		return -LINUX_EINVAL;
	if (fd == 1 || fd == 2)
	{
		long total = 0;

		for (index = 0; index < count; index++)
			total += log_write(fd, host_vector[index].iov_base, host_vector[index].iov_len);
		return total;
	}
	if (positional)
		return result_of(pwritev(fd, host_vector, count, offset));
	return result_of(writev(fd, host_vector, count));
}

static long guest_readv(int fd, uint32_t vector, int count, int64_t offset, int positional)
{
	struct iovec host_vector[64];

	if (!iovec_in(vector, count, host_vector))
		return -LINUX_EINVAL;
	if (positional)
		return result_of(preadv(fd, host_vector, count, offset));
	return result_of(readv(fd, host_vector, count));
}

/* ---------- directories: getdents64 over fdopendir */

#define DIRECTORY_SLOTS 64

static struct
{
	int fd;
	DIR *stream;
	struct dirent *pending;
} directories[DIRECTORY_SLOTS];
static pthread_mutex_t directory_lock = PTHREAD_MUTEX_INITIALIZER;

static void directory_forget(int fd)
{
	int index;

	pthread_mutex_lock(&directory_lock);
	for (index = 0; index < DIRECTORY_SLOTS; index++)
	{
		if (directories[index].stream && directories[index].fd == fd)
		{
			/* (closes the duplicate it holds) */
			closedir(directories[index].stream);
			directories[index].stream = NULL;
			directories[index].pending = NULL;
		}
	}
	pthread_mutex_unlock(&directory_lock);
}

static long guest_getdents64(int fd, uint32_t buffer, uint32_t size)
{
	uint8_t *out = GP(buffer);
	uint32_t used = 0;
	int index, slot = -1;

	if (!out)
		return -LINUX_EFAULT;
	pthread_mutex_lock(&directory_lock);
	for (index = 0; index < DIRECTORY_SLOTS; index++)
	{
		if (directories[index].stream && directories[index].fd == fd)
			slot = index;
	}
	if (slot < 0)
	{
		int duplicate = dup(fd);
		DIR *stream = duplicate >= 0 ? fdopendir(duplicate) : NULL;

		for (index = 0; stream && index < DIRECTORY_SLOTS; index++)
		{
			if (!directories[index].stream)
			{
				directories[index].fd = fd;
				directories[index].stream = stream;
				directories[index].pending = NULL;
				slot = index;
				break;
			}
		}
		if (slot < 0)
		{
			if (stream)
				closedir(stream);
			else if (duplicate >= 0)
				close(duplicate);
			pthread_mutex_unlock(&directory_lock);
			return -LINUX_EBADF;
		}
	}
	for (;;)
	{
		struct dirent *entry = directories[slot].pending ? directories[slot].pending :
			readdir(directories[slot].stream);
		size_t name_length, record;

		directories[slot].pending = NULL;
		if (!entry)
			break;
		name_length = strlen(entry->d_name);
		record = (19 + name_length + 1 + 7) & ~(size_t)7;
		if (used + record > size)
		{
			directories[slot].pending = entry;
			break;
		}
		{
			uint8_t *at = out + used;
			uint64_t inode = entry->d_ino;
			int64_t offset = used + record;
			uint16_t length = (uint16_t)record;

			memcpy(at, &inode, 8);
			memcpy(at + 8, &offset, 8);
			memcpy(at + 16, &length, 2);
			/* DT_* are the same */
			at[18] = entry->d_type;
			memcpy(at + 19, entry->d_name, name_length + 1);
			memset(at + 19 + name_length + 1, 0, record - 19 - name_length - 1);
		}
		used += (uint32_t)record;
	}
	pthread_mutex_unlock(&directory_lock);
	if (!used && directories[slot].pending)
		return -LINUX_EINVAL;
	return used;
}

/* ---------- futexes */

static long guest_futex(uint32_t address, int operation, uint32_t value, uint32_t timeout, uint32_t address2)
{
	int command = operation & 127;
	struct timespec relative;
	int64_t timeout_ns = -1;

	switch (command)
	{
	case 0: /* FUTEX_WAIT */
	case 9: /* FUTEX_WAIT_BITSET (absolute CLOCK_MONOTONIC) */
		if (timespec_in(timeout, &relative))
		{
			timeout_ns = (int64_t)relative.tv_sec * 1000000000 + relative.tv_nsec;
			if (command == 9)
			{
				struct timespec now;

				clock_gettime(CLOCK_MONOTONIC, &now);
				timeout_ns -= (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
				if (timeout_ns < 0)
					timeout_ns = 0;
			}
		}
		{
			int result = host_futex_wait(address, value, timeout_ns);

			/* -ETIMEDOUT, -EAGAIN, -EINVAL as Linux numbers them */
			return result == -ETIMEDOUT ? -110 : result == -EAGAIN ? -11 : result == -EINVAL ? -22 : 0;
		}
	case 1: /* FUTEX_WAKE */
	case 10: /* FUTEX_WAKE_BITSET */
		return host_futex_wake(address, (int)value);
	case 3: /* FUTEX_REQUEUE: waking all of them is allowed */
	case 4: /* FUTEX_CMP_REQUEUE */
		return host_futex_wake(address, 0x7fffffff) + host_futex_wake(address2, 0x7fffffff);
	default:
		return -LINUX_ENOSYS;
	}
}

/* ---------- dispatch */

u64 w2c_env_host_syscall(struct w2c_env *env, u64 number, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f)
{
	(void)env;
	(void)f;
	switch ((int)number)
	{
	case LINUX_write:
		if ((int)a == 1 || (int)a == 2)
			return log_write((int)a, GP(b), (uint32_t)c);
		return result_of(write((int)a, GP(b), (uint32_t)c));
	case LINUX_read:
		return result_of(read((int)a, GP(b), (uint32_t)c));
	case LINUX_pread64:
		return result_of(pread((int)a, GP(b), (uint32_t)c, (off_t)d));
	case LINUX_pwrite64:
		return result_of(pwrite((int)a, GP(b), (uint32_t)c, (off_t)d));
	case LINUX_writev:
		return guest_writev((int)a, (uint32_t)b, (int)c, 0, 0);
	case LINUX_pwritev:
		return guest_writev((int)a, (uint32_t)b, (int)c, (int64_t)d, 1);
	case LINUX_readv:
		return guest_readv((int)a, (uint32_t)b, (int)c, 0, 0);
	case LINUX_preadv:
		return guest_readv((int)a, (uint32_t)b, (int)c, (int64_t)d, 1);
	case LINUX_openat:
		return result_of(openat(fd_in(a), GP(b), open_flags_in((int)c), (mode_t)d));
	case LINUX_close:
		directory_forget((int)a);
		return result_of(close((int)a));
	case LINUX_lseek:
		return result_of((long)lseek((int)a, (off_t)(int64_t)b, (int)c));
	case LINUX_getdents64:
		return guest_getdents64((int)a, (uint32_t)b, (uint32_t)c);
	case LINUX_fstat:
	{
		struct stat information;

		if (fstat((int)a, &information) != 0)
			return failed();
		kstat_out((uint32_t)b, &information);
		return 0;
	}
	case LINUX_newfstatat:
	{
		struct stat information;
		const char *path = GP(b);
		int result;

		if (((int)d & LINUX_AT_EMPTY_PATH) && (!path || !*path))
			result = fstat((int)a, &information);
		else
			result = fstatat(fd_in(a), path, &information, at_flags_in((int)d));
		if (result != 0)
			return failed();
		kstat_out((uint32_t)c, &information);
		return 0;
	}
	case LINUX_unlinkat:
		return result_of(unlinkat(fd_in(a), GP(b), at_flags_in((int)c)));
	case LINUX_renameat:
	case LINUX_renameat2:
		return result_of(renameat(fd_in(a), GP(b), fd_in(c), GP(d)));
	case LINUX_mkdirat:
		return result_of(mkdirat(fd_in(a), GP(b), (mode_t)c));
	case LINUX_fchmod:
		return result_of(fchmod((int)a, (mode_t)b));
	case LINUX_fchmodat:
		return result_of(fchmodat(fd_in(a), GP(b), (mode_t)c, 0));
	case LINUX_ftruncate:
		return result_of(ftruncate((int)a, (off_t)(int64_t)b));
	case LINUX_fsync:
	case LINUX_fdatasync:
		return result_of(fsync((int)a));
	case LINUX_fcntl:
		switch ((int)b)
		{
		case 0: return result_of(fcntl((int)a, F_DUPFD, (int)c));
		case 1030: return result_of(fcntl((int)a, F_DUPFD_CLOEXEC, (int)c));
		case 1: return result_of(fcntl((int)a, F_GETFD));
		case 2: return result_of(fcntl((int)a, F_SETFD, (int)c));
		case 3:
		{
			int flags = fcntl((int)a, F_GETFL);

			return flags < 0 ? failed() : open_flags_out(flags);
		}
		case 4: return result_of(fcntl((int)a, F_SETFL, open_flags_in((int)c) & (O_APPEND | O_NONBLOCK)));
		default: return -LINUX_EINVAL;
		}
	case LINUX_getcwd:
		return getcwd(GP(a), (size_t)(uint32_t)b) ? (long)strlen(GP(a)) + 1 : failed();
	case LINUX_chdir:
		return result_of(chdir(GP(a)));
	case LINUX_readlinkat:
		return result_of(readlinkat(fd_in(a), GP(b), GP(c), (size_t)(uint32_t)d));
	case LINUX_faccessat:
		return result_of(faccessat(fd_in(a), GP(b), (int)c, 0));
	case LINUX_dup:
		return result_of(dup((int)a));
	case LINUX_dup3:
	{
		int result = dup2((int)a, (int)b);

		if (result >= 0 && ((int)c & 02000000))
			fcntl(result, F_SETFD, FD_CLOEXEC);
		return result_of(result);
	}
	case LINUX_pipe2:
	{
		int descriptors[2];
		int32_t *out = GP(a);

		if (pipe(descriptors) != 0)
			return failed();
		out[0] = descriptors[0];
		out[1] = descriptors[1];
		return 0;
	}
	case LINUX_flock:
		return result_of(flock((int)a, (int)b));
	case LINUX_umask:
		return umask((mode_t)a);
	case LINUX_ioctl:
		return -LINUX_ENOTTY;
	case LINUX_utimensat:
	{
		struct timespec times[2];
		const struct guest_timespec *guest_times = GP(c);
		int index;

		for (index = 0; guest_times && index < 2; index++)
		{
			times[index].tv_sec = guest_times[index].seconds;
			times[index].tv_nsec = guest_times[index].nanoseconds;
			/* UTIME_NOW, UTIME_OMIT */
			if (guest_times[index].nanoseconds == (1 << 30) - 1)
				times[index].tv_nsec = UTIME_NOW;
			else if (guest_times[index].nanoseconds == (1 << 30) - 2)
				times[index].tv_nsec = UTIME_OMIT;
		}
		return result_of(utimensat(fd_in(a), GP(b), guest_times ? times : NULL, at_flags_in((int)d)));
	}

	case LINUX_clock_gettime:
	case LINUX_clock_getres:
	{
		struct timespec value;
		int result = (int)number == LINUX_clock_gettime ? clock_gettime(clock_in(a), &value) :
			clock_getres(clock_in(a), &value);

		if (result != 0)
			return failed();
		timespec_out((uint32_t)b, &value);
		return 0;
	}
	case LINUX_gettimeofday:
	{
		struct timeval value;
		struct guest_timespec *result = GP(a);

		gettimeofday(&value, NULL);
		if (result)
		{
			result->seconds = (int32_t)value.tv_sec;
			result->nanoseconds = (int32_t)value.tv_usec;
		}
		return 0;
	}
	case LINUX_nanosleep:
	{
		struct timespec request, remaining;
		long result;

		if (!timespec_in((uint32_t)a, &request))
			return -LINUX_EFAULT;
		result = result_of(nanosleep(&request, &remaining));
		if (result == -4)
			timespec_out((uint32_t)b, &remaining);
		return result;
	}
	case LINUX_clock_nanosleep:
	{
		struct timespec request;

		if (!timespec_in((uint32_t)c, &request))
			return -LINUX_EFAULT;
		if ((int)b & 1)
		{
			/* TIMER_ABSTIME */
			struct timespec now;
			int64_t delta;

			clock_gettime(clock_in(a), &now);
			delta = ((int64_t)request.tv_sec - now.tv_sec) * 1000000000 + (request.tv_nsec - now.tv_nsec);
			if (delta <= 0)
				return 0;
			request.tv_sec = delta / 1000000000;
			request.tv_nsec = delta % 1000000000;
		}
		return nanosleep(&request, NULL) == 0 ? 0 : -4;
	}
	case LINUX_futex:
		return guest_futex((uint32_t)a, (int)b, (uint32_t)c, (uint32_t)d, (uint32_t)e);
	case LINUX_ppoll:
	{
		struct timespec timeout;
		int milliseconds = timespec_in((uint32_t)c, &timeout) ?
			(int)(timeout.tv_sec * 1000 + timeout.tv_nsec / 1000000) : -1;

		/* struct pollfd and the POLL* bits are the same */
		return result_of(poll(GP(a), (nfds_t)(uint32_t)b, milliseconds));
	}
	case LINUX_sched_yield:
		sched_yield();
		return 0;

	case LINUX_mmap:
		return host_guest_mmap(a, b, (int)c, (int)d, (int)e, (int64_t)f);
	case LINUX_munmap:
		return host_guest_munmap(a, b);
	case LINUX_mprotect:
	case LINUX_madvise:
		return 0;
	case LINUX_mremap:
	case LINUX_brk:
		/* musl then maps anew and copies */
		return -LINUX_ENOMEM;
	case LINUX_membarrier:
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		return 0;

	case LINUX_exit:
	case LINUX_exit_group:
		host_exit((int)a);
	case LINUX_kill:
	case LINUX_tkill:
	case LINUX_tgkill:
	{
		int signal_number = (int)((int)number == LINUX_tgkill ? c : b);

		if (signal_number == 6)
			host_abort("the game called abort()");
		if (signal_number == 0)
			return 0;
		host_logf(HOST_LOG_WARN, "the game raised signal %d", signal_number);
		return 0;
	}
	case LINUX_set_tid_address:
	case LINUX_gettid:
	{
		uint64_t id = 0;

		pthread_threadid_np(NULL, &id);
		return (long)(id & 0x3fffffff);
	}
	case LINUX_getpid:
		return getpid();
	case LINUX_getppid:
		return getppid();
	case LINUX_getuid:
	case LINUX_geteuid:
	case LINUX_getgid:
	case LINUX_getegid:
		return 501;
	case LINUX_rt_sigaction:
	case LINUX_rt_sigprocmask:
	case LINUX_sigaltstack:
		/* the host owns signals */
		return 0;
	case LINUX_getrandom:
		arc4random_buf(GP(a), (size_t)(uint32_t)b);
		return (long)(uint32_t)b;
	case LINUX_uname:
	{
		struct guest_utsname *name = GP(a);

		memset(name, 0, sizeof(*name));
		strcpy(name->sysname, "Linux");
		strcpy(name->nodename, "iphone");
		strcpy(name->release, "6.0.0");
		strcpy(name->version, "iOS");
		strcpy(name->machine, "wasm32");
		return 0;
	}
	case LINUX_getrlimit:
	case LINUX_prlimit64:
	case LINUX_statx:
	case LINUX_statfs:
	case LINUX_fstatfs:
	case LINUX_pselect6:
	case LINUX_sysinfo:
		return -LINUX_ENOSYS;
	default:
		host_logf(HOST_LOG_WARN, "guest system call %llu is not supported", (unsigned long long)number);
		return -LINUX_ENOSYS;
	}
}

/* ---------- memory mappings */

long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset)
{
	uint32_t result;

	(void)protection;
	if (!size || size > 0x80000000ull)
		return -LINUX_EINVAL;
	/* MAP_FIXED (0x10), MAP_FIXED_NOREPLACE (0x100000) */
	result = host_guest_map((uint32_t)address, (uint32_t)size, (flags & 0x100010) != 0 && address);
	if (!result)
		return -LINUX_ENOMEM;
	/* a file's contents copied in (MAP_ANONYMOUS is 0x20) */
	if (!(flags & 0x20) && fd >= 0)
	{
		ssize_t read_size = pread(fd, host_guest_base + result, (size_t)size, (off_t)offset);

		if (read_size < 0)
		{
			host_guest_unmap(result, (uint32_t)size);
			return failed();
		}
	}
	return result;
}

long host_guest_munmap(uint64_t address, uint64_t size)
{
	host_guest_unmap((uint32_t)address, (uint32_t)size);
	return 0;
}
