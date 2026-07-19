# SPDX-License-Identifier: GPL-2.0

KDIR ?= ../../../../../..
KSRCDIR := $(if $(wildcard $(KDIR)/source/Makefile),$(KDIR)/source,$(KDIR))
KOUT := $(if $(wildcard $(KDIR)/source/Makefile),O=$(KDIR))

obj-m += vb2_userptr_ppps_test.o

all:
	$(MAKE) -C $(KSRCDIR) $(KOUT) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KSRCDIR) $(KOUT) M=$(CURDIR) clean
