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

static inline bool p3s_harness_has_flag(int argc, char **argv, const char *flag)
{
	int i;

	for (i = 1; i < argc; i++)
		if (!strcmp(argv[i], flag))
			return true;
	return false;
}

static inline void p3s_harness_strip_flag(int *argc, char **argv,
					  const char *flag)
{
	int i, j;

	for (i = 1; i < *argc; i++) {
		if (strcmp(argv[i], flag))
			continue;
		for (j = i; j < *argc; j++)
			argv[j] = argv[j + 1];
		(*argc)--;
		i--;
	}
}

static inline void p3s_harness_require_4kb(void)
{
	if (!p3s_is_4kb_process()) {
		ksft_print_header();
		ksft_exit_skip("P3S 4KB process unavailable\n");
	}
}

static inline void p3s_harness_require_native_4kb(void)
{
	int status;

	if (!p3s_has_native_pages()) {
		ksft_print_header();
		ksft_exit_skip("requires native 16K pages\n");
	}
	status = p3s_run_child(true, "--p3s-probe", NULL);
	if (status == KSFT_SKIP) {
		ksft_print_header();
		ksft_exit_skip("4KB exec is unavailable\n");
	}
	if (status != KSFT_PASS) {
		ksft_print_header();
		ksft_exit_fail_msg("4KB probe failed: status=%d\n", status);
	}
}

/* One entry for normal tests, native controls, and child-only roles. */
static inline int p3s_harness_entry(int argc, char **argv,
				    int (*role)(int, char **),
				    bool native_default,
				    bool native_control)
{
	p3s_argv0 = argv[0];
	if (p3s_harness_listing(argc, argv)) {
		if (p3s_harness_has_flag(argc, argv, "--native"))
			p3s_harness_strip_flag(&argc, argv, "--native");
		return test_harness_run(argc, argv);
	}
#ifndef __aarch64__
	(void)role;
	(void)native_default;
	(void)native_control;
	ksft_print_header();
	ksft_exit_skip("requires arm64 16K native / P3S 4K geometry\n");
#else
	int result, err;
	bool native, native_requested, reexec;

	if (argc == 2 && !strcmp(argv[1], "--p3s-probe")) {
		int persona = personality(0xffffffffUL);

		_exit(persona >= 0 && (persona & ADDR_PAGE_SIZE_4KB) &&
		      getpagesize() == PROCESS_PAGE_SIZE ?
		      KSFT_PASS : KSFT_SKIP);
	}
	reexec = !!getenv("P3S_HARNESS_REEXEC");
	unsetenv("P3S_HARNESS_REEXEC");
	if (role && argc > 1) {
		result = role(argc, argv);
		if (result >= 0)
			return result;
		optind = 1;
	}
	if (p3s_harness_has_flag(argc, argv, "--native") &&
	    !native_default && !native_control) {
		ksft_print_header();
		ksft_exit_skip("this test requires a 4KB process\n");
	}
	native_requested = p3s_harness_has_flag(argc, argv, "--native");
	native = native_default || native_requested;
	if (native) {
		if (p3s_is_4kb_process()) {
			if (reexec) {
				ksft_print_header();
				ksft_exit_fail_msg("native re-exec stayed in 4KB mode\n");
			}
			if (setenv("P3S_HARNESS_REEXEC", "1", 1)) {
				err = errno;
				ksft_print_header();
				ksft_exit_fail_msg("set re-exec marker: %s\n",
						   strerror(err));
			}
			p3s_execv(false, argv);
			err = errno;
			ksft_print_header();
			ksft_exit_fail_msg("native re-exec failed: %s\n",
					   strerror(err));
		}
		if (reexec && !p3s_has_native_pages()) {
			ksft_print_header();
			ksft_exit_fail_msg("native re-exec did not restore 16K pages\n");
		}
		p3s_harness_require_native_4kb();
		if (native_requested)
			p3s_harness_strip_flag(&argc, argv, "--native");
	} else if (!p3s_is_4kb_process()) {
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
	if (!native)
		p3s_harness_require_4kb();
	return test_harness_run(argc, argv);
#endif
}

#ifndef P3S_HARNESS_ROLE
#define P3S_HARNESS_ROLE NULL
#endif

#ifndef P3S_HARNESS_DEFAULT_NATIVE
#define P3S_HARNESS_DEFAULT_NATIVE 0
#endif

#ifndef P3S_HARNESS_NATIVE_CONTROL
#define P3S_HARNESS_NATIVE_CONTROL 0
#endif

#define P3S_HARNESS_4KB_MAIN			\
int main(int argc, char **argv)			\
{						\
	return p3s_harness_entry(argc, \
				 argv, \
				 P3S_HARNESS_ROLE, \
				 P3S_HARNESS_DEFAULT_NATIVE, \
				 P3S_HARNESS_NATIVE_CONTROL); \
}

#endif /* __P3S_HARNESS_H */
