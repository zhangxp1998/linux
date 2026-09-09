// SPDX-License-Identifier: GPL-2.0
/* Native-only device APIs must also reject FDs inherited across compat exec. */
#define _GNU_SOURCE
#include <linux/io_uring.h>
#include <linux/kvm.h>
#include <linux/ublk_cmd.h>
#include <linux/vfio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>

#include "../kselftest_ppps.h"

/* One synchronous uring_cmd, using a fresh SQE128 ring in the calling mm. */
static int ctrl_cmd(int fd, unsigned int op, unsigned int id, void *data, size_t len)
{
	struct io_uring_params p = { .flags = IORING_SETUP_SQE128 };
	struct ublksrv_ctrl_cmd cmd = { .dev_id = id, .queue_id = -1,
		.addr = (uintptr_t)data, .len = len };
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	void *sq, *cq;
	size_t sl, cl;
	unsigned int *tail, *array;
	int ring, ret;

	ring = syscall(__NR_io_uring_setup, 2, &p);
	if (ring < 0)
		return -errno;
	sl = p.sq_off.array + p.sq_entries * sizeof(unsigned int);
	cl = p.cq_off.cqes + p.cq_entries * sizeof(*cqe);
	if (p.features & IORING_FEAT_SINGLE_MMAP) {
		sl = sl > cl ? sl : cl;
		cl = sl;
	}
	sq = mmap(NULL, sl, PROT_READ | PROT_WRITE, MAP_SHARED, ring, IORING_OFF_SQ_RING);
	cq = p.features & IORING_FEAT_SINGLE_MMAP ? sq :
		mmap(NULL, cl, PROT_READ | PROT_WRITE, MAP_SHARED, ring, IORING_OFF_CQ_RING);
	sqe = mmap(NULL, p.sq_entries * 128, PROT_READ | PROT_WRITE,
		   MAP_SHARED, ring, IORING_OFF_SQES);
	if (sq == MAP_FAILED || cq == MAP_FAILED || sqe == MAP_FAILED) {
		ret = -errno;
		goto out;
	}
	sqe->opcode = IORING_OP_URING_CMD;
	sqe->fd = fd;
	sqe->cmd_op = op;
	memcpy(sqe->cmd, &cmd, sizeof(cmd));
	array = (unsigned int *)((char *)sq + p.sq_off.array);
	array[0] = 0;
	tail = (unsigned int *)((char *)sq + p.sq_off.tail);
	__atomic_store_n(tail, 1, __ATOMIC_RELEASE);
	ret = syscall(__NR_io_uring_enter, ring, 1, 1, IORING_ENTER_GETEVENTS, NULL, 0);
	if (ret < 0) {
		ret = -errno;
		goto out;
	}
	tail = (unsigned int *)((char *)cq + p.cq_off.tail);
	if (!__atomic_load_n(tail, __ATOMIC_ACQUIRE)) {
		ret = -EIO;
		goto out;
	}
	cqe = (struct io_uring_cqe *)((char *)cq + p.cq_off.cqes);
	ret = cqe->res;
out:
	if (sqe != MAP_FAILED)
		munmap(sqe, p.sq_entries * 128);
	if (cq != sq && cq != MAP_FAILED)
		munmap(cq, cl);
	if (sq != MAP_FAILED)
		munmap(sq, sl);
	close(ring);
	return ret;
}

static int operation(int kind, int fd)
{
	uint64_t features;
	int ret;

	if (!kind)
		return ctrl_cmd(fd, UBLK_U_CMD_GET_FEATURES, 0, &features, sizeof(features));
	ret = ioctl(fd, kind == 1 ? VFIO_GET_API_VERSION : KVM_GET_API_VERSION, 0);
	if (ret < 0)
		return -errno;
	return ret == (kind == 1 ? VFIO_API_VERSION : KVM_API_VERSION) ? 0 : -EIO;
}

static bool refused_open(const char *path)
{
	int fd = open(path, O_RDWR);
	int error = errno;

	if (fd >= 0)
		close(fd);
	return fd < 0 && error == EOPNOTSUPP;
}

static int check_compat(int kind, int fd, const char *path)
{
	int result = 0, persona = personality(0xffffffffUL);

	if (getpagesize() != PROCESS_PAGE_SIZE)
		return 77;
	if (!refused_open(path))
		result |= 1;
	if (operation(kind, fd) != -EOPNOTSUPP)
		result |= 2;
	/* Clearing the next-exec personality must not bypass the current mm. */
	if (persona < 0 || personality(persona & ~ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		return 15;
	if (!refused_open(path))
		result |= 4;
	if (operation(kind, fd) != -EOPNOTSUPP)
		result |= 8;
	return result;
}

static int compat_child(int kind, int fd, const char *path)
{
	char kind_arg[12], fd_arg[12];
	pid_t pid;
	int status;

	snprintf(kind_arg, sizeof(kind_arg), "%d", kind);
	snprintf(fd_arg, sizeof(fd_arg), "%d", fd);
	fflush(NULL);
	pid = fork();
	if (!pid) {
		ppps_execl(true, NULL, "--child", kind_arg, fd_arg, path, NULL);
		_exit(15);
	}
	if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
		return 15;
	return WEXITSTATUS(status);
}

/* Native-created channel FD: check open, mmap, read/write and uring_cmd. */
static int check_channel(int fd, const char *path)
{
	char byte = 0;
	void *map;
	int bad = 0, ret;

	if (getpagesize() != PROCESS_PAGE_SIZE)
		return 77;
	bad |= !refused_open(path);
	map = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ, MAP_SHARED, fd, 0);
	bad |= map != MAP_FAILED || errno != EOPNOTSUPP;
	if (map != MAP_FAILED)
		munmap(map, PROCESS_PAGE_SIZE);
	ret = pread(fd, &byte, 1, 0);
	bad |= ret != -1 || errno != EOPNOTSUPP;
	ret = pwrite(fd, &byte, 1, 0);
	bad |= ret != -1 || errno != EOPNOTSUPP;
	bad |= ctrl_cmd(fd, UBLK_U_IO_FETCH_REQ, 0, NULL, 0) != -EOPNOTSUPP;
	return bad;
}

static void channel_test(int ctrl)
{
	struct ublksrv_ctrl_dev_info info = { .nr_hw_queues = 1, .queue_depth = 1,
		.max_io_buf_bytes = 65536, .dev_id = UINT32_MAX };
	char path[80], sysfs[128], fd_arg[12];
	unsigned int major, minor;
	FILE *dev;
	int fd = -1, ret, status = 1;
	bool made_node = false;
	pid_t pid;

	ret = ctrl_cmd(ctrl, UBLK_U_CMD_ADD_DEV, UINT32_MAX, &info, sizeof(info));
	if (ret) {
		ksft_test_result_fail("ublk native channel setup: %d\n", ret);
		return;
	}
	snprintf(path, sizeof(path), "/dev/ublkc%u", info.dev_id);
	fd = open(path, O_RDWR);
	if (fd < 0 && errno == ENOENT) {
		snprintf(sysfs, sizeof(sysfs), "/sys/class/ublk-char/ublkc%u/dev", info.dev_id);
		dev = fopen(sysfs, "r");
		if (dev) {
			if (fscanf(dev, "%u:%u", &major, &minor) == 2) {
				made_node = mknod(path, S_IFCHR | 0600, makedev(major, minor)) == 0;
				fd = open(path, O_RDWR);
			}
			fclose(dev);
		}
	}
	if (fd >= 0) {
		snprintf(fd_arg, sizeof(fd_arg), "%d", fd);
		fflush(NULL);
		pid = fork();
		if (!pid) {
			ppps_execl(true, NULL, "--channel", fd_arg, path, NULL);
			_exit(1);
		}
		if (pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status))
			status = WEXITSTATUS(status);
		else
			status = 1;
		close(fd);
	}
	ret = ctrl_cmd(ctrl, UBLK_U_CMD_DEL_DEV_ASYNC, info.dev_id, NULL, 0);
	if (made_node)
		unlink(path);
	if (status == 77 && ret == 0) {
		ksft_test_result_skip("ublk channel: PPPS exec unavailable\n");
		return;
	}
	ksft_test_result(fd >= 0 && status == 0 && ret == 0,
			 "ublk channel: compat open and inherited-FD mmap/read/write/uring rejected; native cleanup\n");
}

int main(int argc, char **argv)
{
	static const char * const paths[] = { "/dev/ublk-control", "/dev/vfio/vfio", "/dev/kvm" };
	static const char * const cases[] = { "compat open", "inherited FD",
		"compat open with personality cleared", "inherited FD with personality cleared" };
	unsigned int i, j;
	int fd, fd2, persona, ret;

	if (argc == 5 && !strcmp(argv[1], "--child"))
		return check_compat(atoi(argv[2]), atoi(argv[3]), argv[4]);
	if (argc == 4 && !strcmp(argv[1], "--channel"))
		return check_channel(atoi(argv[2]), argv[3]);
	if (argc == 1)
		exec_native(argv[0], "--native", NULL);
	ksft_print_header();
	if (getpagesize() != NATIVE_PAGE_SIZE)
		ksft_exit_skip("requires native 16K PPPS kernel\n");
	ksft_set_plan(19);
	for (i = 0; i < 3; i++) {
		fd = open(paths[i], O_RDWR);
		if (fd < 0 && ppps_fixture_unavailable(errno)) {
			for (j = 0; j < 6 + (i == 0); j++)
				ksft_test_result_skip("%s unavailable: %s\n",
						      paths[i], strerror(errno));
			continue;
		}
		ksft_test_result(fd >= 0 && operation(i, fd) == 0,
				 "%s native open/API\n", paths[i]);
		persona = personality(0xffffffffUL);
		ret = persona < 0 ? -1 : personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE);
		fd2 = open(paths[i], O_RDWR);
		ksft_test_result(ret >= 0 && fd2 >= 0 && operation(i, fd2) == 0,
				 "%s native mm with next-exec compat bit still works\n", paths[i]);
		if (fd2 >= 0)
			close(fd2);
		if (persona >= 0)
			personality(persona);
		ret = compat_child(i, fd, paths[i]);
		for (j = 0; j < 4; j++) {
			if (ret == 77)
				ksft_test_result_skip("%s: PPPS exec unavailable\n", paths[i]);
			else
				ksft_test_result(!(ret & (1U << j)), "%s %s rejected\n",
						 paths[i], cases[j]);
		}
		if (i == 0)
			channel_test(fd);
		if (fd >= 0)
			close(fd);
	}
	ksft_finished();
}
