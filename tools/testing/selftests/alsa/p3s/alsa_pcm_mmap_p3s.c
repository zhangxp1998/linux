// SPDX-License-Identifier: GPL-2.0
/* Native PCM access remains usable; 4K compat PCM mmap is unsupported. */
#define _GNU_SOURCE

#include <glob.h>
#include <sound/asound.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define P3S_HARNESS_DEFAULT_NATIVE 1
#define P3S_HARNESS_ROLE child_role
#include "p3s_harness.h"

static int run_case(bool compat)
{
	glob_t paths = {0};
	void *map;
	int fd, version, saved_errno, ret = KSFT_PASS;

	if (glob("/dev/snd/pcmC*D*p", 0, NULL, &paths) || !paths.gl_pathc) {
		globfree(&paths);
		return KSFT_SKIP;
	}
	fd = open(paths.gl_pathv[0], O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		saved_errno = errno;
		ksft_print_msg("open %s: %s\n", paths.gl_pathv[0],
			       strerror(saved_errno));
		globfree(&paths);
		return (p3s_fixture_unavailable(saved_errno) ||
			saved_errno == EBUSY) ? KSFT_SKIP : KSFT_FAIL;
	}
	if (ioctl(fd, SNDRV_PCM_IOCTL_PVERSION, &version)) {
		ksft_print_msg("PCM version ioctl: %s\n", strerror(errno));
		ret = KSFT_FAIL;
		goto out;
	}
	ksft_print_msg("PCM version %#x; mmap without HW_PARAMS\n", version);

	errno = 0;
	map = mmap(NULL, getpagesize(), PROT_READ, MAP_SHARED, fd, 0);
	saved_errno = errno;
	if (map != MAP_FAILED)
		munmap(map, getpagesize());
	if (compat) {
		if (map != MAP_FAILED || saved_errno != EOPNOTSUPP) {
			ksft_print_msg("compat PCM mmap: expected EOPNOTSUPP, got %s (%d)\n",
				       map == MAP_FAILED ? strerror(saved_errno) : "success",
				       saved_errno);
			ret = KSFT_FAIL;
		}
	} else if (map == MAP_FAILED && saved_errno == EOPNOTSUPP) {
		ksft_print_msg("native PCM mmap unexpectedly unsupported\n");
		ret = KSFT_FAIL;
	} else if (map == MAP_FAILED) {
		ksft_print_msg("native unconfigured PCM mmap: %s (%d)\n",
			       strerror(saved_errno), saved_errno);
	}
out:
	close(fd);
	globfree(&paths);
	return ret;
}

static int run_child(bool compat)
{
	return p3s_run_child(compat,
			compat ? "--compat" : "--native", NULL);
}

TEST(native_pcm_open_and_ioctl)
{
	int ret = run_child(false);

	if (ret == KSFT_SKIP)
		SKIP(return, "no native PCM device");
	EXPECT_EQ(KSFT_PASS, ret);
}

TEST(compat_pcm_mmap_rejected)
{
	int ret = run_child(true);

	if (ret == KSFT_SKIP)
		SKIP(return, "no compatible PCM device or P3S process");
	EXPECT_EQ(KSFT_PASS, ret);
}

static int child_role(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--native"))
		return p3s_is_4kb_process() ? KSFT_SKIP : run_case(false);
	if (argc == 2 && !strcmp(argv[1], "--compat"))
		return p3s_is_4kb_process() ? run_case(true) : KSFT_SKIP;
	return -1;
}

P3S_HARNESS_4KB_MAIN
