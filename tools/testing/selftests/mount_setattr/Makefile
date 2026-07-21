# SPDX-License-Identifier: GPL-2.0
# Makefile for mount selftests.
CFLAGS = -g $(KHDR_INCLUDES) -Wall -O2 -pthread

LOCAL_HDRS += ../filesystems/wrappers.h

TEST_GEN_PROGS := mount_setattr_test mount_setattr_size_ppps

include ../lib.mk
