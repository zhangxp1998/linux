// SPDX-License-Identifier: GPL-2.0

#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <sound/pcm.h>

#define TEST_DMA_BYTES SZ_16K

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

static struct miscdevice test_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "snd_pcm_mmap_ppps",
	.fops = &test_fops,
	.mode = 0600,
};

static int __init test_init(void)
{
	test_runtime.state = SNDRV_PCM_STATE_SETUP;
	test_runtime.access = SNDRV_PCM_ACCESS_MMAP_INTERLEAVED;
	test_runtime.info = SNDRV_PCM_INFO_MMAP;
	test_runtime.dma_bytes = TEST_DMA_BYTES;
	test_substream.stream = SNDRV_PCM_STREAM_PLAYBACK;
	test_substream.ops = &test_pcm_ops;
	test_substream.runtime = &test_runtime;
	atomic_set(&test_substream.mmap_count, 0);
	return misc_register(&test_device);
}

static void __exit test_exit(void)
{
	misc_deregister(&test_device);
}

module_init(test_init);
module_exit(test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ALSA PCM mmap PPPS regression fixture");
