/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __P3S_HARNESS_H
#define __P3S_HARNESS_H

#include "kselftest_p3s.h"
#include "kselftest_harness.h"

static inline bool p3s_harness_listing(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		const char *p = argv[i];

		if (!strcmp(p, "--"))
			break;
		if (p[0] != '-' || p[1] == '-' || p[1] == '\0')
			continue;
		for (p++; *p; p++) {
			if (*p == 'l')
				return true;
			if (strchr("FfvVtTr", *p)) {
				if (!p[1])
					i++;
				break;
			}
		}
	}
	return false;
}

static inline void p3s_harness_require_4kb(void)
{
	if (!p3s_is_4kb_process()) {
		ksft_print_header();
		ksft_exit_skip("P3S 4KB process unavailable\n");
	}
}

/* One entry for normal tests, native controls, and child-only roles. */
static inline int p3s_harness_entry(int argc, char **argv,
				    int (*role)(int, char **))
{
	p3s_argv0 = argv[0];
	if (p3s_harness_listing(argc, argv))
		return test_harness_run(argc, argv);
#ifndef __aarch64__
	(void)role;
	ksft_print_header();
	ksft_exit_skip("requires arm64 16K native / P3S 4K geometry\n");
#else
	int result, err;
	bool reexec;

	reexec = !!getenv("P3S_HARNESS_REEXEC");
	unsetenv("P3S_HARNESS_REEXEC");
	if (role && argc > 1) {
		result = role(argc, argv);
		if (result >= 0)
			return result;
		optind = 1;
	}
	if (!p3s_is_4kb_process()) {
		if (reexec)
			p3s_harness_require_4kb();
		if (!p3s_has_native_pages()) {
			ksft_print_header();
			ksft_exit_skip("native exec does not use 16K pages\n");
		}
		if (setenv("P3S_HARNESS_REEXEC", "1", 1)) {
			err = errno;
			ksft_print_header();
			ksft_exit_fail_msg("set re-exec marker: %s\n",
					   strerror(err));
		}
		p3s_execv(true, argv);
		err = errno;
		ksft_print_header();
		if (err == EINVAL || err == EPERM)
			ksft_exit_skip("P3S personality unavailable: %s\n",
				       strerror(err));
		ksft_exit_fail_msg("P3S re-exec failed: %s\n", strerror(err));
	}
	p3s_harness_require_4kb();
	return test_harness_run(argc, argv);
#endif
}

#ifndef P3S_HARNESS_ROLE
#define P3S_HARNESS_ROLE NULL
#endif

#define P3S_HARNESS_4KB_MAIN			\
int main(int argc, char **argv)			\
{						\
	return p3s_harness_entry(argc, \
				 argv, \
				 P3S_HARNESS_ROLE); \
}

#endif /* __P3S_HARNESS_H */
