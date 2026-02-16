// SPDX-License-Identifier: GPL-2.0-only
/*
 *  WonderMedia WM8505 Frame Buffer device driver
 *
 *  Copyright (C) 2010 Ed Spiridonov <edo.rus@gmail.com>
 *    Based on vt8500lcdfb.c
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fb.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/wait.h>
#include <video/of_display_timing.h>

#include "wm8505fb_regs.h"
#include "wmt_ge_rops.h"

#define DRIVER_NAME "wm8505-fb"

#define to_wm8505fb_info(__info) container_of(__info, \
						struct wm8505fb_info, fb)
struct wm8505fb_info {
	struct fb_info		fb;
	void __iomem		*regbase;
	unsigned int		contrast;
	wait_queue_head_t	vsync_wait;
	unsigned int		vsync_count;	/* incremented by ISR */
};

static char *mode_option;
module_param(mode_option, charp, 0);
MODULE_PARM_DESC(mode_option, "Default video mode ('800x480-16@60')");

static int vram_size_mb = 8;
module_param(vram_size_mb, int, 0);
MODULE_PARM_DESC(vram_size_mb, "VRAM size in MiB (default=8)");

static int wm8505fb_init_hw(struct fb_info *info);
static int wm8505fb_init_hw(struct fb_info *info)
{
	struct wm8505fb_info *fbi = to_wm8505fb_info(info);

	int i;

	/* I know the purpose only of few registers, so clear unknown */
	for (i = 0; i < 0x200; i += 4)
		writel(0, fbi->regbase + i);

	/* Set frame buffer address */
	writel(fbi->fb.fix.smem_start, fbi->regbase + WMT_GOVR_FBADDR);
	writel(fbi->fb.fix.smem_start, fbi->regbase + WMT_GOVR_FBADDR1);

	/*
	 * Set in-memory picture format to RGB
	 * 0x31C sets the correct color mode (RGB565) for WM8650
	 * Bit 8+9 (0x300) are ignored on WM8505 as reserved
	 */
	writel(0x31c,		       fbi->regbase + WMT_GOVR_COLORSPACE);
	writel(1,		       fbi->regbase + WMT_GOVR_COLORSPACE1);

	/* Virtual buffer size */
	writel(info->var.xres,	       fbi->regbase + WMT_GOVR_XRES);
	writel(info->var.xres_virtual, fbi->regbase + WMT_GOVR_XRES_VIRTUAL);

	/* black magic ;) */
	writel(0xf,		       fbi->regbase + WMT_GOVR_FHI);
	writel(4,		       fbi->regbase + WMT_GOVR_DVO_SET);
	writel(1,		       fbi->regbase + WMT_GOVR_MIF_ENABLE);
	writel(1,		       fbi->regbase + WMT_GOVR_REG_UPDATE);

	return 0;
}

static int wm8505fb_set_timing(struct fb_info *info)
{
	struct wm8505fb_info *fbi = to_wm8505fb_info(info);

	int h_start = info->var.left_margin;
	int h_end = h_start + info->var.xres;
	int h_all = h_end + info->var.right_margin;
	int h_sync = info->var.hsync_len;

	int v_start = info->var.upper_margin;
	int v_end = v_start + info->var.yres;
	int v_all = v_end + info->var.lower_margin;
	int v_sync = info->var.vsync_len;

	writel(0, fbi->regbase + WMT_GOVR_TG);

	writel(h_start, fbi->regbase + WMT_GOVR_TIMING_H_START);
	writel(h_end,   fbi->regbase + WMT_GOVR_TIMING_H_END);
	writel(h_all,   fbi->regbase + WMT_GOVR_TIMING_H_ALL);
	writel(h_sync,  fbi->regbase + WMT_GOVR_TIMING_H_SYNC);

	writel(v_start, fbi->regbase + WMT_GOVR_TIMING_V_START);
	writel(v_end,   fbi->regbase + WMT_GOVR_TIMING_V_END);
	writel(v_all,   fbi->regbase + WMT_GOVR_TIMING_V_ALL);
	writel(v_sync,  fbi->regbase + WMT_GOVR_TIMING_V_SYNC);

	writel(1, fbi->regbase + WMT_GOVR_TG);

	return 0;
}


static int wm8505fb_set_par(struct fb_info *info)
{
	struct wm8505fb_info *fbi = to_wm8505fb_info(info);

	if (!fbi)
		return -EINVAL;

	if (info->var.bits_per_pixel == 32) {
		info->var.red.offset = 16;
		info->var.red.length = 8;
		info->var.red.msb_right = 0;
		info->var.green.offset = 8;
		info->var.green.length = 8;
		info->var.green.msb_right = 0;
		info->var.blue.offset = 0;
		info->var.blue.length = 8;
		info->var.blue.msb_right = 0;
		info->fix.visual = FB_VISUAL_TRUECOLOR;
		info->fix.line_length = info->var.xres_virtual << 2;
	} else if (info->var.bits_per_pixel == 16) {
		info->var.red.offset = 11;
		info->var.red.length = 5;
		info->var.red.msb_right = 0;
		info->var.green.offset = 5;
		info->var.green.length = 6;
		info->var.green.msb_right = 0;
		info->var.blue.offset = 0;
		info->var.blue.length = 5;
		info->var.blue.msb_right = 0;
		info->fix.visual = FB_VISUAL_TRUECOLOR;
		info->fix.line_length = info->var.xres_virtual << 1;
	}

	wm8505fb_set_timing(info);

	writel(fbi->contrast<<16 | fbi->contrast<<8 | fbi->contrast,
		fbi->regbase + WMT_GOVR_CONTRAST);

	return 0;
}

static ssize_t contrast_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct fb_info *info = dev_get_drvdata(dev);
	struct wm8505fb_info *fbi = to_wm8505fb_info(info);

	return sprintf(buf, "%u\n", fbi->contrast);
}

static ssize_t contrast_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct fb_info *info = dev_get_drvdata(dev);
	struct wm8505fb_info *fbi = to_wm8505fb_info(info);
	unsigned long tmp;

	if (kstrtoul(buf, 10, &tmp) || (tmp > 0xff))
		return -EINVAL;
	fbi->contrast = tmp;

	wm8505fb_set_par(info);

	return count;
}

static DEVICE_ATTR_RW(contrast);

static struct attribute *wm8505fb_attrs[] = {
	&dev_attr_contrast.attr,
	NULL,
};
ATTRIBUTE_GROUPS(wm8505fb);

static inline u_int chan_to_field(u_int chan, struct fb_bitfield *bf)
{
	chan &= 0xffff;
	chan >>= 16 - bf->length;
	return chan << bf->offset;
}

static int wm8505fb_setcolreg(unsigned regno, unsigned red, unsigned green,
			   unsigned blue, unsigned transp,
			   struct fb_info *info) {
	struct wm8505fb_info *fbi = to_wm8505fb_info(info);
	int ret = 1;
	unsigned int val;
	if (regno >= 256)
		return -EINVAL;

	if (info->var.grayscale)
		red = green = blue =
			(19595 * red + 38470 * green + 7471 * blue) >> 16;

	switch (fbi->fb.fix.visual) {
	case FB_VISUAL_TRUECOLOR:
		if (regno < 16) {
			u32 *pal = info->pseudo_palette;

			val  = chan_to_field(red, &fbi->fb.var.red);
			val |= chan_to_field(green, &fbi->fb.var.green);
			val |= chan_to_field(blue, &fbi->fb.var.blue);

			pal[regno] = val;
			ret = 0;
		}
		break;
	}

	return ret;
}

static int wm8505fb_pan_display(struct fb_var_screeninfo *var,
				struct fb_info *info)
{
	struct wm8505fb_info *fbi = to_wm8505fb_info(info);

	writel(var->xoffset, fbi->regbase + WMT_GOVR_XPAN);
	writel(var->yoffset, fbi->regbase + WMT_GOVR_YPAN);
	return 0;
}

static irqreturn_t wm8505fb_irq(int irq, void *data)
{
	struct wm8505fb_info *fbi = data;
	u32 status = readl(fbi->regbase + WMT_GOVR_INT);

	if (status & GOVRH_INT_MEM) {
		/*
		 * Clear the MEM status bit (W1C) while preserving the
		 * enable bits in the lower half of the register.
		 *
		 * The legacy driver used 16-bit writes to the upper half
		 * (govrh_clean_int_status: vppif_reg16_out(REG_GOVRH_INT+0x2, 0x2))
		 * to avoid clobbering enable bits. We achieve the same by
		 * masking: keep enables (bits 0-1), set the W1C status bit.
		 * Writing 0 to other W1C bits is a no-op.
		 */
		writel((status & GOVRH_INT_ENABLE_MASK) | GOVRH_INT_MEM,
		       fbi->regbase + WMT_GOVR_INT);

		fbi->vsync_count++;
		wake_up_interruptible(&fbi->vsync_wait);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static int wm8505fb_wait_for_vsync(struct fb_info *info)
{
	struct wm8505fb_info *fbi = to_wm8505fb_info(info);
	unsigned int count;
	u32 reg;
	int ret;

	/*
	 * VSync wait implementation — matches the legacy driver approach:
	 *
	 * Legacy ge_main.c FBIO_WAITFORVSYNC called vpp_wait_vsync(),
	 * which used interrupt-driven semaphores on VPP_INT_GOVRH_VBIS.
	 *
	 * Legacy ge_accel.c wait_vsync() polled VPP_INTSTS (0xd8050f04)
	 * bit 2 (GOVW_VBIE) with msleep_interruptible(10).
	 *
	 * Our implementation uses the GOVRH local interrupt register (0x38)
	 * with wait_event and a software counter to avoid the race condition
	 * where the ISR clears the hardware status bit before wait_event
	 * can observe it.
	 *
	 * Register layout of GOVRH_INT (offset 0x38):
	 *   Bits 0-1:   Enable (normal R/W)
	 *   Bits 16-17: Status (write-1-to-clear)
	 *
	 * The legacy driver used 16-bit writes to upper/lower halves to
	 * avoid clobbering enable vs status fields. We use read-modify-write
	 * with masking instead.
	 */

	/* Snapshot the current vsync count before enabling the interrupt */
	count = fbi->vsync_count;

	/*
	 * Clear any pending MEM status, then enable MEM interrupt.
	 * Use a single write: preserve nothing (interrupts were disabled),
	 * set enable bit + W1C status bit to clear any stale status.
	 */
	writel(GOVRH_INT_MEM_ENABLE | GOVRH_INT_MEM,
	       fbi->regbase + WMT_GOVR_INT);

	/*
	 * Wait until the ISR increments vsync_count, meaning a frame
	 * complete (VSync) event occurred. Timeout after 100ms (~6 frames
	 * at 60Hz — generous to avoid false negatives).
	 */
	ret = wait_event_interruptible_timeout(fbi->vsync_wait,
					       fbi->vsync_count != count,
					       HZ / 10);

	/*
	 * Disable the interrupt (one-shot). Read-modify-write to keep
	 * other bits undisturbed. Clear enables, don't touch status (W1C).
	 */
	reg = readl(fbi->regbase + WMT_GOVR_INT);
	writel(reg & ~GOVRH_INT_ENABLE_MASK, fbi->regbase + WMT_GOVR_INT);

	if (ret < 0)
		return ret;
	if (ret == 0)
		return -ETIMEDOUT;

	return 0;
}

static int wm8505fb_ioctl(struct fb_info *info, unsigned int cmd,
			  unsigned long arg)
{
	unsigned int cmd_val;

	switch (cmd) {

	/* ---- Sync ---- */
	case FBIO_WAITFORVSYNC:
		return wm8505fb_wait_for_vsync(info);

	case GEIO_WAIT_SYNC:
		return wmt_ge_sync(info);

	/* ---- Transform ---- */
	case GEIO_ROTATE:
		if (copy_from_user(&cmd_val, (void __user *)arg,
				   sizeof(cmd_val)))
			return -EFAULT;
		return wmt_ge_rotate(info, cmd_val);

	case GEIO_MIRROR:
		if (copy_from_user(&cmd_val, (void __user *)arg,
				   sizeof(cmd_val)))
			return -EFAULT;
		return wmt_ge_mirror(info, cmd_val);

	/* ---- AMX layer alpha & enable ---- */
	case GEIO_ALPHA_BLEND:
		if (copy_from_user(&cmd_val, (void __user *)arg,
				   sizeof(cmd_val)))
			return -EFAULT;
		return wmt_ge_amx_set_alpha(info, cmd_val);

	case GEIOSET_AMX_EN:
		if (copy_from_user(&cmd_val, (void __user *)arg,
				   sizeof(cmd_val)))
			return -EFAULT;
		return wmt_ge_amx_set_en(info, cmd_val);

	/* ---- Per-blit alpha ---- */
	case GEIO_ROP_ALPHA:
		if (copy_from_user(&cmd_val, (void __user *)arg,
				   sizeof(cmd_val)))
			return -EFAULT;
		return wmt_ge_rop_alpha_blend(info, cmd_val, 0);

	/* ---- Color keys ---- */
	case GEIOSET_COLORKEY:
		if (copy_from_user(&cmd_val, (void __user *)arg,
				   sizeof(cmd_val)))
			return -EFAULT;
		return wmt_ge_set_colorkey(info, cmd_val);

	case GEIO_SET_SRC_CK:
		if (copy_from_user(&cmd_val, (void __user *)arg,
				   sizeof(cmd_val)))
			return -EFAULT;
		return wmt_ge_set_src_colorkey(info, cmd_val);

	case GEIO_SET_DST_CK:
		if (copy_from_user(&cmd_val, (void __user *)arg,
				   sizeof(cmd_val)))
			return -EFAULT;
		return wmt_ge_set_dst_colorkey(info, cmd_val);

	/* ---- CSC (Color Space Conversion) ---- */
	case GEIO_SET_CSC:
		if (copy_from_user(&cmd_val, (void __user *)arg,
				   sizeof(cmd_val)))
			return -EFAULT;
		return wmt_ge_set_csc(info, cmd_val);

	/* ---- GE engine delay ---- */
	case GEIO_SET_DELAY:
		if (copy_from_user(&cmd_val, (void __user *)arg,
				   sizeof(cmd_val)))
			return -EFAULT;
		return wmt_ge_set_delay(info, cmd_val);

	/* ---- Chip ID (WM8650 = 0x3465) ---- */
	case GEIOGET_CHIP_ID:
	{
		/*
		 * Legacy BSP used PMC register 0xd8120000 to read chip ID.
		 * For our device-tree based driver, return the known
		 * WM8650 chip ID (0x3465).
		 */
		unsigned int chip_id = 0x3465;

		if (copy_to_user((void __user *)arg, &chip_id,
				 sizeof(chip_id)))
			return -EFAULT;
		return 0;
	}

	/* ---- Stop boot logo (legacy compat - no-op) ---- */
	case GEIO_STOP_LOGO:
		return 0;

	/* ---- Fill rect (userspace IOCTL with raw RGB color) ---- */
	case GEIO_FILLRECT:
	{
		struct ge_fillrect_user rect_user;
		struct fb_fillrect rect_kernel;

		if (copy_from_user(&rect_user, (void __user *)arg,
				   sizeof(rect_user)))
			return -EFAULT;

		rect_kernel.dx = rect_user.dx;
		rect_kernel.dy = rect_user.dy;
		rect_kernel.width = rect_user.width;
		rect_kernel.height = rect_user.height;

		/*
		 * Use direct RGB backend because rect_user.color is a
		 * raw pixel value, not a palette index. The standard
		 * wmt_ge_fillrect expects an index for TrueColor visuals
		 * (fbdev standard), which causes wrong colors from IOCTL.
		 */
		rect_kernel.color = rect_user.color;
		rect_kernel.rop = rect_user.rop;

		wmt_ge_fillrect_rgb(info, &rect_kernel, rect_user.color);
		return 0;
	}

	/* ---- Copy area ---- */
	case GEIO_COPYAREA:
	{
		struct ge_copyarea_user area_user;
		struct fb_copyarea area_kernel;

		if (copy_from_user(&area_user, (void __user *)arg,
				   sizeof(area_user)))
			return -EFAULT;

		area_kernel.dx = area_user.dx;
		area_kernel.dy = area_user.dy;
		area_kernel.sx = area_user.sx;
		area_kernel.sy = area_user.sy;
		area_kernel.width = area_user.width;
		area_kernel.height = area_user.height;

		wmt_ge_copyarea(info, &area_kernel);
		return 0;
	}

	/* ---- Line drawing ---- */
	case GEIO_LINE:
	{
		struct ge_line_user line;

		if (copy_from_user(&line, (void __user *)arg, sizeof(line)))
			return -EFAULT;

		return wmt_ge_draw_line(info, &line);
	}

	/* ---- Bezier curve ---- */
	case GEIO_BEZIER:
	{
		struct ge_bezier_user bezier;

		if (copy_from_user(&bezier, (void __user *)arg,
				   sizeof(bezier)))
			return -EFAULT;

		return wmt_ge_draw_bezier(info, &bezier);
	}

	/* ---- General blit with alpha/colorkey ---- */
	case GEIO_BLIT:
	{
		struct ge_blit_user blit;

		if (copy_from_user(&blit, (void __user *)arg, sizeof(blit)))
			return -EFAULT;

		return wmt_ge_blit(info, &blit);
	}

	/* ---- Lock (legacy compat - serialize access, no-op here) ---- */
	case GEIO_LOCK:
		return 0;

	default:
		return -ENOTTY;
	}
}

static int wm8505fb_blank(int blank, struct fb_info *info)
{
	struct wm8505fb_info *fbi = to_wm8505fb_info(info);

	switch (blank) {
	case FB_BLANK_UNBLANK:
		wm8505fb_set_timing(info);
		break;
	default:
		writel(0,  fbi->regbase + WMT_GOVR_TIMING_V_SYNC);
		break;
	}

	return 0;
}

static const struct fb_ops wm8505fb_ops = {
	.owner		= THIS_MODULE,
	__FB_DEFAULT_DMAMEM_OPS_RDWR,
	.fb_set_par	= wm8505fb_set_par,
	.fb_setcolreg	= wm8505fb_setcolreg,
	.fb_fillrect	= wmt_ge_fillrect,
	.fb_copyarea	= wmt_ge_copyarea,
	.fb_imageblit	= sys_imageblit,
	.fb_sync	= wmt_ge_sync,
	.fb_ioctl	= wm8505fb_ioctl,
	.fb_pan_display	= wm8505fb_pan_display,
	.fb_blank	= wm8505fb_blank,
	__FB_DEFAULT_IOMEM_OPS_MMAP,
};

static int wm8505fb_probe(struct platform_device *pdev)
{
	struct wm8505fb_info	*fbi;
	struct display_timings *disp_timing;
	void			*addr;
	int ret, irq;

	struct fb_videomode	mode;
	u32			bpp;
	dma_addr_t fb_mem_phys;
	unsigned long fb_mem_len;
	void *fb_mem_virt;

	fbi = devm_kzalloc(&pdev->dev, sizeof(struct wm8505fb_info) +
			sizeof(u32) * 16, GFP_KERNEL);
	if (!fbi)
		return -ENOMEM;

	strcpy(fbi->fb.fix.id, DRIVER_NAME);

	fbi->fb.fix.type	= FB_TYPE_PACKED_PIXELS;
	fbi->fb.fix.xpanstep	= 1;
	fbi->fb.fix.ypanstep	= 1;
	fbi->fb.fix.ywrapstep	= 0;
	fbi->fb.fix.accel	= FB_ACCEL_NONE;

	fbi->fb.fbops		= &wm8505fb_ops;
	fbi->fb.flags		= FBINFO_HWACCEL_COPYAREA
				| FBINFO_HWACCEL_FILLRECT
				| FBINFO_HWACCEL_XPAN
				| FBINFO_HWACCEL_YPAN
				| FBINFO_VIRTFB
				| FBINFO_PARTIAL_PAN_OK;
	fbi->fb.node		= -1;

	addr = fbi;
	addr = addr + sizeof(struct wm8505fb_info);
	fbi->fb.pseudo_palette	= addr;

	fbi->regbase = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(fbi->regbase))
		return PTR_ERR(fbi->regbase);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "no irq resource found\n");
		return irq;
	}

	init_waitqueue_head(&fbi->vsync_wait);

	ret = devm_request_irq(&pdev->dev, irq, wm8505fb_irq, 0,
			       DRIVER_NAME, fbi);
	if (ret) {
		dev_err(&pdev->dev, "failed to request irq\n");
		return ret;
	}

	/* Disable all interrupts initially */
	writel(0, fbi->regbase + WMT_GOVR_INT);

	disp_timing = of_get_display_timings(pdev->dev.of_node);
	if (!disp_timing)
		return -EINVAL;

	ret = of_get_fb_videomode(pdev->dev.of_node, &mode, OF_USE_NATIVE_MODE);
	if (ret)
		return ret;

	ret = of_property_read_u32(pdev->dev.of_node, "bits-per-pixel", &bpp);
	if (ret)
		return ret;

	fb_videomode_to_var(&fbi->fb.var, &mode);

	fbi->fb.var.nonstd		= 0;
	fbi->fb.var.activate		= FB_ACTIVATE_NOW;

	fbi->fb.var.height		= -1;
	fbi->fb.var.width		= -1;

	/* Allocate VRAM (using fixed size from module param or default) */
	/* Legacy drivers used large buffers (e.g. 8MB) to allow offscreen surfaces for GE */
	fb_mem_len = vram_size_mb * 1024 * 1024;
	
	/* Ensure we have at least enough for the mode */
	if (fb_mem_len < mode.xres * mode.yres * 4 * 2) /* Safety margin */
		fb_mem_len = mode.xres * mode.yres * 4 * 2;

	/* Ideally use dma_alloc_wc for write-combining if arch supports it, 
	   but dmam_alloc_coherent is safer/standard for this platform bus context */
	fb_mem_virt = dmam_alloc_coherent(&pdev->dev, fb_mem_len, &fb_mem_phys,
				GFP_KERNEL);
	if (!fb_mem_virt) {
		pr_err("%s: Failed to allocate framebuffer\n", __func__);
		return -ENOMEM;
	}

	fbi->fb.var.xres_virtual	= mode.xres;
	fbi->fb.var.yres_virtual	= mode.yres * 2;
	fbi->fb.var.bits_per_pixel	= bpp;

	fbi->fb.fix.smem_start		= fb_mem_phys;
	fbi->fb.fix.smem_len		= fb_mem_len;
	fbi->fb.screen_buffer		= fb_mem_virt;
	fbi->fb.screen_size		= fb_mem_len;

	fbi->contrast = 0x10;
	ret = wm8505fb_set_par(&fbi->fb);
	if (ret) {
		dev_err(&pdev->dev, "Failed to set parameters\n");
		return ret;
	}

	if (fb_alloc_cmap(&fbi->fb.cmap, 256, 0) < 0) {
		dev_err(&pdev->dev, "Failed to allocate color map\n");
		return -ENOMEM;
	}

	wm8505fb_init_hw(&fbi->fb);

	/* Initialize interrupts disabled (one-shot mode) */
	writel(0, fbi->regbase + WMT_GOVR_INT);

	platform_set_drvdata(pdev, fbi);

	ret = register_framebuffer(&fbi->fb);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"Failed to register framebuffer device: %d\n", ret);
		if (fbi->fb.cmap.len)
			fb_dealloc_cmap(&fbi->fb.cmap);
		return ret;
	}

	fb_info(&fbi->fb, "%s frame buffer at 0x%lx-0x%lx\n",
		fbi->fb.fix.id, fbi->fb.fix.smem_start,
		fbi->fb.fix.smem_start + fbi->fb.fix.smem_len - 1);

	return 0;
}

static void wm8505fb_remove(struct platform_device *pdev)
{
	struct wm8505fb_info *fbi = platform_get_drvdata(pdev);

	unregister_framebuffer(&fbi->fb);

	/* Disable interrupts */
	writel(0, fbi->regbase + WMT_GOVR_INT);

	writel(0, fbi->regbase);

	if (fbi->fb.cmap.len)
		fb_dealloc_cmap(&fbi->fb.cmap);
}

static const struct of_device_id wmt_dt_ids[] = {
	{ .compatible = "wm,wm8505-fb", },
	{}
};

static struct platform_driver wm8505fb_driver = {
	.probe		= wm8505fb_probe,
	.remove		= wm8505fb_remove,
	.driver		= {
		.name	= DRIVER_NAME,
		.of_match_table = wmt_dt_ids,
		.dev_groups	= wm8505fb_groups,
	},
};

module_platform_driver(wm8505fb_driver);

MODULE_AUTHOR("Ed Spiridonov <edo.rus@gmail.com>");
MODULE_DESCRIPTION("Framebuffer driver for WMT WM8505");
MODULE_DEVICE_TABLE(of, wmt_dt_ids);
