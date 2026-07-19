# SPDX-License-Identifier: GPL-2.0

KDIR ?= ../../../../../..

obj-m += vb2_mmap_ppps_test.o

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
