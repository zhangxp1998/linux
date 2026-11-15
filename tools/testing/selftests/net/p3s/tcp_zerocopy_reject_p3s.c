// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "p3s_harness.h"

#ifndef MSG_ZEROCOPY
#define MSG_ZEROCOPY 0x4000000
#endif

/* Compat mmap refusal must preserve the MSG_ZEROCOPY send/read fallback. */
struct tcp_outcome {
	bool setup_ok;
	bool unavailable;
	bool mmap_rejected;
	bool send_ok;
	bool fallback_ok;
	bool native_fragment;
	bool native_data_ok;
	bool native_failed;
};

static struct tcp_outcome run_probe(bool compat)
{
	struct sockaddr_in sa = { .sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	struct tcp_outcome outcome = {};
	socklen_t sl = sizeof(sa);
	const size_t bytes = 64 * 1024;
	unsigned char *data_res = MAP_FAILED, *data, *area = MAP_FAILED;
	unsigned char scratch[NATIVE_PAGE_SIZE];
	int listener = -1, sender = -1, receiver = -1;
	int one = 1, mss = NATIVE_PAGE_SIZE + 12;
	int socket_buffer = 1024 * 1024;
	size_t sent = 0, consumed = 0;

	alarm(20);
	listener = socket(AF_INET, SOCK_STREAM, 0);
	sender = socket(AF_INET, SOCK_STREAM, 0);
	if (listener < 0 || sender < 0)
		goto out;
	if (setsockopt(listener, SOL_SOCKET, SO_RCVBUF, &socket_buffer,
		       sizeof(socket_buffer)) ||
	    setsockopt(sender, SOL_SOCKET, SO_SNDBUF, &socket_buffer,
		       sizeof(socket_buffer)))
		goto out;
	setsockopt(listener, IPPROTO_TCP, TCP_MAXSEG, &mss, sizeof(mss));
	setsockopt(sender, IPPROTO_TCP, TCP_MAXSEG, &mss, sizeof(mss));
	setsockopt(sender, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	if (setsockopt(sender, SOL_SOCKET, SO_ZEROCOPY, &one, sizeof(one))) {
		outcome.unavailable = true;
		goto out;
	}
	if (bind(listener, (void *)&sa, sl) || listen(listener, 1) ||
	    getsockname(listener, (void *)&sa, &sl) ||
	    connect(sender, (void *)&sa, sl))
		goto out;
	receiver = accept(listener, NULL, NULL);
	if (receiver < 0)
		goto out;
	data_res = mmap(NULL, bytes + 2 * NATIVE_PAGE_SIZE,
			PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (data_res == MAP_FAILED)
		goto out;
	data = (unsigned char *)(((uintptr_t)data_res + NATIVE_PAGE_SIZE - 1) &
				 ~(uintptr_t)(NATIVE_PAGE_SIZE - 1));
	if (compat)
		data += PROCESS_PAGE_SIZE;
	area = mmap(NULL, bytes + NATIVE_PAGE_SIZE, PROT_READ, MAP_SHARED,
		    receiver, 0);
	outcome.mmap_rejected = area == MAP_FAILED && errno == EOPNOTSUPP;
	if (!compat && area == MAP_FAILED)
		goto out;
	outcome.setup_ok = true;
	madvise(data, bytes, MADV_NOHUGEPAGE);
	for (size_t page = 0; page < bytes / PROCESS_PAGE_SIZE; page++)
		memset(data + page * PROCESS_PAGE_SIZE, 0x40 + page,
		       PROCESS_PAGE_SIZE);
	while (sent < bytes) {
		ssize_t n = send(sender, data + sent, bytes - sent, MSG_ZEROCOPY);

		if (n <= 0)
			goto out;
		sent += n;
	}
	outcome.send_ok = true;
	shutdown(sender, SHUT_WR);
	if (compat) {
		unsigned char *received = malloc(bytes);

		if (!received)
			goto out;
		while (consumed < bytes) {
			ssize_t n = recv(receiver, received + consumed,
					 bytes - consumed, 0);

			if (n <= 0)
				break;
			consumed += n;
		}
		outcome.fallback_ok = consumed == bytes &&
				       !memcmp(received, data, bytes);
		free(received);
		goto out;
	}
	while (consumed < bytes) {
		struct tcp_zerocopy_receive zc = {
			.address = ((uintptr_t)area + NATIVE_PAGE_SIZE - 1) &
				   ~(uintptr_t)(NATIVE_PAGE_SIZE - 1),
			.length = bytes,
		};
		socklen_t len = sizeof(zc);
		int ret = getsockopt(receiver, IPPROTO_TCP, TCP_ZEROCOPY_RECEIVE,
				     &zc, &len);

		if (ret) {
			outcome.native_failed = true;
			break;
		}
		if (zc.length) {
			unsigned char *mapped = (void *)(uintptr_t)zc.address;

			outcome.native_fragment = true;
			outcome.native_data_ok = !memcmp(mapped, data, zc.length);
			break;
		}
		if (zc.recv_skip_hint) {
			size_t left = zc.recv_skip_hint;

			while (left) {
				ssize_t n = read(receiver, scratch,
					left < sizeof(scratch) ? left : sizeof(scratch));

				if (n <= 0)
					goto out;
				left -= n;
				consumed += n;
			}
		} else {
			usleep(1000);
		}
	}
 out:
	if (area != MAP_FAILED)
		munmap(area, bytes + NATIVE_PAGE_SIZE);
	if (data_res != MAP_FAILED)
		munmap(data_res, bytes + 2 * NATIVE_PAGE_SIZE);
	if (receiver >= 0)
		close(receiver);
	if (sender >= 0)
		close(sender);
	if (listener >= 0)
		close(listener);
	alarm(0);
	return outcome;
}

static bool native_mode;

TEST(compat_tcp_mmap_returns_eopnotsupp)
{
	struct tcp_outcome outcome;

	if (native_mode)
		SKIP(return, "compat-only check");
	outcome = run_probe(true);
	if (outcome.unavailable)
		SKIP(return, "SO_ZEROCOPY unavailable");
	ASSERT_TRUE(outcome.setup_ok);
	EXPECT_TRUE(outcome.mmap_rejected);
}

TEST(compat_read_fallback_preserves_all_bytes)
{
	struct tcp_outcome outcome;

	if (native_mode)
		SKIP(return, "compat-only check");
	outcome = run_probe(true);
	if (outcome.unavailable)
		SKIP(return, "SO_ZEROCOPY unavailable");
	ASSERT_TRUE(outcome.setup_ok);
	ASSERT_TRUE(outcome.send_ok);
	EXPECT_TRUE(outcome.fallback_ok);
}

TEST(native_zero_copy_preserves_mapped_fragment)
{
	struct tcp_outcome outcome;

	if (!native_mode)
		SKIP(return, "native-only check");
	outcome = run_probe(false);
	if (outcome.unavailable)
		SKIP(return, "SO_ZEROCOPY unavailable");
	ASSERT_TRUE(outcome.setup_ok);
	ASSERT_TRUE(outcome.send_ok);
	ASSERT_FALSE(outcome.native_failed);
	if (!outcome.native_fragment)
		SKIP(return, "zero-copy fragments unavailable");
	EXPECT_TRUE(outcome.native_data_ok);
}

int main(int argc, char **argv)
{
	p3s_argv0 = argv[0];
	if (argc == 2 && !strcmp(argv[1], "--native")) {
		if (p3s_is_compat_process())
			exec_native(argv[0], "--native", NULL);
		native_mode = true;
		return p3s_harness_run();
	}
	return p3s_compat_main(argc, argv, p3s_harness_run);
}
