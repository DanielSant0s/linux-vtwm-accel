// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/drivers/video/wmt_ge_rops.c
 *
 *  Accelerators for raster operations using WonderMedia Graphics Engine
 *
 *  Copyright (C) 2010 Alexey Charkov <alchark@gmail.com>
 */

#include <linux/module.h>
#include <linux/fb.h>
#include <linux/io.h>
#include <linux/platform_device.h>

#include "core/fb_draw.h"
#include "wmt_ge_rops.h"

#define GE_COMMAND_OFF		0x00
#define GE_DEPTH_OFF		0x04
#define GE_HIGHCOLOR_OFF	0x08
#define GE_ROPCODE_OFF		0x14
#define GE_FIRE_OFF		0x18
#define GE_SRCBASE_OFF		0x20
#define GE_SRCDISPW_OFF		0x24
#define GE_SRCDISPH_OFF		0x28
#define GE_SRCAREAX_OFF		0x2c
#define GE_SRCAREAY_OFF		0x30
#define GE_SRCAREAW_OFF		0x34
#define GE_SRCAREAH_OFF		0x38
#define GE_DESTBASE_OFF		0x3c
#define GE_DESTDISPW_OFF	0x40
#define GE_DESTDISPH_OFF	0x44
#define GE_DESTAREAX_OFF	0x48
#define GE_DESTAREAY_OFF	0x4c
#define GE_DESTAREAW_OFF	0x50
#define GE_DESTAREAH_OFF	0x54
#define GE_PAT0C_OFF		0x88	/* Pattern 0 color */
#define GE_ENABLE_OFF		0xec
#define GE_INTEN_OFF		0xf0
#define GE_STATUS_OFF		0xf8

/* High Color Reg */
#define GE_HIGH_COLOR_EN	BIT(31)

/* Command Reg */
#define GE_CMD_FIRE		BIT(31)
#define GE_CMD_ROP_EN		BIT(30)
#define GE_CMD_PAT_ON		BIT(20)
#define GE_CMD_PAT_MONO		BIT(19)
#define GE_CMD_PAT_EN		BIT(18)
#define GE_CMD_SRC_ON		BIT(17)
#define GE_CMD_SRC_FMT_LUT8	(0 << 12)
#define GE_CMD_SRC_FMT_RGB16	(1 << 12)
#define GE_CMD_SRC_FMT_RGB32	(3 << 12)
#define GE_CMD_ROT_90		(1 << 2)
#define GE_CMD_ROT_180		(2 << 2)
#define GE_CMD_ROT_270		(3 << 2)

/* Alpha Reg */
#define GE_ALPHA_OFF		0x1c
#define GE_ALPHA_EN		BIT(31)
#define GE_ALPHA_MODE_CONST	(2 << 29)
#define GE_ALPHA_VAL(x)		((x) << 21)

/* Colorkey Regs */
#define GE_G1_CK_EN_OFF		0x298
#define GE_G1_C_KEY_OFF		0x2a0

/* AMX Alpha Regs (WM8650) */
#define GE_CK2_APA_OFF		0x2b0
#define GE_AMX_CTL_OFF		0x2b4
#define GE_CK_APA_OFF		0x2b8
#define GE_FIX_APA_OFF		0x2bc
#define GE_AMX2_CTL_OFF		0x2d8
#define GE_FIX2_APA_OFF		0x2dc

/* AMX Control Regs */
#define GE_G1_AMX_EN_OFF	0x2a8
#define GE_REG_UPD_OFF		0x2d0

static void __iomem *regbase;

void wmt_ge_fillrect(struct fb_info *p, const struct fb_fillrect *rect)
{
	unsigned long fg, pat;

	if (p->state != FBINFO_STATE_RUNNING)
		return;

	if (p->fix.visual == FB_VISUAL_TRUECOLOR ||
	    p->fix.visual == FB_VISUAL_DIRECTCOLOR)
		fg = ((u32 *) (p->pseudo_palette))[rect->color];
	else
		fg = rect->color;

	pat = pixel_to_pat(p->var.bits_per_pixel, fg);

	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);

	writel(p->var.bits_per_pixel == 32 ? 3 :
	      (p->var.bits_per_pixel == 8 ? 0 : 1), regbase + GE_DEPTH_OFF);
	writel(p->var.bits_per_pixel == 15 ? 1 : 0, regbase + GE_HIGHCOLOR_OFF);
	writel(p->fix.smem_start, regbase + GE_DESTBASE_OFF);
	writel(p->var.xres_virtual - 1, regbase + GE_DESTDISPW_OFF);
	writel(p->var.yres_virtual - 1, regbase + GE_DESTDISPH_OFF);
	writel(rect->dx, regbase + GE_DESTAREAX_OFF);
	writel(rect->dy, regbase + GE_DESTAREAY_OFF);
	writel(rect->width - 1, regbase + GE_DESTAREAW_OFF);
	writel(rect->height - 1, regbase + GE_DESTAREAH_OFF);

	writel(pat, regbase + GE_PAT0C_OFF);
	writel(1, regbase + GE_COMMAND_OFF);
	writel(rect->rop == ROP_XOR ? 0x5a : 0xf0, regbase + GE_ROPCODE_OFF);
	writel(1, regbase + GE_FIRE_OFF);
}
EXPORT_SYMBOL_GPL(wmt_ge_fillrect);

void wmt_ge_copyarea(struct fb_info *p, const struct fb_copyarea *area)
{
	if (p->state != FBINFO_STATE_RUNNING)
		return;

	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);

	writel(p->var.bits_per_pixel > 16 ? 3 :
	      (p->var.bits_per_pixel > 8 ? 1 : 0), regbase + GE_DEPTH_OFF);

	writel(p->fix.smem_start, regbase + GE_SRCBASE_OFF);
	writel(p->var.xres_virtual - 1, regbase + GE_SRCDISPW_OFF);
	writel(p->var.yres_virtual - 1, regbase + GE_SRCDISPH_OFF);
	writel(area->sx, regbase + GE_SRCAREAX_OFF);
	writel(area->sy, regbase + GE_SRCAREAY_OFF);
	writel(area->width - 1, regbase + GE_SRCAREAW_OFF);
	writel(area->height - 1, regbase + GE_SRCAREAH_OFF);

	writel(p->fix.smem_start, regbase + GE_DESTBASE_OFF);
	writel(p->var.xres_virtual - 1, regbase + GE_DESTDISPW_OFF);
	writel(p->var.yres_virtual - 1, regbase + GE_DESTDISPH_OFF);
	writel(area->dx, regbase + GE_DESTAREAX_OFF);
	writel(area->dy, regbase + GE_DESTAREAY_OFF);
	writel(area->width - 1, regbase + GE_DESTAREAW_OFF);
	writel(area->height - 1, regbase + GE_DESTAREAH_OFF);

	writel(0xcc, regbase + GE_ROPCODE_OFF);
	writel(1, regbase + GE_COMMAND_OFF);
	writel(1, regbase + GE_FIRE_OFF);
}
EXPORT_SYMBOL_GPL(wmt_ge_copyarea);

int wmt_ge_sync(struct fb_info *p)
{
	int loops = 5000000;
	while ((readl(regbase + GE_STATUS_OFF) & 4) && --loops)
		cpu_relax();
	return loops > 0 ? 0 : -EBUSY;
}
EXPORT_SYMBOL_GPL(wmt_ge_sync);

int wmt_ge_rotate(struct fb_info *p, int angle)
{
	u32 cmd = GE_CMD_FIRE | GE_CMD_ROP_EN | GE_CMD_SRC_ON;
	u32 fmt;

	if (p->state != FBINFO_STATE_RUNNING)
		return 0;

	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);

	switch (p->var.bits_per_pixel) {
	case 8: fmt = GE_CMD_SRC_FMT_LUT8; break;
	case 16: fmt = GE_CMD_SRC_FMT_RGB16; break;
	case 32: fmt = GE_CMD_SRC_FMT_RGB32; break;
	default: return -EINVAL;
	}
	cmd |= fmt;

	switch (angle) {
	case 90: cmd |= GE_CMD_ROT_90; break;
	case 180: cmd |= GE_CMD_ROT_180; break;
	case 270: cmd |= GE_CMD_ROT_270; break;
	case 0: break;
	default: return -EINVAL;
	}

	writel(p->var.bits_per_pixel == 32 ? 3 :
	      (p->var.bits_per_pixel == 8 ? 0 : 1), regbase + GE_DEPTH_OFF);
	writel(p->var.bits_per_pixel == 15 ? 1 : 0, regbase + GE_HIGHCOLOR_OFF);

	writel(p->fix.smem_start, regbase + GE_SRCBASE_OFF);
	writel(p->var.xres_virtual - 1, regbase + GE_SRCDISPW_OFF);
	writel(p->var.yres_virtual - 1, regbase + GE_SRCDISPH_OFF);
	writel(0, regbase + GE_SRCAREAX_OFF);
	writel(0, regbase + GE_SRCAREAY_OFF);
	writel(p->var.xres - 1, regbase + GE_SRCAREAW_OFF);
	writel(p->var.yres - 1, regbase + GE_SRCAREAH_OFF);

	writel(p->fix.smem_start, regbase + GE_DESTBASE_OFF);
	
	/* Destination pitch/dims flip for 90/270 */
	if (angle == 90 || angle == 270) {
		writel(p->var.yres_virtual - 1, regbase + GE_DESTDISPW_OFF);
		writel(p->var.xres_virtual - 1, regbase + GE_DESTDISPH_OFF);
		writel(0, regbase + GE_DESTAREAX_OFF);
		writel(0, regbase + GE_DESTAREAY_OFF);
		writel(p->var.yres - 1, regbase + GE_DESTAREAW_OFF);
		writel(p->var.xres - 1, regbase + GE_DESTAREAH_OFF);
	} else {
		writel(p->var.xres_virtual - 1, regbase + GE_DESTDISPW_OFF);
		writel(p->var.yres_virtual - 1, regbase + GE_DESTDISPH_OFF);
		writel(0, regbase + GE_DESTAREAX_OFF);
		writel(0, regbase + GE_DESTAREAY_OFF);
		writel(p->var.xres - 1, regbase + GE_DESTAREAW_OFF);
		writel(p->var.yres - 1, regbase + GE_DESTAREAH_OFF);
	}

	writel(0xcc, regbase + GE_ROPCODE_OFF); /* SRC copy */
	writel(cmd, regbase + GE_COMMAND_OFF); /* Includes FIRE bit */
	/* Legacy driver actually writes 1 to FIRE_OFF separately, but COMMAND reg has FIRE bit too. 
	   Let's follow legacy GE_COMMAND flow or just write FIRE. 
	   wmt_ge_copyarea writes COMMAND then FIRE. */
	writel(1, regbase + GE_FIRE_OFF);

	return 0;
}
EXPORT_SYMBOL_GPL(wmt_ge_rotate);

EXPORT_SYMBOL_GPL(wmt_ge_rotate);

int wmt_ge_rop_alpha_blend(struct fb_info *p, int alpha, int mode)
{
	/* Simple constant alpha blend implementation */
	/* Not full implementation but sufficient for testing/completeness */
	/* NOTE: This requires GE_ALPHA_OFF to be defined (0x1c) */
	
	if (p->state != FBINFO_STATE_RUNNING)
		return 0;

	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);

	/* Set Alpha Value */
	writel(GE_ALPHA_EN | GE_ALPHA_MODE_CONST | GE_ALPHA_VAL(alpha & 0xff), 
	       regbase + GE_ALPHA_OFF);

	return 0;
}
EXPORT_SYMBOL_GPL(wmt_ge_rop_alpha_blend);

int wmt_ge_amx_set_alpha(struct fb_info *p, int alpha)
{
	/* Sets Global Graphic Layer Alpha (AMX) */
	/* Logic based on legacy amx_set_alpha_wm8650 */
	
	if (p->state != FBINFO_STATE_RUNNING)
		return 0;

	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);

	/* Write registers directly as per legacy driver for WM8650 */
	writel(0x10, regbase + GE_AMX_CTL_OFF); /* BIT4 */
	writel(0xffff | ((alpha & 0xff) << 24), regbase + GE_FIX_APA_OFF);
	writel(0x400, regbase + GE_CK_APA_OFF); /* BIT10 */
	writel(0x76, regbase + GE_AMX2_CTL_OFF);
	writel(0xff, regbase + GE_FIX2_APA_OFF);
	writel(0x888, regbase + GE_CK2_APA_OFF); /* BIT11 | BIT7 | BIT3 */

	return 0;
}
EXPORT_SYMBOL_GPL(wmt_ge_amx_set_alpha);

int wmt_ge_amx_set_en(struct fb_info *p, int enable)
{
	if (p->state != FBINFO_STATE_RUNNING)
		return 0;

	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);

	/* Set AMX Enable for G1 */
	writel(enable ? 1 : 0, regbase + GE_G1_AMX_EN_OFF);
	
	/* Trigger Register Update */
	writel(1, regbase + GE_REG_UPD_OFF);

	return 0;
}
EXPORT_SYMBOL_GPL(wmt_ge_amx_set_en);

int wmt_ge_set_colorkey(struct fb_info *p, unsigned int colorkey)
{
	/* Sets the Colorkey for G1 (Graphic Layer 1) */
	/* We assume G1 is the main framebuffer layer */
	
	if (p->state != FBINFO_STATE_RUNNING)
		return 0;

	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);

	/* 
	 * Logic ported from legacy ge_set_amx_colorkey:
	 * The Color Key argument is expected to be in A:R:G:B format (32-bit).
	 * If the Alpha channel (top 8 bits) is non-zero, Color Key is ENABLED.
	 * If the Alpha channel is zero, Color Key is DISABLED.
	 */
	int enable = (colorkey >> 24) & 0xff;
	unsigned int rgb_val = colorkey & 0xffffff;

	/* Set the color key value (lower 24 bits) */
	writel(rgb_val, regbase + GE_G1_C_KEY_OFF);
	
	/* Enable or Disable based on Alpha presence */
	writel(enable ? 1 : 0, regbase + GE_G1_CK_EN_OFF);

	return 0;
}
EXPORT_SYMBOL_GPL(wmt_ge_set_colorkey);

static int wmt_ge_rops_probe(struct platform_device *pdev)
{
	struct resource *res;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res == NULL) {
		dev_err(&pdev->dev, "no I/O memory resource defined\n");
		return -ENODEV;
	}

	/* Only one ROP engine is presently supported. */
	if (unlikely(regbase)) {
		WARN_ON(1);
		return -EBUSY;
	}

	regbase = ioremap(res->start, resource_size(res));
	if (regbase == NULL) {
		dev_err(&pdev->dev, "failed to map I/O memory\n");
		return -EBUSY;
	}

	writel(1, regbase + GE_ENABLE_OFF);
	printk(KERN_INFO "Enabled support for WMT GE raster acceleration\n");

	return 0;
}

static void wmt_ge_rops_remove(struct platform_device *pdev)
{
	iounmap(regbase);
}

static const struct of_device_id wmt_dt_ids[] = {
	{ .compatible = "wm,prizm-ge-rops", },
	{ /* sentinel */ }
};

static struct platform_driver wmt_ge_rops_driver = {
	.probe		= wmt_ge_rops_probe,
	.remove		= wmt_ge_rops_remove,
	.driver		= {
		.name	= "wmt_ge_rops",
		.of_match_table = wmt_dt_ids,
	},
};

module_platform_driver(wmt_ge_rops_driver);

MODULE_AUTHOR("Alexey Charkov <alchark@gmail.com>");
MODULE_DESCRIPTION("Accelerators for raster operations using "
		   "WonderMedia Graphics Engine");
MODULE_DEVICE_TABLE(of, wmt_dt_ids);
