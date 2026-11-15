// SPDX-License-Identifier: GPL-2.0
/* 4KB processes reject TCP socket mmap() while keeping read/recv fallback. */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/mman.h>
#include <sys/socket.h>

#define P3S_HARNESS_DEFAULT_NATIVE 1
#define P3S_HARNESS_ROLE child_role
#include "p3s_harness.h"

#ifndef MSG_ZEROCOPY
#define MSG_ZEROCOPY 0x4000000
#endif

static int connected_loopback_fd(int *peer)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	socklen_t alen = sizeof(addr);
	int lfd, cfd;

	*peer = -1;
	lfd = socket(AF_INET, SOCK_STREAM, 0);
	if (lfd < 0)
		return -1;
	if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) ||
	    listen(lfd, 1) ||
	    getsockname(lfd, (struct sockaddr *)&addr, &alen)) {
		close(lfd);
		return -1;
	}
	cfd = socket(AF_INET, SOCK_STREAM, 0);
	if (cfd < 0 || connect(cfd, (struct sockaddr *)&addr, sizeof(addr))) {
		if (cfd >= 0)
			close(cfd);
		close(lfd);
		return -1;
	}
	*peer = accept(lfd, NULL, NULL);
	close(lfd);
	if (*peer < 0) {
		close(cfd);
		return -1;
	}
	return cfd;
}

static int check_tcp_mmap(bool four_kb, int fd)
{
	int page_size = getpagesize();
	int saved_errno;
	void *map;

	errno = 0;
	map = mmap(NULL, page_size, PROT_READ, MAP_SHARED, fd, 0);
	saved_errno = errno;
	if (four_kb) {
		if (map != MAP_FAILED) {
			munmap(map, page_size);
			ksft_print_msg("4KB TCP mmap unexpectedly succeeded\n");
			return KSFT_FAIL;
		}
		if (saved_errno != EOPNOTSUPP) {
			ksft_print_msg("4KB TCP mmap: expected EOPNOTSUPP, got %s (%d)\n",
				       strerror(saved_errno), saved_errno);
			return KSFT_FAIL;
		}
		return KSFT_PASS;
	}
	if (map == MAP_FAILED) {
		if (saved_errno == EOPNOTSUPP)
			return KSFT_SKIP;
		ksft_print_msg("native TCP mmap failed: %s\n",
			       strerror(saved_errno));
		return KSFT_FAIL;
	}
	munmap(map, page_size);
	return KSFT_PASS;
}

static int run_mmap_mode(bool four_kb)
{
	int fd, peer, ret;

	fd = connected_loopback_fd(&peer);
	if (fd < 0)
		return KSFT_FAIL;
	ret = check_tcp_mmap(four_kb, fd);
	close(peer);
	close(fd);
	return ret;
}

static int run_inherited_mode(const char *fd_arg)
{
	char *end = NULL;
	long fd_val;
	int ret;

	errno = 0;
	fd_val = strtol(fd_arg, &end, 10);
	if (errno || !end || *end || fd_val < 0 || fd_val > INT_MAX)
		return KSFT_FAIL;
	ret = check_tcp_mmap(true, (int)fd_val);
	close((int)fd_val);
	return ret;
}

static int run_read_fallback(void)
{
	const size_t bytes = 64 * 1024;
	unsigned char *data = MAP_FAILED;
	unsigned char *received = NULL;
	int sender = -1, receiver = -1;
	int one = 1, buf_size = 1024 * 1024;
	int send_flags = 0, ret = KSFT_FAIL;
	size_t sent = 0, consumed = 0, i;

	alarm(20);
	sender = connected_loopback_fd(&receiver);
	if (sender < 0)
		goto out;
	setsockopt(receiver, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
	setsockopt(sender, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
	setsockopt(sender, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	if (check_tcp_mmap(true, receiver) != KSFT_PASS)
		goto out;

	data = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	received = malloc(bytes);
	if (data == MAP_FAILED || !received)
		goto out;
	for (i = 0; i < bytes; i++)
		data[i] = (unsigned char)(0x40 + i / PROCESS_PAGE_SIZE);

	if (!setsockopt(sender, SOL_SOCKET, SO_ZEROCOPY, &one, sizeof(one)))
		send_flags = MSG_ZEROCOPY;
	while (sent < bytes) {
		ssize_t n = send(sender, data + sent, bytes - sent, send_flags);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && send_flags && !sent &&
		    (errno == ENOBUFS || errno == EOPNOTSUPP ||
		     errno == EINVAL)) {
			send_flags = 0;
			continue;
		}
		if (n <= 0) {
			ksft_print_msg("send failed: %s\n", strerror(errno));
			goto out;
		}
		sent += n;
	}
	shutdown(sender, SHUT_WR);

	while (consumed < bytes) {
		ssize_t n = recv(receiver, received + consumed,
				 bytes - consumed, 0);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		consumed += n;
	}
	if (consumed != bytes || memcmp(received, data, bytes)) {
		ksft_print_msg("TCP recv fallback mismatch: %zu/%zu bytes\n",
			       consumed, bytes);
		goto out;
	}
	ret = KSFT_PASS;
out:
	free(received);
	if (data != MAP_FAILED)
		munmap(data, bytes);
	if (receiver >= 0)
		close(receiver);
	if (sender >= 0)
		close(sender);
	alarm(0);
	return ret;
}

TEST(native_tcp_mmap_succeeds)
{
	int ret = p3s_run_child(false, "--native", "mmap", NULL);

	if (ret == KSFT_SKIP)
		SKIP(return, "native TCP mmap unavailable");
	EXPECT_EQ(KSFT_PASS, ret);
}

TEST(four_kb_tcp_mmap_rejected)
{
	int ret = p3s_run_child(true, "--4kb", "mmap", NULL);

	if (ret == KSFT_SKIP)
		SKIP(return, "4KB TCP mmap check unavailable");
	EXPECT_EQ(KSFT_PASS, ret);
}

TEST(four_kb_inherited_tcp_mmap_rejected)
{
	char fd_buf[32];
	int fd, peer, flags, ret;

	fd = connected_loopback_fd(&peer);
	ASSERT_GE(fd, 0);
	ASSERT_GE(peer, 0);
	flags = fcntl(fd, F_GETFD);
	if (flags < 0 || fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC)) {
		close(peer);
		close(fd);
		ASSERT_TRUE(false);
	}
	snprintf(fd_buf, sizeof(fd_buf), "%d", fd);
	ret = p3s_run_child(true, "--4kb", "inherited", fd_buf, NULL);
	close(peer);
	close(fd);
	if (ret == KSFT_SKIP)
		SKIP(return, "4KB inherited TCP mmap check unavailable");
	EXPECT_EQ(KSFT_PASS, ret);
}

TEST(four_kb_read_after_mmap_refusal)
{
	int ret = p3s_run_child(true, "--4kb", "read", NULL);

	if (ret == KSFT_SKIP)
		SKIP(return, "4KB TCP read fallback unavailable");
	EXPECT_EQ(KSFT_PASS, ret);
}

static int child_role(int argc, char **argv)
{
	bool four_kb;

	if (argc < 3 ||
	    (strcmp(argv[1], "--native") && strcmp(argv[1], "--4kb")))
		return -1;
	four_kb = !strcmp(argv[1], "--4kb");
	if (four_kb ? !p3s_is_4kb_process() :
	    getpagesize() != NATIVE_PAGE_SIZE)
		return KSFT_SKIP;
	if (!strcmp(argv[2], "mmap") && argc == 3)
		return run_mmap_mode(four_kb);
	if (four_kb && !strcmp(argv[2], "inherited") && argc == 4)
		return run_inherited_mode(argv[3]);
	if (four_kb && !strcmp(argv[2], "read") && argc == 3)
		return run_read_fallback();
	return KSFT_FAIL;
}

P3S_HARNESS_4KB_MAIN
