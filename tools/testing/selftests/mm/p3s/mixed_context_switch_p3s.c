// SPDX-License-Identifier: GPL-2.0
/*
 * A native (16K) parent and a 4K compat child pinned to the same CPU hand
 * off through a shared futex. Both use the same virtual address, with
 * different private canaries, and verify every slice after each handoff.
 * Forks stress the TG0/T0SZ context switch and the ASID handling fixed
 * by aosp/4367990 while both MMs stay alive.
 */
#define _GNU_SOURCE

#include <linux/futex.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define P3S_HARNESS_DEFAULT_NATIVE 1
#define P3S_HARNESS_ROLE child_role
#define P3S_HARNESS_SETUP setup_harness
#include "p3s_harness.h"

#define DEFAULT_ROUNDS 50000
#define DEFAULT_FORKS 32
#define TEST_ADDRESS ((void *)(1UL << 32))

struct shared_state {
	int turn;
	int ready;
	int child_error;
	long child_page_size;
	unsigned long parent_rounds;
	unsigned long child_rounds;
	unsigned long forks;
};

static int futex_wait(int *address, int value)
{
	int ret;

	do {
		ret = syscall(SYS_futex, address, FUTEX_WAIT, value,
			      NULL, NULL, 0);
	} while (ret && errno == EINTR);
	return ret && errno != EAGAIN ? -1 : 0;
}

static int futex_wake(int *address)
{
	return syscall(SYS_futex, address, FUTEX_WAKE, 1, NULL, NULL, 0) < 0 ?
		-1 : 0;
}

static int wait_for_value(int *address, int value)
{
	for (;;) {
		int current = __atomic_load_n(address, __ATOMIC_ACQUIRE);

		if (current == value)
			return 0;
		if (futex_wait(address, current))
			return -1;
	}
}

static int pin_to_cpu_zero(void)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(0, &set);
	return sched_setaffinity(0, sizeof(set), &set);
}

static unsigned char canary(unsigned long round, unsigned int slice,
			    bool child)
{
	return ((round + 17 * slice) & 0x7f) | (child ? 0x80 : 0);
}

static void fill_mapping(volatile unsigned char *mapping, unsigned long round,
			 bool child)
{
	unsigned int slice;

	for (slice = 0; slice < NATIVE_PAGE_SIZE / PROCESS_PAGE_SIZE; slice++) {
		unsigned char value = canary(round, slice, child);

		mapping[slice * PROCESS_PAGE_SIZE] = value;
		mapping[(slice + 1) * PROCESS_PAGE_SIZE - 1] = value;
	}
}

static int check_mapping(volatile unsigned char *mapping, unsigned long round,
			 bool child)
{
	unsigned int slice;

	for (slice = 0; slice < NATIVE_PAGE_SIZE / PROCESS_PAGE_SIZE; slice++) {
		unsigned char value = canary(round, slice, child);

		if (mapping[slice * PROCESS_PAGE_SIZE] != value ||
		    mapping[(slice + 1) * PROCESS_PAGE_SIZE - 1] != value)
			return -1;
	}
	return 0;
}

static int churn_asids(unsigned char *mapping, unsigned long forks,
		       unsigned long round, bool child)
{
	unsigned long i;

	for (i = 0; i < forks; i++) {
		int status;
		pid_t pid = fork();

		if (pid < 0)
			return -1;
		if (!pid) {
			if (check_mapping(mapping, round, child))
				_exit(EXIT_FAILURE);
			fill_mapping(mapping, i, !child);
			_exit(check_mapping(mapping, i, !child) ?
			      EXIT_FAILURE : EXIT_SUCCESS);
		}
		if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
		    WEXITSTATUS(status) || check_mapping(mapping, round, child))
			return -1;
	}
	return 0;
}

static int run_child(int fd, unsigned long rounds)
{
	struct shared_state *shared;
	unsigned char *mapping;
	unsigned long round;
	int error = 0;

	shared = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_SHARED, fd, 0);
	if (shared == MAP_FAILED)
		return EXIT_FAILURE;
	mapping = mmap(TEST_ADDRESS, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (mapping != TEST_ADDRESS || pin_to_cpu_zero())
		error = errno ? errno : EFAULT;
	if (!error && sysconf(_SC_PAGESIZE) != PROCESS_PAGE_SIZE)
		error = EINVAL;
	shared->child_page_size = sysconf(_SC_PAGESIZE);
	shared->child_error = error;
	__atomic_store_n(&shared->ready, 1, __ATOMIC_RELEASE);
	futex_wake(&shared->ready);
	if (error)
		return EXIT_FAILURE;
	fill_mapping(mapping, 0, true);

	for (round = 0; round < rounds; round++) {
		if (wait_for_value(&shared->turn, 1) ||
		    check_mapping(mapping, round, true)) {
			shared->child_error = errno ? errno : EIO;
			return EXIT_FAILURE;
		}
		fill_mapping(mapping, round + 1, true);
		shared->child_rounds = round + 1;
		__atomic_store_n(&shared->turn, 0, __ATOMIC_RELEASE);
		if (futex_wake(&shared->turn)) {
			shared->child_error = errno;
			return EXIT_FAILURE;
		}
	}
	if (churn_asids(mapping, shared->forks, rounds, true)) {
		shared->child_error = EIO;
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

static int exec_compat_child(int fd, unsigned long rounds)
{
	char fd_arg[16];
	char rounds_arg[32];

	snprintf(fd_arg, sizeof(fd_arg), "%d", fd);
	snprintf(rounds_arg, sizeof(rounds_arg), "%lu", rounds);
	p3s_execl(true, NULL, "--child", fd_arg, rounds_arg, NULL);
	return EXIT_FAILURE;
}

struct switch_outcome {
	bool native_parent, mapped, peer_started, peer_compat;
	bool rounds_completed, asid_ok, peer_exits;
	bool both_rounds, no_child_error, canaries_ok;
	bool affinity_restored;
	unsigned long parent_rounds, child_rounds;
	int child_error;
};

static struct switch_outcome observe_switch(unsigned long rounds,
					    unsigned long forks)
{
	struct switch_outcome outcome = {};
	struct shared_state *shared = MAP_FAILED;
	unsigned char *mapping = MAP_FAILED;
	cpu_set_t saved_cpus;
	int child_status = 0, fd = -1;
	unsigned long round;
	pid_t child = -1;
	bool affinity_set = false;

	outcome.native_parent = sysconf(_SC_PAGESIZE) == NATIVE_PAGE_SIZE;
	fd = syscall(SYS_memfd_create, "mixed-context-switch", 0);
	if (fd < 0 || ftruncate(fd, NATIVE_PAGE_SIZE))
		goto out;
	shared = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_SHARED, fd, 0);
	mapping = mmap(TEST_ADDRESS, NATIVE_PAGE_SIZE,
		       PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	outcome.mapped = shared != MAP_FAILED && mapping == TEST_ADDRESS;
	if (!outcome.mapped)
		goto out;
	memset(shared, 0, NATIVE_PAGE_SIZE);
	shared->forks = forks;
	fill_mapping(mapping, 0, false);
	if (sched_getaffinity(0, sizeof(saved_cpus), &saved_cpus) ||
	    pin_to_cpu_zero())
		goto out;
	affinity_set = true;
	child = fork();
	if (child < 0)
		goto out;
	if (!child)
		_exit(exec_compat_child(fd, rounds));
	outcome.peer_started = !wait_for_value(&shared->ready, 1);
	outcome.peer_compat = shared->child_page_size == PROCESS_PAGE_SIZE &&
			      !shared->child_error;
	if (!outcome.peer_started || !outcome.peer_compat)
		goto out;
	for (round = 0; round < rounds; round++) {
		if (wait_for_value(&shared->turn, 0) ||
		    check_mapping(mapping, round, false))
			break;
		fill_mapping(mapping, round + 1, false);
		shared->parent_rounds = round + 1;
		__atomic_store_n(&shared->turn, 1, __ATOMIC_RELEASE);
		if (futex_wake(&shared->turn))
			break;
	}
	outcome.rounds_completed = round == rounds;
	if (!outcome.rounds_completed)
		goto out;
	outcome.asid_ok = !churn_asids(mapping, forks, rounds, false);
 out:
	if (child > 0) {
		if (!outcome.rounds_completed || !outcome.asid_ok)
			kill(child, SIGKILL);
		if (waitpid(child, &child_status, 0) == child)
			outcome.peer_exits = WIFEXITED(child_status) &&
					     WEXITSTATUS(child_status) == EXIT_SUCCESS;
	}
	if (shared != MAP_FAILED) {
		outcome.parent_rounds = shared->parent_rounds;
		outcome.child_rounds = shared->child_rounds;
		outcome.child_error = shared->child_error;
		outcome.both_rounds = shared->parent_rounds == rounds &&
				      shared->child_rounds == rounds;
		outcome.no_child_error = !shared->child_error;
	}
	if (mapping != MAP_FAILED && outcome.rounds_completed)
		outcome.canaries_ok = !check_mapping(mapping, rounds, false);
	if (affinity_set)
		outcome.affinity_restored =
			!sched_setaffinity(0, sizeof(saved_cpus), &saved_cpus);
	if (mapping != MAP_FAILED)
		munmap(mapping, NATIVE_PAGE_SIZE);
	if (shared != MAP_FAILED)
		munmap(shared, NATIVE_PAGE_SIZE);
	if (fd >= 0)
		close(fd);
	return outcome;
}

static struct switch_outcome result;
static unsigned long requested_rounds, requested_forks;

TEST(parent_uses_native_page_size)
{
	EXPECT_TRUE(result.native_parent);
}

TEST(map_private_canaries_at_common_address)
{
	EXPECT_TRUE(result.mapped);
}

TEST(start_same_cpu_4k_peer)
{
	EXPECT_TRUE(result.peer_started);
	EXPECT_TRUE(result.affinity_restored);
}

TEST(peer_enters_4k_compat_mode)
{
	EXPECT_TRUE(result.peer_compat)
		TH_LOG("child error=%d", result.child_error);
}

TEST(native_peer_completes_memory_checked_turns)
{
	EXPECT_TRUE(result.rounds_completed)
		TH_LOG("requested rounds=%lu", requested_rounds);
}

TEST(parent_asid_churn_keeps_private_mapping)
{
	EXPECT_TRUE(result.asid_ok)
		TH_LOG("requested forks=%lu", requested_forks);
}

TEST(compat_peer_exits_cleanly)
{
	EXPECT_TRUE(result.peer_exits);
}

TEST(both_peers_complete_every_handoff)
{
	EXPECT_TRUE(result.both_rounds)
		TH_LOG("parent=%lu child=%lu requested=%lu",
		       result.parent_rounds, result.child_rounds,
		       requested_rounds);
}

TEST(mixed_handoff_reports_no_memory_or_futex_error)
{
	EXPECT_TRUE(result.no_child_error)
		TH_LOG("child error=%d", result.child_error);
}

TEST(private_canaries_survive_switching_and_forks)
{
	EXPECT_TRUE(result.canaries_ok);
}

static int child_role(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "--child")) {
		alarm(600);
		return run_child(atoi(argv[2]), strtoul(argv[3], NULL, 0));
	}
	return -1;
}

static void setup_harness(int *argc, char ***argv)
{
	char *value;
	unsigned long rounds = DEFAULT_ROUNDS, forks = DEFAULT_FORKS;

	if (p3s_harness_listing(*argc, *argv))
		return;
	alarm(600);
	value = p3s_harness_take_positional(argc, argv);
	if (value)
		rounds = strtoul(value, NULL, 0);
	value = p3s_harness_take_positional(argc, argv);
	if (value)
		forks = strtoul(value, NULL, 0);
	if (!rounds)
		ksft_exit_fail_msg("round count must be positive\n");
	requested_rounds = rounds;
	requested_forks = forks;
	result = observe_switch(rounds, forks);
	alarm(0);
}

P3S_HARNESS_4KB_MAIN
