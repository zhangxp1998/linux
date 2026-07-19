// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/socket.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL
#define RING_SIZE (4 * USER_PAGE_SIZE)
#define FRAME_SIZE 2048U

static int run_test(void)
{
	struct tpacket_req3 req = {
		.tp_block_size = RING_SIZE,
		.tp_block_nr = 1,
		.tp_frame_size = FRAME_SIZE,
		.tp_frame_nr = RING_SIZE / FRAME_SIZE,
		.tp_retire_blk_tov = 64,
	};
	unsigned char residency[RING_SIZE / USER_PAGE_SIZE] = {};
	int version = TPACKET_V3;
	void *offset_ring;
	void *ring = MAP_FAILED;
	bool resident = true;
	int saved_errno;
	int fd;
	int i;

	ksft_print_header();
	ksft_set_plan(6);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL));
	ksft_test_result(fd >= 0, "create an AF_PACKET socket\n");
	if (fd < 0)
		ksft_exit_fail_msg("AF_PACKET socket failed: %s\n",
				   strerror(errno));

	if (setsockopt(fd, SOL_PACKET, PACKET_VERSION,
		       &version, sizeof(version)) ||
	    setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &req, sizeof(req)))
		ksft_exit_fail_msg("packet ring setup failed: %s\n",
				   strerror(errno));
	ksft_test_result_pass("configure a 16K packet ring\n");

	errno = 0;
	offset_ring = mmap(NULL, RING_SIZE, PROT_READ | PROT_WRITE,
			   MAP_SHARED, fd, USER_PAGE_SIZE);
	saved_errno = errno;
	ksft_test_result(offset_ring == MAP_FAILED,
			 "reject a packet-ring mmap at offset 4K (errno=%d)\n",
			 saved_errno);
	if (offset_ring != MAP_FAILED)
		munmap(offset_ring, RING_SIZE);

	ring = mmap(NULL, RING_SIZE, PROT_READ | PROT_WRITE,
		    MAP_SHARED | MAP_POPULATE, fd, 0);
	ksft_test_result(ring != MAP_FAILED, "map the packet ring\n");
	if (ring == MAP_FAILED)
		ksft_exit_fail_msg("packet ring mmap failed: %s\n",
				   strerror(errno));
	if (mincore(ring, RING_SIZE, residency))
		ksft_exit_fail_msg("packet ring mincore failed: %s\n",
				   strerror(errno));
	for (i = 0; i < (int)sizeof(residency); i++)
		resident &= residency[i] & 1;
	ksft_test_result(resident,
			 "map all four 4K slices of the packet ring\n");

	munmap(ring, RING_SIZE);
	close(fd);
	ksft_finished();
}

static int exec_compat(void)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0)
		ksft_exit_fail_msg("personality get failed: %s\n",
				   strerror(errno));
	if (personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("personality set failed: %s\n",
				   strerror(errno));
	execl("/proc/self/exe", "packet_mmap_ppps", "--run", NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return exec_compat();
	if (argc == 2 && !strcmp(argv[1], "--run"))
		return run_test();
	return EXIT_FAILURE;
}
