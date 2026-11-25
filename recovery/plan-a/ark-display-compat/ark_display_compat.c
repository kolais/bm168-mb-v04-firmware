// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal /dev/ark_display for stock libarkcmn.so on the 4.19 Plan A kernel.
 *
 * libarkcmn's display path (arkapi_init_fb_display, arkapi_show_fb, used by
 * CarPlay's libScreenStream and the Android Auto sink) queries this stock
 * misc device. Behaviour follows the stock 3.4 kernel's handler (disassembly
 * at 0x802c70f8):
 *
 *   0xc004a01d  get screen info: 5 x u32 {screen_id, width, height, w2, h2}
 *               (w2/h2 = a second size pair, else width/height again)
 *   0x4004a01a  gui_tvout(int): routes the GUI to the TV encoder; accepted
 *               and logged (no TV-out on BM168)
 *   0x0000a01b  sets an internal flag; accepted
 *   0xc004a001  get VDE (colour) config, 0x4004a002 set it: 5 x u32
 *               {layer 0-4, hue, saturation, brightness, contrast}, each
 *               value 0-255 (ark_disp_get/set_vde_cfg at 0x802c99b0 and
 *               0x802c9a14). Layers 0-2 are OSD1-3 (LCDC 0x144, 0x14c,
 *               0x154), 3-4 VIDEO1-2 (0x1d8, 0x134); each register holds
 *               hue << 24 | saturation << 16 | brightness << 8 | contrast.
 *               Launcher's brightness/contrast/saturation settings use
 *               these; without them the GUI kept the SDK default colours.
 *   other       -EINVAL (stock handles more, unused by projection)
 *
 * /proc/display (stock ark_disp_write_proc, 0x802c77c0): "CMD ARG" lines.
 * Only command 24 is implemented: Launcher writes "24 1" once its UI is up
 * ("show osd1"). Here that shows OSD1 and hides OSD2, the layer on which
 * U-Boot left the boot logo (CONTROL bit 8, kept by patch 0009); stock's
 * running state is CONTROL 0x03600081. Other commands are logged.
 *
 * The screen values are module parameters; defaults are BM168's 1280x480
 * panel. Downstream linux-arkmicro's ark_display.c (9e66660) served the same
 * role with the Prado's 800x480 hard-coded and VDE register writes; those
 * are not needed for projection video.
 */
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/proc_fs.h>
#include <linux/module.h>
#include <linux/uaccess.h>

static unsigned int screen_id;
module_param(screen_id, uint, 0444);
static unsigned int width = 1280;
module_param(width, uint, 0444);
static unsigned int height = 480;
module_param(height, uint, 0444);
static unsigned int width2;
module_param(width2, uint, 0444);
MODULE_PARM_DESC(width2, "second size pair (0 = same as width/height)");
static unsigned int height2;
module_param(height2, uint, 0444);

#define ARKDISP_GET_SCREEN_INFO 0xc004a01d
#define ARKDISP_GUI_TVOUT       0x4004a01a
#define ARKDISP_FLAG_1B         0x0000a01b
#define ARKDISP_GET_VDE_CFG     0xc004a001
#define ARKDISP_SET_VDE_CFG     0x4004a002

#define LCDC_BASE 0xe0500000
static void __iomem *lcdc;
static const u16 vde_reg[5] = { 0x144, 0x14c, 0x154, 0x1d8, 0x134 };
static DEFINE_SPINLOCK(vde_lock);

static long ad_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	u32 info[5];
	unsigned long flags;
	int v, i;

	switch (cmd) {
	case ARKDISP_GET_VDE_CFG:
	case ARKDISP_SET_VDE_CFG:
		if (copy_from_user(info, (void __user *)arg, sizeof(info)))
			return -EFAULT;
		if (info[0] > 4)
			return -EINVAL;
		if (cmd == ARKDISP_GET_VDE_CFG) {
			u32 r = readl(lcdc + vde_reg[info[0]]);
			for (i = 0; i < 4; i++)	/* hue, saturation, brightness, contrast */
				info[1 + i] = (r >> (24 - 8 * i)) & 0xff;
			return copy_to_user((void __user *)arg, info, sizeof(info)) ? -EFAULT : 0;
		}
		for (i = 1; i < 5; i++)
			if (info[i] > 255)
				return -EINVAL;
		spin_lock_irqsave(&vde_lock, flags);
		writel(info[1] << 24 | info[2] << 16 | info[3] << 8 | info[4],
		       lcdc + vde_reg[info[0]]);
		spin_unlock_irqrestore(&vde_lock, flags);
		return 0;
	case ARKDISP_GET_SCREEN_INFO:
		info[0] = screen_id;
		info[1] = width;
		info[2] = height;
		info[3] = width2 && height2 ? width2 : width;
		info[4] = width2 && height2 ? height2 : height;
		return copy_to_user((void __user *)arg, info, sizeof(info)) ? -EFAULT : 0;
	case ARKDISP_GUI_TVOUT:
		if (get_user(v, (int __user *)arg))
			return -EFAULT;
		pr_info("ark_display: gui_tvout %d ignored\n", v);
		return 0;
	case ARKDISP_FLAG_1B:
		return 0;
	default:
		pr_debug("ark_display: unhandled ioctl 0x%08x\n", cmd);
		return -EINVAL;
	}
}

#define LCDC_CONTROL	0x04
#define OSD1_EN		(1 << 7)
#define OSD2_EN		(1 << 8)	/* U-Boot's logo layer */

static ssize_t ad_proc_write(struct file *f, const char __user *buf, size_t len, loff_t *off)
{
	char line[32];
	int cmd, arg;
	unsigned long flags;
	u32 v;

	if (len >= sizeof(line))
		return -EINVAL;
	if (copy_from_user(line, buf, len))
		return -EFAULT;
	line[len] = 0;
	if (sscanf(line, "%d %d", &cmd, &arg) != 2) {
		pr_info("ark_display: /proc/display \"%s\" ignored\n", strim(line));
		return len;
	}
	if (cmd == 24) {
		spin_lock_irqsave(&vde_lock, flags);
		v = readl(lcdc + LCDC_CONTROL);
		v = arg ? (v | OSD1_EN) & ~OSD2_EN : v & ~OSD1_EN;
		writel(v, lcdc + LCDC_CONTROL);
		spin_unlock_irqrestore(&vde_lock, flags);
		pr_info("ark_display: show osd1 %d, boot logo layer %s (CONTROL 0x%08x)\n",
			arg, arg ? "hidden" : "kept", v);
	} else {
		pr_info("ark_display: /proc/display command %d %d ignored\n", cmd, arg);
	}
	return len;
}

static const struct file_operations ad_proc_fops = {
	.owner = THIS_MODULE,
	.write = ad_proc_write,
};

static const struct file_operations ad_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = ad_ioctl,
};

static struct miscdevice ad_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ark_display",
	.fops = &ad_fops,
};

static int __init ad_init(void)
{
	pr_info("ark_display: compat, screen %u %ux%u\n", screen_id, width, height);
	/* The LCD driver owns the region; this only touches the VP registers. */
	lcdc = ioremap(LCDC_BASE, 0x400);
	if (!lcdc)
		return -ENOMEM;
	if (!proc_create("display", 0222, NULL, &ad_proc_fops))
		pr_warn("ark_display: /proc/display not created\n");
	return misc_register(&ad_dev);
}

static void __exit ad_exit(void)
{
	misc_deregister(&ad_dev);
	remove_proc_entry("display", NULL);
	iounmap(lcdc);
}

module_init(ad_init);
module_exit(ad_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Stock-compatible /dev/ark_display for libarkcmn (AUTOTOP Plan A)");
