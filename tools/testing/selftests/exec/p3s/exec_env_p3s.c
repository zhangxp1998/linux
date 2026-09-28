// SPDX-License-Identifier: GPL-2.0
/* Verify that a P3S exec copies large argv and environment strings. */
#define _GNU_SOURCE

#include "p3s_harness.h"

#define ENV_COUNT 3
#define ENV_SIZE 3072
#define MARKER "--check-exec-env"

static char **test_argv;

TEST(exec_preserves_arguments_across_native_slices)
{
	unsigned int i;

	for (i = 0; i < ENV_COUNT; i++) {
		const char *value = test_argv[i + 2];

		ASSERT_NE(NULL, value);
		ASSERT_EQ(ENV_SIZE, strlen(value));
		EXPECT_EQ('A' + i, value[0]);
		EXPECT_EQ('A' + i, value[ENV_SIZE - 1]);
	}
}

TEST(exec_preserves_environment_across_native_slices)
{
	char name[32];
	unsigned int i;

	for (i = 0; i < ENV_COUNT; i++) {
		const char *value;

		snprintf(name, sizeof(name), "P3S_EXEC_ENV_%u", i);
		value = getenv(name);
		ASSERT_NE(NULL, value);
		ASSERT_EQ(ENV_SIZE, strlen(value));
		EXPECT_EQ('A' + i, value[0]);
		EXPECT_EQ('A' + i, value[ENV_SIZE - 1]);
	}
}

int main(int argc, char **argv)
{
	char name[32];
	char *values[ENV_COUNT];
	unsigned int i;

	p3s_run_mode(argc, argv, NULL);
	if (argc == 1)
		p3s_require_native_compat();

	if (argc == ENV_COUNT + 2 && !strcmp(argv[1], MARKER)) {
		p3s_require_compat();
		test_argv = argv;
		return p3s_harness_run();
	}
	if (argc != 1)
		return EXIT_FAILURE;

	for (i = 0; i < ENV_COUNT; i++) {
		values[i] = malloc(ENV_SIZE + 1);
		if (!values[i])
			ksft_exit_fail_msg("malloc failed\n");
		memset(values[i], 'A' + i, ENV_SIZE);
		values[i][ENV_SIZE] = '\0';
		snprintf(name, sizeof(name), "P3S_EXEC_ENV_%u", i);
		if (setenv(name, values[i], 1))
			ksft_exit_fail_msg("setenv %s failed: %s\n", name,
					   strerror(errno));
	}
	exec_compat(argv[0], MARKER, values[0], values[1], values[2], NULL);
}
