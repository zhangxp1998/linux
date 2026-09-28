// SPDX-License-Identifier: GPL-2.0
/* Verify that a P3S exec copies large argv and environment strings. */
#define _GNU_SOURCE

#define P3S_HARNESS_ROLE check_exec_copy
#include "p3s_harness.h"

#define ENV_COUNT 6
#define ENV_SIZE 3584
#define LAUNCH_ROLE "--native-launch"
#define ARGS_ROLE "--check-exec-args"
#define ENV_ROLE "--check-exec-env"

static int launch_4kb_exec(const char *role)
{
	char name[32];
	char *values[ENV_COUNT];
	unsigned int i;

	if (getpagesize() != NATIVE_PAGE_SIZE)
		return EXIT_FAILURE;
	for (i = 0; i < ENV_COUNT; i++) {
		values[i] = malloc(ENV_SIZE + 1);
		if (!values[i])
			return EXIT_FAILURE;
		memset(values[i], 'A' + i, ENV_SIZE);
		values[i][ENV_SIZE] = '\0';
		snprintf(name, sizeof(name), "P3S_EXEC_ENV_%u", i);
		if (setenv(name, values[i], 1))
			return EXIT_FAILURE;
	}
	p3s_execl(true, NULL, role, values[0], values[1], values[2],
		  values[3], values[4], values[5], NULL);
	return EXIT_FAILURE;
}

static int check_exec_copy(int argc, char **argv)
{
	const bool args = !strcmp(argv[1], ARGS_ROLE);
	const bool env = !strcmp(argv[1], ENV_ROLE);
	char name[32];
	unsigned int i;

	if (argc == 3 && !strcmp(argv[1], LAUNCH_ROLE) &&
	    (!strcmp(argv[2], ARGS_ROLE) || !strcmp(argv[2], ENV_ROLE)))
		return launch_4kb_exec(argv[2]);
	if (!args && !env)
		return -1;
	if (!p3s_is_4kb_process() || argc != ENV_COUNT + 2)
		return EXIT_FAILURE;
	for (i = 0; i < ENV_COUNT; i++) {
		const char *value;
		size_t j;

		snprintf(name, sizeof(name), "P3S_EXEC_ENV_%u", i);
		value = args ? argv[i + 2] : getenv(name);
		if (!value)
			return EXIT_FAILURE;
		for (j = 0; j < ENV_SIZE; j++)
			if (value[j] != 'A' + (int)i)
				return EXIT_FAILURE;
		if (value[ENV_SIZE] != '\0')
			return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

static int run_exec_copy(const char *role)
{
	return p3s_run_child(false, LAUNCH_ROLE, role, NULL);
}

TEST(exec_preserves_arguments_across_native_slices)
{
	ASSERT_EQ(EXIT_SUCCESS, run_exec_copy(ARGS_ROLE));
}

TEST(exec_preserves_environment_across_native_slices)
{
	ASSERT_EQ(EXIT_SUCCESS, run_exec_copy(ENV_ROLE));
}

P3S_HARNESS_4KB_MAIN
