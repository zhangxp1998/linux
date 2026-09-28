// SPDX-License-Identifier: GPL-2.0
/*
 * Truncating a memfd at a native-page boundary under a 4K compat process's
 * two-slice mapping keeps the slice below the new EOF readable and makes the
 * slice at the new EOF fault with SIGBUS.
 */
#define _GNU_SOURCE

#include <linux/memfd.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>

#include "kselftest_p3s.h"
#include "p3s_harness.h"

#define MAPPING_OFFSET	(3 * PROCESS_PAGE_SIZE)
#define MAPPING_SIZE	(2 * PROCESS_PAGE_SIZE)
#define FILE_SIZE	(8 * PROCESS_PAGE_SIZE)
#define TRUNCATED_SIZE	(4 * PROCESS_PAGE_SIZE)

static sigjmp_buf fault_env;

static unsigned char page_pattern(unsigned int page)
{
	return 0x41 + page;
}

static bool initialize_file(int fd)
{
	unsigned char page[PROCESS_PAGE_SIZE];
	unsigned int i;

	for (i = 0; i < FILE_SIZE / PROCESS_PAGE_SIZE; i++) {
		ssize_t written;

		memset(page, page_pattern(i), sizeof(page));
		written = pwrite(fd, page, sizeof(page), i * PROCESS_PAGE_SIZE);
		if (written != (ssize_t)sizeof(page)) {
			ksft_print_msg("pwrite page %u returned %zd: %s\n", i,
				       written, strerror(errno));
			return false;
		}
	}
	return true;
}

static void sigbus_handler(int signal_number)
{
	siglongjmp(fault_env, signal_number);
}

static bool read_byte(const unsigned char *address, unsigned char *value)
{
	if (sigsetjmp(fault_env, 1))
		return false;
	*value = *address;
	return true;
}

TEST(p3s_truncate)
{
	unsigned char *mapping;
	struct sigaction action = {
		.sa_handler = sigbus_handler,
	};
	unsigned char truncated_value = 0;
	bool initial_contents;
	bool truncated_ok;
	bool truncated_readable;
	int fd;

	fd = memfd_create("truncate-p3s", MFD_CLOEXEC);
	ASSERT_GE(fd, 0);
	ASSERT_TRUE(initialize_file(fd));
	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ, MAP_SHARED, fd,
		       MAPPING_OFFSET);
	ASSERT_NE(MAP_FAILED, mapping);

	initial_contents = mapping[0] == page_pattern(3) &&
			   mapping[PROCESS_PAGE_SIZE] == page_pattern(4);
	EXPECT_TRUE(initial_contents);

	truncated_ok = !ftruncate(fd, TRUNCATED_SIZE);
	EXPECT_TRUE(truncated_ok);
	if (!truncated_ok)
		ksft_print_msg("ftruncate failed: %s\n", strerror(errno));

	EXPECT_TRUE(mapping[0] == page_pattern(3));
	sigemptyset(&action.sa_mask);
	ASSERT_EQ(0, sigaction(SIGBUS, &action, NULL));
	truncated_readable = read_byte(&mapping[PROCESS_PAGE_SIZE],
				       &truncated_value);
	EXPECT_TRUE(!truncated_readable);
	if (truncated_readable)
		ksft_print_msg("post-EOF access returned %#x\n", truncated_value);

	munmap(mapping, MAPPING_SIZE);
	close(fd);
}

P3S_HARNESS_4KB_MAIN
