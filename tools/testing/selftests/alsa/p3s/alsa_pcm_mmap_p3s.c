// SPDX-License-Identifier: GPL-2.0
/* Native PCM access remains usable; 4K compat PCM mmap is unsupported. */
#define _GNU_SOURCE

#include <glob.h>
#include <sound/asound.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "p3s_harness.h"

static int run_case(bool compat)
{
	glob_t paths = {0};
	void *map;
	int fd, version, saved_errno, ret = KSFT_PASS;

	if (glob("/dev/snd/pcmC*D*p", 0, NULL, &paths) || !paths.gl_pathc)
		return KSFT_SKIP;
	fd = open(paths.gl_pathv[0], O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		ksft_print_msg("open %s: %s\n", paths.gl_pathv[0], strerror(errno));
		globfree(&paths);
		return errno == ENOENT ? KSFT_SKIP : KSFT_FAIL;
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
		errno = 0;
		map = mmap(NULL, getpagesize(), PROT_READ, MAP_SHARED, fd,
			   SNDRV_PCM_MMAP_OFFSET_STATUS_NEW + PROCESS_PAGE_SIZE);
		saved_errno = errno;
		if (map != MAP_FAILED)
			munmap(map, getpagesize());
		if (map != MAP_FAILED || saved_errno != ENXIO) {
			ksft_print_msg("compat shifted PCM status mmap: expected ENXIO, got %s (%d)\n",
				       map == MAP_FAILED ? strerror(saved_errno) : "success",
				       saved_errno);
			ret = KSFT_FAIL;
		}
	} else if (map == MAP_FAILED && saved_errno == EOPNOTSUPP) {
		ksft_print_msg("native PCM mmap unexpectedly unsupported\n");
		ret = KSFT_FAIL;
	} else if (!compat && map == MAP_FAILED) {
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
	int status;
	pid_t pid = fork();

	if (pid < 0)
		ksft_exit_fail_msg("fork: %s\n", strerror(errno));
	if (!pid) {
		p3s_execl(compat, NULL, compat ? "--compat" : "--native", NULL);
		_exit(KSFT_SKIP);
	}
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
		return KSFT_FAIL;
	return WEXITSTATUS(status);
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

int main(int argc, char **argv)
{
	const char *mode = p3s_run_mode(argc, argv, NULL);

	if (mode && !strcmp(mode, "--native"))
		return p3s_is_compat_process() ? KSFT_SKIP : run_case(false);
	if (mode && !strcmp(mode, "--compat"))
		return p3s_is_compat_process() ? run_case(true) : KSFT_SKIP;

	return test_harness_run(argc, argv);
}
