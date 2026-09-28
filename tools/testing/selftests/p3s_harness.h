/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __P3S_HARNESS_H
#define __P3S_HARNESS_H

#include "kselftest_p3s.h"
#include "kselftest_harness.h"

/* Re-exec once in the 4K process before the harness forks individual tests. */
static inline int p3s_harness_run(void)
{
	char *argv[] = { (char *)p3s_argv0, NULL };

	return test_harness_run(1, argv);
}

/* Keep the harness's -l/-t options intact across the 4KB re-exec. */
static inline int p3s_harness_main(int argc, char **argv)
{
	p3s_argv0 = argv[0];
#ifndef __aarch64__
	ksft_print_header();
	ksft_exit_skip("requires arm64 16K native / P3S 4K geometry\n");
#else
	if (!getenv("P3S_HARNESS_REEXEC")) {
		if (p3s_is_compat_process())
			return test_harness_run(argc, argv);
		if (getpagesize() != NATIVE_PAGE_SIZE) {
			ksft_print_header();
			ksft_exit_skip("native exec does not use 16K pages\n");
		}
		if (setenv("P3S_HARNESS_REEXEC", "1", 1))
			ksft_exit_fail_msg("set re-exec marker: %s\n", strerror(errno));
		p3s_execv(true, argv);
		if (errno == EINVAL || errno == EPERM) {
			ksft_print_header();
			ksft_exit_skip("P3S personality unavailable: %s\n",
				       strerror(errno));
		}
		ksft_exit_fail_msg("P3S re-exec failed: %s\n", strerror(errno));
	}
	unsetenv("P3S_HARNESS_REEXEC");
	p3s_require_compat();
	return test_harness_run(argc, argv);
#endif
}

#define P3S_HARNESS_COMPAT_MAIN			\
int main(int argc, char **argv)			\
{						\
	return p3s_harness_main(argc, argv);	\
}

#endif /* __P3S_HARNESS_H */
