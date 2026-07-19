// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026 Kelvin Zhang */

#include <test_progs.h>
#include "arena_ppps.skel.h"

void test_arena_ppps(void)
{
	struct arena_ppps *skel;

	skel = arena_ppps__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open_and_load"))
		return;

	ASSERT_EQ(skel->arena->arena_blob[0], 0x31, "arena_blob_first");
	ASSERT_EQ(skel->arena->arena_blob[4999], 0x7a, "arena_blob_last");

	arena_ppps__destroy(skel);
}
