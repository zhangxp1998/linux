// SPDX-License-Identifier: GPL-2.0

#include <linux/fs.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <sound/pcm.h>

#include "../../ppps/ppps_misc_module.h"

#define TEST_DMA_BYTES (6 * SZ_1K)

static struct snd_pcm_substream test_substream;
static struct snd_pcm_runtime test_runtime;

static struct page *test_pcm_page(struct snd_pcm_substream *substream,
				  unsigned long offset)
{
	return NULL;
}

static const struct snd_pcm_ops test_pcm_ops = {
	.page = test_pcm_page,
};

static int test_mmap(struct file *file, struct vm_area_struct *vma)
{
	return snd_pcm_mmap_data(&test_substream, file, vma);
}

static const struct file_operations test_fops = {
	.owner = THIS_MODULE,
	.mmap = test_mmap,
};

static int test_setup(void)
{
	test_runtime.state = SNDRV_PCM_STATE_SETUP;
	test_runtime.access = SNDRV_PCM_ACCESS_MMAP_INTERLEAVED;
	test_runtime.info = SNDRV_PCM_INFO_MMAP;
	test_runtime.dma_bytes = TEST_DMA_BYTES;
	test_substream.stream = SNDRV_PCM_STREAM_PLAYBACK;
	test_substream.ops = &test_pcm_ops;
	test_substream.runtime = &test_runtime;
	atomic_set(&test_substream.mmap_count, 0);
	return 0;
}

PPPS_MISC_MODULE("snd_pcm_mmap_ppps", &test_fops, 0600,
		 test_setup, ppps_misc_no_teardown,
		 "ALSA PCM mmap PPPS regression fixture");
