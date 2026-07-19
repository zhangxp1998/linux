// SPDX-License-Identifier: GPL-2.0
/*
 * AF_PACKET TPACKET_V3 rings of a 4K compat process map with every 4K slice
 * resident, reject an mmap at file offset 4K, and back 4K blocks separately.
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <sys/mman.h>
#include <sys/socket.h>

#include "kselftest_ppps.h"


#define RING_SIZE (4 * PROCESS_PAGE_SIZE)
#define FRAME_SIZE 2048U

static bool test_subpage_blocks(void)
{
	struct tpacket_req3 req = {
		.tp_block_size = PROCESS_PAGE_SIZE,
		.tp_block_nr = 2,
		.tp_frame_size = FRAME_SIZE,
		.tp_frame_nr = 2 * PROCESS_PAGE_SIZE / FRAME_SIZE,
		.tp_retire_blk_tov = 64,
	};
	unsigned char residency[2] = {};
	int version = TPACKET_V3;
	void *ring = MAP_FAILED;
	bool pass = false;
	int fd;

	fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL));
	if (fd < 0)
		return false;
	if (setsockopt(fd, SOL_PACKET, PACKET_VERSION,
		       &version, sizeof(version)) ||
	    setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &req, sizeof(req)))
		goto out;
	ring = mmap(NULL, 2 * PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		    MAP_SHARED | MAP_POPULATE, fd, 0);
	if (ring == MAP_FAILED)
		goto out;
	if (mincore(ring, 2 * PROCESS_PAGE_SIZE, residency))
		goto out;
	pass = (residency[0] & 1) && (residency[1] & 1);
out:
	if (ring != MAP_FAILED)
		munmap(ring, 2 * PROCESS_PAGE_SIZE);
	close(fd);
	return pass;
}

static int run_test(void)
{
	struct tpacket_req3 req = {
		.tp_block_size = RING_SIZE,
		.tp_block_nr = 1,
		.tp_frame_size = FRAME_SIZE,
		.tp_frame_nr = RING_SIZE / FRAME_SIZE,
		.tp_retire_blk_tov = 64,
	};
	unsigned char residency[RING_SIZE / PROCESS_PAGE_SIZE] = {};
	int version = TPACKET_V3;
	void *offset_ring;
	void *ring = MAP_FAILED;
	bool resident = true;
	int saved_errno;
	int fd;
	int i;

	ksft_print_header();
	ksft_set_plan(6);
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
			   MAP_SHARED, fd, PROCESS_PAGE_SIZE);
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
	ksft_test_result(test_subpage_blocks(),
			 "configure and map two separately backed 4K blocks\n");

	munmap(ring, RING_SIZE);
	close(fd);
	ksft_finished();
}

PPPS_COMPAT_MAIN(run_test)
