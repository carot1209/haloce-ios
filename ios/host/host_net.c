/*
HOST_NET.C

The guest's sockets (posix.h's posix_socket_*, behind the Winsock layer in
port/linux/src/xnet.c), on iOS: OpenCE's posix_net.c, which the Android host
builds, made Darwin's.

The one real difference is the address: the guest's sockaddr is Winsock's
and Linux's (a 16-bit family), Darwin's starts with a length byte and an
8-bit family. Addresses are turned around at the boundary. The families,
types, protocols, message flags and IPPROTO_IP/IPPROTO_TCP option numbers
the game uses are the same on both; SOL_SOCKET options are translated, as
posix_net.c does.

System link on iOS: since iOS 14 an app needs Apple's multicast
entitlement to broadcast, which a sideloaded app does not have. The host
sends the game's broadcasts to every address of the local /24 network as
well (network.broadcast, host_main.m), as unicast, which any app may send
once the player allows the local network.
*/

#include "host.h"
#include "posix.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

/* Winsock error codes (winsockx.h) */
#define WSAEINTR 10004
#define WSAEBADF 10009
#define WSAEACCES 10013
#define WSAEFAULT 10014
#define WSAEINVAL 10022
#define WSAEMFILE 10024
#define WSAEWOULDBLOCK 10035
#define WSAEINPROGRESS 10036
#define WSAEALREADY 10037
#define WSAENOTSOCK 10038
#define WSAEDESTADDRREQ 10039
#define WSAEMSGSIZE 10040
#define WSAEPROTOTYPE 10041
#define WSAENOPROTOOPT 10042
#define WSAEPROTONOSUPPORT 10043
#define WSAEOPNOTSUPP 10045
#define WSAEAFNOSUPPORT 10047
#define WSAEADDRINUSE 10048
#define WSAEADDRNOTAVAIL 10049
#define WSAENETDOWN 10050
#define WSAENETUNREACH 10051
#define WSAECONNABORTED 10053
#define WSAECONNRESET 10054
#define WSAENOBUFS 10055
#define WSAEISCONN 10056
#define WSAENOTCONN 10057
#define WSAESHUTDOWN 10058
#define WSAETIMEDOUT 10060
#define WSAECONNREFUSED 10061
#define WSAEHOSTUNREACH 10065

/* Winsock's SOL_SOCKET and its options */
#define WINSOCK_SOL_SOCKET 0xffff
#define WINSOCK_SO_REUSEADDR 0x0004
#define WINSOCK_SO_KEEPALIVE 0x0008
#define WINSOCK_SO_BROADCAST 0x0020
#define WINSOCK_SO_LINGER 0x0080
#define WINSOCK_SO_SNDBUF 0x1001
#define WINSOCK_SO_RCVBUF 0x1002
#define WINSOCK_SO_ERROR 0x1007
#define WINSOCK_SO_TYPE 0x1008

/* (Linux, for the test harness: no such option, MSG_NOSIGNAL instead, and
the guest's address layout is the system's) */
#ifndef SO_NOSIGPIPE
#define SO_NOSIGPIPE 0
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

static __thread int last_error;

static int winsock_error(int error)
{
	switch (error)
	{
	case EINTR: return WSAEINTR;
	case EBADF: return WSAEBADF;
	case EACCES: case EPERM: return WSAEACCES;
	case EFAULT: return WSAEFAULT;
	case EINVAL: return WSAEINVAL;
	case EMFILE: case ENFILE: return WSAEMFILE;
	case EAGAIN: return WSAEWOULDBLOCK;
	case EINPROGRESS: return WSAEWOULDBLOCK;
	case EALREADY: return WSAEALREADY;
	case ENOTSOCK: return WSAENOTSOCK;
	case EDESTADDRREQ: return WSAEDESTADDRREQ;
	case EMSGSIZE: return WSAEMSGSIZE;
	case EPROTOTYPE: return WSAEPROTOTYPE;
	case ENOPROTOOPT: return WSAENOPROTOOPT;
	case EPROTONOSUPPORT: return WSAEPROTONOSUPPORT;
	case EOPNOTSUPP: return WSAEOPNOTSUPP;
	case EAFNOSUPPORT: return WSAEAFNOSUPPORT;
	case EADDRINUSE: return WSAEADDRINUSE;
	case EADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
	case ENETDOWN: return WSAENETDOWN;
	case ENETUNREACH: return WSAENETUNREACH;
	case ECONNABORTED: return WSAECONNABORTED;
	/* (a send on a connection the other end reset: SIGPIPE is off) */
	case ECONNRESET: case EPIPE: return WSAECONNRESET;
	case ENOBUFS: case ENOMEM: return WSAENOBUFS;
	case EISCONN: return WSAEISCONN;
	case ENOTCONN: return WSAENOTCONN;
	case ESHUTDOWN: return WSAESHUTDOWN;
	case ETIMEDOUT: return WSAETIMEDOUT;
	case ECONNREFUSED: return WSAECONNREFUSED;
	case EHOSTUNREACH: case EHOSTDOWN: return WSAEHOSTUNREACH;
	default: return WSAEINVAL;
	}
}

static int fail(void)
{
	last_error = winsock_error(errno);
	return -1;
}

static int succeed(int result)
{
	if (result < 0)
		return fail();
	last_error = 0;
	return result;
}

/* ---------- addresses: the guest's (a 16-bit family first) and Darwin's */

/* a guest address as Darwin's, in storage; its length */
static socklen_t address_in(const void *guest, int length, struct sockaddr_storage *storage)
{
	const uint8_t *bytes = guest;

	if (!guest || length < 2)
		return (socklen_t)(length < 0 ? 0 : length);
	if ((size_t)length > sizeof(*storage))
		length = (int)sizeof(*storage);
	memset(storage, 0, sizeof(*storage));
	memcpy(storage, guest, (size_t)length);
#ifdef __APPLE__
	((uint8_t *)storage)[0] = (uint8_t)length;
	((uint8_t *)storage)[1] = (uint8_t)(bytes[0] | bytes[1] << 8);
#else
	(void)bytes;
#endif
	return (socklen_t)length;
}

/* Darwin's address into the guest's */
static void address_out(const struct sockaddr_storage *storage, socklen_t length, void *guest, int *guest_length)
{
	uint8_t *bytes = guest;
#ifdef __APPLE__
	uint16_t family = ((const uint8_t *)storage)[1];
#else
	uint16_t family = storage->ss_family;
#endif
	int size;

	if (!guest || !guest_length)
		return;
	size = (int)length < *guest_length ? (int)length : *guest_length;
	if (size > 0)
		memcpy(guest, storage, (size_t)size);
	if (size >= 2)
	{
		bytes[0] = (uint8_t)family;
		bytes[1] = (uint8_t)(family >> 8);
	}
	*guest_length = (int)length;
}

/* ---------- the calls */

int posix_socket_last_error(void)
{
	return last_error;
}

int posix_socket(int family, int type, int protocol)
{
	int descriptor = socket(family, type, protocol);
	int on = 1;

	if (descriptor >= 0)
	{
		fcntl(descriptor, F_SETFD, FD_CLOEXEC);
		/* (no SIGPIPE: a reset connection is an error, as on Winsock) */
		if (SO_NOSIGPIPE)
			setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
	}
	return succeed(descriptor);
}

int posix_socket_close(int socket)
{
	return succeed(close(socket));
}

int posix_socket_bind(int socket, const void *address, int address_length)
{
	struct sockaddr_storage storage;
	socklen_t length = address_in(address, address_length, &storage);

	return succeed(bind(socket, (struct sockaddr *)&storage, length));
}

int posix_socket_connect(int socket, const void *address, int address_length)
{
	struct sockaddr_storage storage;
	socklen_t length = address_in(address, address_length, &storage);
	int result = connect(socket, (struct sockaddr *)&storage, length);

	/* (Winsock's WSAEWOULDBLOCK, which the game waits on: posix_net.c) */
	if (result < 0 && errno == EINPROGRESS)
	{
		last_error = WSAEWOULDBLOCK;
		return -1;
	}
	return succeed(result);
}

int posix_socket_listen(int socket, int backlog)
{
	return succeed(listen(socket, backlog));
}

int posix_socket_accept(int socket, void *address, int *address_length)
{
	struct sockaddr_storage storage;
	socklen_t length = sizeof(storage);
	int result = accept(socket, (struct sockaddr *)&storage, &length);
	int on = 1;

	if (result >= 0)
	{
		fcntl(result, F_SETFD, FD_CLOEXEC);
		if (SO_NOSIGPIPE)
			setsockopt(result, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
		address_out(&storage, length, address, address_length);
	}
	return succeed(result);
}

int posix_socket_send(int socket, const void *buffer, int length, int flags)
{
	return succeed((int)send(socket, buffer, (size_t)length, flags | SEND_FLAGS));
}

int posix_socket_sendto(int socket, const void *buffer, int length, int flags,
	const void *address, int address_length)
{
	struct sockaddr_storage storage;
	socklen_t size = address_in(address, address_length, &storage);

	return succeed((int)sendto(socket, buffer, (size_t)length, flags | SEND_FLAGS,
		address ? (struct sockaddr *)&storage : NULL, address ? size : 0));
}

int posix_socket_recv(int socket, void *buffer, int length, int flags)
{
	return succeed((int)recv(socket, buffer, (size_t)length, flags));
}

int posix_socket_recvfrom(int socket, void *buffer, int length, int flags,
	void *address, int *address_length)
{
	struct sockaddr_storage storage;
	struct iovec vector;
	struct msghdr message;
	int result;

	vector.iov_base = buffer;
	vector.iov_len = (size_t)length;
	memset(&message, 0, sizeof(message));
	message.msg_name = &storage;
	message.msg_namelen = sizeof(storage);
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	result = (int)recvmsg(socket, &message, flags);
	if (result >= 0)
		address_out(&storage, message.msg_namelen, address, address_length);
	/* (a datagram larger than the buffer is an error on Winsock) */
	if (result >= 0 && (message.msg_flags & MSG_TRUNC))
	{
		last_error = WSAEMSGSIZE;
		return -1;
	}
	return succeed(result);
}

int posix_socket_shutdown(int socket, int how)
{
	return succeed(shutdown(socket, how));
}

int posix_socket_set_nonblocking(int socket, int nonblocking)
{
	int flags = fcntl(socket, F_GETFL);

	if (flags < 0)
		return fail();
	flags = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
	return succeed(fcntl(socket, F_SETFL, flags));
}

int posix_socket_set_nodelay(int socket)
{
	int value = 1;

	return succeed(setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)));
}

int posix_socket_bytes_available(int socket, posix_ulong *count)
{
	int available = 0;
	int result = ioctl(socket, FIONREAD, &available);

	if (result >= 0)
		*count = (posix_ulong)available;
	return succeed(result);
}

static int translate_option(int level, int name, int *host_level, int *host_name)
{
	if (level != WINSOCK_SOL_SOCKET)
	{
		*host_level = level;
		*host_name = name;
		return 0;
	}
	*host_level = SOL_SOCKET;
	switch (name)
	{
	case WINSOCK_SO_REUSEADDR: *host_name = SO_REUSEADDR; return 0;
	case WINSOCK_SO_KEEPALIVE: *host_name = SO_KEEPALIVE; return 0;
	case WINSOCK_SO_BROADCAST: *host_name = SO_BROADCAST; return 0;
	case WINSOCK_SO_LINGER: *host_name = SO_LINGER; return 0;
	case WINSOCK_SO_SNDBUF: *host_name = SO_SNDBUF; return 0;
	case WINSOCK_SO_RCVBUF: *host_name = SO_RCVBUF; return 0;
	case WINSOCK_SO_ERROR: *host_name = SO_ERROR; return 0;
	case WINSOCK_SO_TYPE: *host_name = SO_TYPE; return 0;
	default: return -1;
	}
}

int posix_socket_setsockopt(int socket, int level, int name, const void *value, int length)
{
	int host_level, host_name;

	if (translate_option(level, name, &host_level, &host_name) != 0)
	{
		/* (Xbox-only options such as SO_ENCRYPT) */
		last_error = 0;
		return 0;
	}
	return succeed(setsockopt(socket, host_level, host_name, value, (socklen_t)length));
}

int posix_socket_getsockopt(int socket, int level, int name, void *value, int *length)
{
	int host_level, host_name;
	socklen_t socket_length = (socklen_t)*length;
	int result;

	if (translate_option(level, name, &host_level, &host_name) != 0)
	{
		last_error = WSAENOPROTOOPT;
		return -1;
	}
	result = getsockopt(socket, host_level, host_name, value, &socket_length);
	*length = (int)socket_length;
	/* (SO_ERROR's value is an errno: Winsock's is a Winsock code) */
	if (result == 0 && host_name == SO_ERROR && host_level == SOL_SOCKET && *length >= (int)sizeof(int) &&
		*(int *)value)
		*(int *)value = winsock_error(*(int *)value);
	return succeed(result);
}

int posix_socket_getsockname(int socket, void *address, int *address_length)
{
	struct sockaddr_storage storage;
	socklen_t length = sizeof(storage);
	int result = getsockname(socket, (struct sockaddr *)&storage, &length);

	if (result == 0)
		address_out(&storage, length, address, address_length);
	return succeed(result);
}

int posix_socket_getpeername(int socket, void *address, int *address_length)
{
	struct sockaddr_storage storage;
	socklen_t length = sizeof(storage);
	int result = getpeername(socket, (struct sockaddr *)&storage, &length);

	if (result == 0)
		address_out(&storage, length, address, address_length);
	return succeed(result);
}

/* (posix_net.c's: poll with select's readiness and Winsock's connect
reporting) */
int posix_socket_select(int *read, int *read_count, int *write, int *write_count,
	int *error, int *error_count, posix_long timeout_seconds, posix_long timeout_microseconds, int infinite)
{
	static const short events[3] = { POLLIN, POLLOUT, POLLPRI };
	static const short ready[3] = { POLLIN | POLLHUP | POLLERR, POLLOUT | POLLERR, POLLPRI | POLLERR };
	static __thread struct pollfd *buffer;
	static __thread int buffer_size;
	int *lists[3] = { read, write, error };
	int *counts[3] = { read_count, write_count, error_count };
	struct pollfd stack[256];
	struct pollfd *descriptors = stack;
	long long milliseconds = (long long)timeout_seconds * 1000 + ((long long)timeout_microseconds + 999) / 1000;
	int total = 0;
	int list, index;
	int result;

	for (list = 0; list < 3; list++)
	{
		if (!lists[list] || !counts[list])
			lists[list] = NULL;
		else
			total += *counts[list];
	}
	if (total > (int)(sizeof(stack) / sizeof(*stack)))
	{
		if (total > buffer_size)
		{
			struct pollfd *larger = realloc(buffer, sizeof(*buffer) * (size_t)total);

			if (!larger)
			{
				errno = ENOMEM;
				return fail();
			}
			buffer = larger;
			buffer_size = total;
		}
		descriptors = buffer;
	}
	total = 0;
	for (list = 0; list < 3; list++)
	{
		for (index = 0; lists[list] && index < *counts[list]; index++, total++)
		{
			descriptors[total].fd = lists[list][index];
			descriptors[total].events = events[list];
			descriptors[total].revents = 0;
		}
	}
	result = poll(descriptors, (nfds_t)total, infinite ? -1 :
		(int)(milliseconds < 0 ? 0 : milliseconds > INT_MAX ? INT_MAX : milliseconds));
	for (index = 0; index < total && result > 0; index++)
	{
		if (descriptors[index].revents & POLLNVAL)
		{
			errno = EBADF;
			result = -1;
		}
	}
	if (result < 0)
		return fail();
	result = 0;
	total = 0;
	for (list = 0; list < 3; list++)
	{
		int kept = 0;

		for (index = 0; lists[list] && index < *counts[list]; index++, total++)
		{
			int descriptor = lists[list][index];
			int pending = 0;
			socklen_t length = sizeof(pending);

			if (!(descriptors[total].revents & ready[list]))
				continue;
			if (list == 1 && getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &pending, &length) == 0 && pending)
			{
				errno = pending;
				fail();
				continue;
			}
			lists[list][kept++] = descriptor;
		}
		if (lists[list])
			*counts[list] = kept;
		result += kept;
	}
	if (result > 0)
		last_error = 0;
	return result;
}
