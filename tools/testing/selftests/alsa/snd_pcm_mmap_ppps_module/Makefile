SND_PCM_MMAP_PPPS_MODULE_DIR := \
	$(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
KDIR ?= $(if $(O),$(O),$(realpath ../../../../..))

obj-m += snd_pcm_mmap_ppps_test.o

all:
	+$(MAKE) -C $(KDIR) M=$(SND_PCM_MMAP_PPPS_MODULE_DIR) modules

clean:
	+$(MAKE) -C $(KDIR) M=$(SND_PCM_MMAP_PPPS_MODULE_DIR) clean
