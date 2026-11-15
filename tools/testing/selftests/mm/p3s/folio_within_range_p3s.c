// SPDX-License-Identifier: GPL-2.0
/*
 * The in-kernel folio_within_range() fixture, driven from a 4K compat
 * process, recognizes a fully covered native folio and rejects truncated
 * query ranges. The rmap check separately covers a nonzero VMA slice offset.
 */
#define _GNU_SOURCE

#include <sys/ioctl.h>

#include "p3s_test.h"
#include "folio_within_range_p3s.h"
#include "p3s_harness.h"

TEST(p3s_folio_within_range)
{
	int fd;

	fd = p3s_open_fixture_or_skip("/dev/folio_within_range_p3s",
				       O_RDONLY);
	EXPECT_TRUE(fd >= 0);

	EXPECT_TRUE(ioctl(fd,
			       FOLIO_WITHIN_RANGE_P3S_CHECK_ALIGNED) == 0);
	EXPECT_TRUE(ioctl(fd,
			       FOLIO_WITHIN_RANGE_P3S_CHECK_SUBPAGE) == 0);
	EXPECT_TRUE(ioctl(fd,
			       FOLIO_WITHIN_RANGE_P3S_CHECK_SHORT_END) == 0);
	EXPECT_TRUE(ioctl(fd,
			       FOLIO_WITHIN_RANGE_P3S_CHECK_ADDRESS_END) == 0);

	close(fd);
}

P3S_HARNESS_COMPAT_MAIN
