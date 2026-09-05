// SPDX-License-Identifier: GPL-2.0
/*
 * TCP_ZEROCOPY_RECEIVE for a 4K compat process accepts a socket mapping at a
 * native-page-aligned address and lands the received data there intact.
 * Zero-copy maps whole native pages, so like every other native-page
 * operation the address must be 16K aligned (tcp_zerocopy_align_ppps covers
 * the rejection of misaligned addresses).
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <linux/tcp.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include "kselftest_ppps.h"


#ifndef MSG_ZEROCOPY
#define MSG_ZEROCOPY 0x4000000
#endif

#define MAP_LENGTH (256UL * 1024)
#define SEND_LENGTH (512UL * 1024)

static unsigned char pattern(size_t offset)
{
	return offset & 0xff;
}

static int make_listener(struct sockaddr_in *address)
{
	socklen_t length = sizeof(*address);
	int buffer_size = 2 * SEND_LENGTH;
	int low_water = MAP_LENGTH;
	int mss = NATIVE_PAGE_SIZE + 12;
	int one = 1;
	int fd;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer_size,
		   sizeof(buffer_size));
	setsockopt(fd, SOL_SOCKET, SO_RCVLOWAT, &low_water,
		   sizeof(low_water));
	setsockopt(fd, IPPROTO_TCP, TCP_MAXSEG, &mss, sizeof(mss));

	memset(address, 0, sizeof(*address));
	address->sin_family = AF_INET;
	address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(fd, (struct sockaddr *)address, sizeof(*address)) ||
	    getsockname(fd, (struct sockaddr *)address, &length) ||
	    listen(fd, 1)) {
		close(fd);
		return -1;
	}
	return fd;
}

static void sender(const struct sockaddr_in *address)
{
	unsigned char *buffer;
	size_t offset;
	int buffer_size = 2 * SEND_LENGTH;
	int mss = NATIVE_PAGE_SIZE + 12;
	int one = 1;
	int fd;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		_exit(2);
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer_size,
		   sizeof(buffer_size));
	setsockopt(fd, IPPROTO_TCP, TCP_MAXSEG, &mss, sizeof(mss));
	if (connect(fd, (const struct sockaddr *)address, sizeof(*address)))
		_exit(3);
	if (setsockopt(fd, SOL_SOCKET, SO_ZEROCOPY, &one, sizeof(one)))
		_exit(4);

	buffer = mmap(NULL, SEND_LENGTH, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buffer == MAP_FAILED)
		_exit(5);
	for (offset = 0; offset < SEND_LENGTH; offset++)
		buffer[offset] = pattern(offset);

	offset = 0;
	while (offset < SEND_LENGTH) {
		ssize_t sent = send(fd, buffer + offset, SEND_LENGTH - offset,
				    MSG_ZEROCOPY);

		if (sent <= 0)
			_exit(6);
		offset += sent;
	}
	shutdown(fd, SHUT_WR);
	close(fd);
	_exit(0);
}

static void *map_native_aligned(int fd, void **reservation,
				size_t *reservation_length)
{
	uintptr_t target;
	void *area;

	*reservation_length = MAP_LENGTH + 2 * NATIVE_PAGE_SIZE;
	*reservation = mmap(NULL, *reservation_length, PROT_NONE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (*reservation == MAP_FAILED)
		return MAP_FAILED;
	target = ((uintptr_t)*reservation + NATIVE_PAGE_SIZE - 1) &
		 ~(NATIVE_PAGE_SIZE - 1);
	area = mmap((void *)target, MAP_LENGTH, PROT_READ,
		    MAP_SHARED | MAP_FIXED, fd, 0);
	if (area == MAP_FAILED)
		return MAP_FAILED;
	return area;
}

static bool verify_mapping(const unsigned char *mapping, size_t length)
{
	size_t i;

	for (i = 0; i < length; i++) {
		if (mapping[i] != pattern(i))
			return false;
	}
	return true;
}

static int run_test(void)
{
	struct tcp_zerocopy_receive zc = {};
	struct sockaddr_in address;
	struct pollfd pfd;
	size_t reservation_length;
	void *reservation = MAP_FAILED;
	unsigned char *mapping;
	socklen_t zc_length;
	bool native_aligned;
	bool contents;
	bool mapped;
	pid_t child;
	int listener;
	int result;
	int status;
	int fd;

	ksft_print_header();
	ksft_set_plan(4);
	listener = make_listener(&address);
	if (listener < 0)
		ksft_exit_fail_msg("listener setup failed: %s\n",
				   strerror(errno));
	child = fork();
	if (child < 0)
		ksft_exit_fail_msg("fork failed: %s\n", strerror(errno));
	if (!child)
		sender(&address);
	fd = accept(listener, NULL, NULL);
	close(listener);
	if (fd < 0)
		ksft_exit_fail_msg("accept failed: %s\n", strerror(errno));

	mapping = map_native_aligned(fd, &reservation, &reservation_length);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("socket mmap failed: %s\n", strerror(errno));
	native_aligned = !((uintptr_t)mapping & (NATIVE_PAGE_SIZE - 1));
	ksft_test_result(native_aligned,
			 "place socket VMA at a native page boundary\n");

	pfd.fd = fd;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 5000) <= 0)
		ksft_exit_fail_msg("timed out waiting for TCP data\n");
	zc.address = (uintptr_t)mapping;
	zc.length = MAP_LENGTH;
	zc_length = sizeof(zc);
	result = getsockopt(fd, IPPROTO_TCP, TCP_ZEROCOPY_RECEIVE,
			    &zc, &zc_length);
	ksft_test_result(!result,
			 "accept a process-page-aligned zero-copy address\n");
	mapped = !result && zc.length;
	ksft_test_result(mapped, "map TCP receive pages\n");
	contents = mapped && verify_mapping(mapping, zc.length);
	ksft_test_result(contents, "preserve zero-copy TCP data\n");

	close(fd);
	munmap(reservation, reservation_length);
	waitpid(child, &status, 0);
	ksft_finished();
}

PPPS_COMPAT_MAIN(run_test)
