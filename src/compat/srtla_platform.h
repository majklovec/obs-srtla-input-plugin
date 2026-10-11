/*
 * Portable socket/event-loop layer for the embedded irlserver/srtla receiver.
 *
 * The upstream receiver is written against Linux (BSD sockets + epoll). This
 * header gives it and the plugin one small API that maps to Winsock + wepoll on
 * Windows, kqueue on macOS, and native calls on Linux. It is included from C
 * (common.h) and C++ code, so everything here is plain C.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <wepoll.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#if defined(_MSC_VER) && !defined(_SSIZE_T_DEFINED)
#define _SSIZE_T_DEFINED
typedef intptr_t ssize_t;
#endif
typedef uint32_t in_addr_t;

#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
#ifndef EPOLL_CLOEXEC
#define EPOLL_CLOEXEC 0
#endif

#define htobe16(x) htons(x)
#define be16toh(x) ntohs(x)
#define htobe32(x) htonl(x)
#define be32toh(x) ntohl(x)

#elif defined(__APPLE__)

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* SRTLA uses the Linux epoll shape; map it to kqueue on macOS. */
struct epoll_event {
	uint32_t events;
	union {
		void *ptr;
		int fd;
		uint32_t u32;
		uint64_t u64;
	} data;
};

#define EPOLLIN 0x001
#define EPOLLOUT 0x004
#define EPOLLERR 0x008
#define EPOLLHUP 0x010
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3
#define EPOLL_CLOEXEC 0

#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

#ifndef htobe16
#define htobe16(x) htons(x)
#define be16toh(x) ntohs(x)
#define htobe32(x) htonl(x)
#define be32toh(x) ntohl(x)
#endif

#else /* !_WIN32 && !_APPLE */

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#endif /* _WIN32 */

#ifdef __cplusplus
extern "C" {
#endif

/* Sockets are handled as int everywhere, like upstream. Windows kernel handles
 * (including SOCKETs) are guaranteed to fit in 32 bits. */
#ifdef _WIN32
#define SRTLA_SOCK(fd) ((SOCKET)(intptr_t)(fd))
#else
#define SRTLA_SOCK(fd) (fd)
#endif

static inline int srtla_close_socket(int fd)
{
#ifdef _WIN32
	return closesocket(SRTLA_SOCK(fd));
#else
	return close(fd);
#endif
}

static inline int srtla_socket_errno(void)
{
#ifdef _WIN32
	return WSAGetLastError();
#else
	return errno;
#endif
}

static inline int srtla_socket_would_block(void)
{
#ifdef _WIN32
	const int err = WSAGetLastError();
	return err == WSAEWOULDBLOCK || err == WSATRY_AGAIN;
#else
	return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

static inline int srtla_socket_interrupted(void)
{
#ifdef _WIN32
	return WSAGetLastError() == WSAEINTR;
#else
	return errno == EINTR;
#endif
}

/* Text for the last socket error. Uses a static buffer on Windows, like
 * upstream's print_addr(); do not keep the pointer across calls. */
static inline const char *srtla_socket_strerror(void)
{
#ifdef _WIN32
	static char buffer[256];
	const DWORD err = (DWORD)WSAGetLastError();
	const DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS |
					       FORMAT_MESSAGE_MAX_WIDTH_MASK,
				       NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buffer,
				       (DWORD)sizeof(buffer), NULL);
	if (n == 0)
		snprintf(buffer, sizeof(buffer), "Winsock error %lu", (unsigned long)err);
	return buffer;
#else
	return strerror(errno);
#endif
}

/* Puts a UDP socket in non-blocking mode. On Windows it also stops the socket
 * from failing later recvfrom() calls with WSAECONNRESET after an ICMP port
 * unreachable, which BSD sockets never report for unconnected datagrams. */
static inline int srtla_configure_udp_socket(int fd)
{
#ifdef _WIN32
	u_long nonblocking = 1;
	BOOL report_reset = FALSE;
	DWORD returned = 0;
	if (ioctlsocket(SRTLA_SOCK(fd), FIONBIO, &nonblocking) != 0)
		return -1;
	WSAIoctl(SRTLA_SOCK(fd), SIO_UDP_CONNRESET, &report_reset, sizeof(report_reset), NULL, 0,
		 &returned, NULL, NULL);
	return 0;
#else
	const int flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1)
		return -1;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1 ? -1 : 0;
#endif
}

static inline int srtla_setsockopt(int fd, int level, int name, const void *value, socklen_t len)
{
#ifdef _WIN32
	return setsockopt(SRTLA_SOCK(fd), level, name, (const char *)value, (int)len);
#else
	return setsockopt(fd, level, name, value, len);
#endif
}

static inline int srtla_connect(int fd, const struct sockaddr *addr, socklen_t len)
{
#ifdef _WIN32
	return connect(SRTLA_SOCK(fd), addr, (int)len);
#else
	return connect(fd, addr, len);
#endif
}

static inline int srtla_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
#ifdef _WIN32
	return bind(SRTLA_SOCK(fd), addr, (int)len);
#else
	return bind(fd, addr, len);
#endif
}

static inline int srtla_getsockname(int fd, struct sockaddr *addr, socklen_t *len)
{
#ifdef _WIN32
	int ilen = (int)*len;
	const int ret = getsockname(SRTLA_SOCK(fd), addr, &ilen);
	*len = (socklen_t)ilen;
	return ret;
#else
	return getsockname(fd, addr, len);
#endif
}

static inline int srtla_send(int fd, const void *buf, int len, int flags)
{
#ifdef _WIN32
	return send(SRTLA_SOCK(fd), (const char *)buf, len, flags);
#else
	return (int)send(fd, buf, (size_t)len, flags);
#endif
}

static inline int srtla_recv(int fd, void *buf, int len, int flags)
{
#ifdef _WIN32
	return recv(SRTLA_SOCK(fd), (char *)buf, len, flags);
#else
	return (int)recv(fd, buf, (size_t)len, flags);
#endif
}

static inline int srtla_sendto(int fd, const void *buf, int len, int flags, const struct sockaddr *to,
			       socklen_t tolen)
{
#ifdef _WIN32
	return sendto(SRTLA_SOCK(fd), (const char *)buf, len, flags, to, (int)tolen);
#else
	return (int)sendto(fd, buf, (size_t)len, flags, to, tolen);
#endif
}

static inline int srtla_recvfrom(int fd, void *buf, int len, int flags, struct sockaddr *from,
				 socklen_t *fromlen)
{
#ifdef _WIN32
	int ilen = (int)*fromlen;
	const int ret = recvfrom(SRTLA_SOCK(fd), (char *)buf, len, flags, from, &ilen);
	*fromlen = (socklen_t)ilen;
	return ret;
#else
	return (int)recvfrom(fd, buf, (size_t)len, flags, from, fromlen);
#endif
}

/* epoll with int descriptors; wepoll uses HANDLE/SOCKET on Windows. */
static inline int srtla_epoll_create(void)
{
#ifdef _WIN32
	HANDLE handle = epoll_create1(0);
	return handle == NULL ? -1 : (int)(intptr_t)handle;
#elif defined(__APPLE__)
	return kqueue();
#else
	return epoll_create1(EPOLL_CLOEXEC);
#endif
}

static inline int srtla_epoll_close(int epfd)
{
#ifdef _WIN32
	return epoll_close((HANDLE)(intptr_t)epfd);
#elif defined(__APPLE__)
	return close(epfd);
#else
	return close(epfd);
#endif
}

static inline int srtla_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
#ifdef _WIN32
	return epoll_ctl((HANDLE)(intptr_t)epfd, op, SRTLA_SOCK(fd), event);
#elif defined(__APPLE__)
	if (op == EPOLL_CTL_DEL) {
		struct kevent change;
		int removed = 0;
		const int filters[] = {EVFILT_READ, EVFILT_WRITE};
		for (size_t i = 0; i < sizeof(filters) / sizeof(filters[0]); ++i) {
			EV_SET(&change, (uintptr_t)fd, filters[i], EV_DELETE, 0, 0, NULL);
			if (kevent(epfd, &change, 1, NULL, 0, NULL) == 0)
				removed++;
			else if (errno != ENOENT)
				return -1;
		}
		if (removed == 0) {
			errno = ENOENT;
			return -1;
		}
		return 0;
	}

	if (op != EPOLL_CTL_ADD && op != EPOLL_CTL_MOD) {
		errno = EINVAL;
		return -1;
	}

	struct kevent changes[2];
	int count = 0;
	if ((event->events & EPOLLIN) != 0) {
		EV_SET(&changes[count++], (uintptr_t)fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0,
		       event->data.ptr);
	}
	if ((event->events & EPOLLOUT) != 0) {
		EV_SET(&changes[count++], (uintptr_t)fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0,
		       event->data.ptr);
	}
	if (count == 0) {
		errno = EINVAL;
		return -1;
	}
	return kevent(epfd, changes, count, NULL, 0, NULL);
#else
	return epoll_ctl(epfd, op, fd, event);
#endif
}

static inline int srtla_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout_ms)
{
#ifdef _WIN32
	return epoll_wait((HANDLE)(intptr_t)epfd, events, maxevents, timeout_ms);
#elif defined(__APPLE__)
	if (maxevents <= 0) {
		errno = EINVAL;
		return -1;
	}
	struct kevent *ready = (struct kevent *)malloc((size_t)maxevents * sizeof(*ready));
	if (ready == NULL)
		return -1;
	struct timespec timeout;
	struct timespec *timeout_ptr = NULL;
	if (timeout_ms >= 0) {
		timeout.tv_sec = timeout_ms / 1000;
		timeout.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
		timeout_ptr = &timeout;
	}
	const int count = kevent(epfd, NULL, 0, ready, maxevents, timeout_ptr);
	for (int i = 0; i < count; ++i) {
		events[i].events = 0;
		if (ready[i].filter == EVFILT_READ)
			events[i].events |= EPOLLIN;
		if (ready[i].filter == EVFILT_WRITE)
			events[i].events |= EPOLLOUT;
		if ((ready[i].flags & EV_EOF) != 0)
			events[i].events |= EPOLLHUP;
		if ((ready[i].flags & EV_ERROR) != 0)
			events[i].events |= EPOLLERR;
		events[i].data.ptr = ready[i].udata;
	}
	free(ready);
	return count;
#else
	return epoll_wait(epfd, events, maxevents, timeout_ms);
#endif
}

/* Coarse monotonic clock, as CLOCK_MONOTONIC_COARSE upstream. */
static inline int srtla_monotonic_time(struct timespec *ts)
{
#ifdef _WIN32
	const ULONGLONG ms = GetTickCount64();
	ts->tv_sec = (time_t)(ms / 1000);
	ts->tv_nsec = (long)(ms % 1000) * 1000000L;
	return 0;
#elif defined(__APPLE__)
	return clock_gettime(CLOCK_MONOTONIC, ts);
#else
	return clock_gettime(CLOCK_MONOTONIC_COARSE, ts);
#endif
}

/* Cryptographically secure random bytes; returns 0 on success. */
static inline int srtla_random_bytes(void *buffer, size_t size)
{
#ifdef _WIN32
	return BCRYPT_SUCCESS(BCryptGenRandom(NULL, (PUCHAR)buffer, (ULONG)size,
					      BCRYPT_USE_SYSTEM_PREFERRED_RNG))
		       ? 0
		       : -1;
#else
	const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	size_t total = 0;
	if (fd < 0)
		return -1;
	while (total < size) {
		const ssize_t n = read(fd, (char *)buffer + total, size - total);
		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			close(fd);
			return -1;
		}
		total += (size_t)n;
	}
	close(fd);
	return 0;
#endif
}

/* Winsock needs explicit initialization per process; no-ops elsewhere.
 * Both calls are reference counted by the OS. */
static inline int srtla_sockets_init(void)
{
#ifdef _WIN32
	WSADATA data;
	return WSAStartup(MAKEWORD(2, 2), &data) == 0 ? 0 : -1;
#else
	return 0;
#endif
}

static inline void srtla_sockets_cleanup(void)
{
#ifdef _WIN32
	WSACleanup();
#endif
}

#ifdef __cplusplus
}
#endif
