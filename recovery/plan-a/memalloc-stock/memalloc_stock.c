// SPDX-License-Identifier: GPL-2.0
/*
 * Stock-compatible linear buffer allocator for stock libmfc.so on the 4.19
 * Plan A kernel.
 *
 * Stock libmfc maps decoder buffers through /dev/mem without O_SYNC and never
 * cleans the CPU cache for them (its DWLCleanCache has no callers). That is
 * only correct when the buffers lie outside kernel RAM, where /dev/mem maps
 * them uncached: stock boots with mem=180M and keeps its memalloc pool at
 * physical 0x0BC00000, 65 MiB (static device mapping 0xCBC00000 in the stock
 * kernel). The SDK allocator uses CMA inside kernel RAM, where the same
 * mapping is cacheable. This module serves stock's pool with stock's ioctl
 * numbers and argument layout:
 *
 *   0xc0046b01  MEMALLOC_IOCXGETBUFFER  {u32 busAddress (out); u32 size (in)}
 *   0x40046b02  MEMALLOC_IOCSFREEBUFFER u32 busAddress
 *   0x40046b06  accepted, no effect
 *
 * The RAM-buffer ioctls 4 and 5 (used by libarkcmn) are not implemented.
 * Load refuses a pool that overlaps kernel RAM. Buffers are zeroed when
 * allocated and freed when their file is closed.
 */
#include <linux/bitmap.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

static unsigned long base = 0x0bc00000;
module_param(base, ulong, 0444);
MODULE_PARM_DESC(base, "physical pool base (stock: 0x0bc00000)");
static unsigned long size = 0x04100000;
module_param(size, ulong, 0444);
MODULE_PARM_DESC(size, "pool size in bytes (stock: 0x04100000)");

#define MA_GET  0xc0046b01
#define MA_FREE 0x40046b02
#define MA_SYNC 0x40046b06

struct chunk {
	struct list_head node;
	struct file *owner;
	unsigned long first;	/* page index in the pool */
	unsigned long pages;
};

static DEFINE_MUTEX(lock);
static LIST_HEAD(chunks);
static unsigned long *map;
static unsigned long npages;
static int major;
static struct resource *region;

static void free_chunk(struct chunk *c)
{
	bitmap_clear(map, c->first, c->pages);
	list_del(&c->node);
	kfree(c);
}

static long ma_get(struct file *filp, u32 __user *arg)
{
	u32 req[2];	/* busAddress, size */
	unsigned long pages, first;
	struct chunk *c;
	void __iomem *va;

	if (copy_from_user(req, arg, sizeof(req)))
		return -EFAULT;
	pages = PAGE_ALIGN((unsigned long)req[1]) >> PAGE_SHIFT;
	req[0] = 0;
	if (!pages)
		goto out;
	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	mutex_lock(&lock);
	first = bitmap_find_next_zero_area(map, npages, 0, pages, 0);
	if (first >= npages) {
		mutex_unlock(&lock);
		kfree(c);
		pr_warn("memalloc-stock: no space for %lu pages\n", pages);
		goto out;
	}
	bitmap_set(map, first, pages);
	c->owner = filp;
	c->first = first;
	c->pages = pages;
	list_add(&c->node, &chunks);
	mutex_unlock(&lock);
	va = ioremap(base + (first << PAGE_SHIFT), pages << PAGE_SHIFT);
	if (va) {
		memset_io(va, 0, pages << PAGE_SHIFT);
		iounmap(va);
	}
	req[0] = base + (first << PAGE_SHIFT);
out:
	if (copy_to_user(arg, req, sizeof(req)))
		return -EFAULT;
	return req[0] ? 0 : -ENOMEM;
}

static long ma_free(u32 __user *arg)
{
	struct chunk *c;
	u32 bus;
	long ret = -EINVAL;

	if (get_user(bus, arg))
		return -EFAULT;
	mutex_lock(&lock);
	list_for_each_entry(c, &chunks, node) {
		if (base + (c->first << PAGE_SHIFT) == bus) {
			free_chunk(c);
			ret = 0;
			break;
		}
	}
	mutex_unlock(&lock);
	return ret;
}

static long ma_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case MA_GET:
		return ma_get(filp, (u32 __user *)arg);
	case MA_FREE:
		return ma_free((u32 __user *)arg);
	case MA_SYNC:
		return 0;
	default:
		return -ENOTTY;
	}
}

static int ma_release(struct inode *inode, struct file *filp)
{
	struct chunk *c, *n;

	mutex_lock(&lock);
	list_for_each_entry_safe(c, n, &chunks, node)
		if (c->owner == filp)
			free_chunk(c);
	mutex_unlock(&lock);
	return 0;
}

static const struct file_operations ma_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = ma_ioctl,
	.release = ma_release,
};

static int __init ma_init(void)
{
	unsigned long pfn;

	if (!size || (base | size) & ~PAGE_MASK)
		return -EINVAL;
	for (pfn = PFN_DOWN(base); pfn < PFN_DOWN(base + size); pfn++)
		if (pfn_valid(pfn)) {
			pr_err("memalloc-stock: pool 0x%lx+0x%lx overlaps kernel RAM\n", base, size);
			return -EBUSY;
		}
	region = request_mem_region(base, size, "memalloc-stock");
	if (!region)
		return -EBUSY;
	npages = size >> PAGE_SHIFT;
	map = kcalloc(BITS_TO_LONGS(npages), sizeof(long), GFP_KERNEL);
	if (!map) {
		release_mem_region(base, size);
		return -ENOMEM;
	}
	major = register_chrdev(0, "memalloc_stock", &ma_fops);
	if (major < 0) {
		kfree(map);
		release_mem_region(base, size);
		return major;
	}
	pr_info("memalloc-stock: pool 0x%08lx+0x%lx (%lu pages), major %d\n", base, size, npages, major);
	return 0;
}

static void __exit ma_exit(void)
{
	struct chunk *c, *n;

	unregister_chrdev(major, "memalloc_stock");
	list_for_each_entry_safe(c, n, &chunks, node)
		free_chunk(c);
	kfree(map);
	release_mem_region(base, size);
}

module_init(ma_init);
module_exit(ma_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Stock-layout linear buffer pool for stock libmfc (AUTOTOP Plan A)");
