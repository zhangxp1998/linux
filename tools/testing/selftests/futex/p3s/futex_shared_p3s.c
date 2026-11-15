// SPDX-License-Identifier: GPL-2.0
/*
 * Shared futexes of a 4KB process key on the file page, not the virtual
 * address: a waiter on one alias of a 4KB file slice is woken through a
 * tagged second alias mapped at a different native-page offset.
 */
#define _GNU_SOURCE

#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>

#include "p3s_harness.h"

#define POINTER_TAG 0xb4UL
#define WAKE_RETRIES 1000

struct alias_mapping {
	void *reservation;
	uint32_t *word;
};

struct waiter_state {
	uint32_t *word;
	atomic_int ready;
	atomic_int done;
	int result;
	int error;
};

static int futex_wait(uint32_t *word, uint32_t expected,
		      const struct timespec *timeout)
{
	return syscall(SYS_futex, word, FUTEX_WAIT, expected, timeout,
		       NULL, 0);
}

static int futex_wake(uint32_t *word, int count)
{
	return syscall(SYS_futex, word, FUTEX_WAKE, count, NULL, NULL, 0);
}

static struct alias_mapping map_alias(int fd, unsigned long native_offset)
{
	struct alias_mapping alias = {
		.reservation = MAP_FAILED,
		.word = MAP_FAILED,
	};
	uintptr_t aligned;
	void *reservation;
	void *mapping;

	reservation = mmap(NULL, 3 * NATIVE_PAGE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		return alias;

	aligned = ((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
		  ~(NATIVE_PAGE_SIZE - 1);
	mapping = mmap((void *)(aligned + native_offset), PROCESS_PAGE_SIZE,
		       PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
	if (mapping == MAP_FAILED) {
		munmap(reservation, 3 * NATIVE_PAGE_SIZE);
		return alias;
	}

	alias.reservation = reservation;
	alias.word = mapping;
	return alias;
}

static void unmap_alias(struct alias_mapping *alias)
{
	if (alias->reservation != MAP_FAILED)
		munmap(alias->reservation, 3 * NATIVE_PAGE_SIZE);
}

static void *waiter_thread(void *arg)
{
	struct waiter_state *state = arg;
	struct timespec timeout = {
		.tv_sec = 2,
	};

	atomic_store_explicit(&state->ready, 1, memory_order_release);
	state->result = futex_wait(state->word, 0, &timeout);
	state->error = state->result < 0 ? errno : 0;
	atomic_store_explicit(&state->done, 1, memory_order_release);
	return NULL;
}

TEST(shared_file_futex_aliases)
{
	struct alias_mapping alias_a = { .reservation = MAP_FAILED };
	struct alias_mapping alias_b = { .reservation = MAP_FAILED };
	struct waiter_state waiter = {};
	struct timespec delay = { .tv_nsec = 1000000 };
	uint32_t *tagged_b;
	pthread_t thread;
	int wake_error = 0;
	int wake_result = 0;
	int fd;
	int i;

	ASSERT_EQ(0, prctl(PR_SET_TAGGED_ADDR_CTRL, PR_TAGGED_ADDR_ENABLE,
			   0, 0, 0));
	fd = memfd_create("futex-shared", MFD_CLOEXEC);
	ASSERT_GE(fd, 0);
	if (ftruncate(fd, NATIVE_PAGE_SIZE)) {
		close(fd);
		ASSERT_EQ(0, -1);
	}

	alias_a = map_alias(fd, PROCESS_PAGE_SIZE);
	alias_b = map_alias(fd, 2 * PROCESS_PAGE_SIZE);
	if (alias_a.word == MAP_FAILED || alias_b.word == MAP_FAILED) {
		unmap_alias(&alias_a);
		unmap_alias(&alias_b);
		close(fd);
		ASSERT_TRUE(false);
	}

	*alias_a.word = 0;
	ASSERT_EQ(0U, *alias_b.word);

	waiter.word = alias_a.word;
	if (pthread_create(&thread, NULL, waiter_thread, &waiter)) {
		unmap_alias(&alias_a);
		unmap_alias(&alias_b);
		close(fd);
		ASSERT_TRUE(false);
	}

	while (!atomic_load_explicit(&waiter.ready, memory_order_acquire))
		sched_yield();

	tagged_b = (uint32_t *)((uintptr_t)alias_b.word | (POINTER_TAG << 56));
	for (i = 0; i < WAKE_RETRIES; i++) {
		wake_result = futex_wake(tagged_b, 1);
		if (wake_result == 1)
			break;
		if (wake_result < 0) {
			wake_error = errno;
			break;
		}
		if (atomic_load_explicit(&waiter.done, memory_order_acquire))
			break;
		nanosleep(&delay, NULL);
	}

	pthread_join(thread, NULL);
	EXPECT_EQ(1, wake_result);
	EXPECT_EQ(0, wake_error);
	EXPECT_EQ(0, waiter.result);
	EXPECT_EQ(0, waiter.error);

	unmap_alias(&alias_a);
	unmap_alias(&alias_b);
	close(fd);
}

P3S_HARNESS_4KB_MAIN
