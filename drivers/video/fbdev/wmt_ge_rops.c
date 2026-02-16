// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/drivers/video/wmt_ge_rops.c
 *
 *  Full hardware-accelerated 2D operations using WonderMedia Graphics Engine
 *
 *  Copyright (C) 2010 Alexey Charkov <alchark@gmail.com>
 *
 *  Extended with all legacy GE hardware features from WM8650/WM8505 BSP:
 *    - Fill rectangle, copy area (original)
 *    - Rotation 0/90/180/270 degrees
 *    - Horizontal mirror
 *    - Hardware line drawing with style/thickness
 *    - Hardware Bezier curve drawing
 *    - General blit with per-blit alpha and color key
 *    - Source/Destination color key configuration
 *    - AMX layer alpha compositing (WM8650 variant)
 *    - AMX layer enable/disable
 *    - CSC (Color Space Conversion) coefficient tables
 *    - GE engine timing/delay configuration
 *
 *  Legacy register maps referenced:
 *    ge_regs_8430.h, ge_regs_8435.h, ge_regs_8440.h,
 *    ge_regs_8510.h, ge_regs_8650.h
 *    by Vincent Chen <vincentchen@wondermedia.com.tw>
 */

#include <linux/module.h>
#include <linux/fb.h>
#include <linux/io.h>
#include <linux/platform_device.h>

#include "core/fb_draw.h"
#include "wmt_ge_rops.h"

/*
 * ============================================================
 *  GE Register Offsets (WM8650 / ge_regs_8650.h compatible)
 * ============================================================
 *
 *  Base: mapped via platform device resource (typically 0xd8050400)
 *
 *  The register map below covers ALL known GE hardware registers
 *  from VT8430 through WM8650 variants.
 */

/* --- GE Core Command Registers --- */
#define GE_COMMAND_OFF		0x00	/* GE Command */
#define GE_DEPTH_OFF		0x04	/* Color Depth (0=8bpp,1=16bpp,3=32bpp) */
#define GE_HIGHCOLOR_OFF	0x08	/* Hi-Color Mode (0=565,1=555,2=454) */
#define GE_PAT_TRANS_OFF	0x0c	/* Pattern Transparency Enable */
#define GE_FONT_TRANS_OFF	0x10	/* Font Transparency Enable */
#define GE_ROPCODE_OFF		0x14	/* ROP Code */
#define GE_FIRE_OFF		0x18	/* Fire (write 1 to execute) */
#define GE_ROP_BG_OFF		0x1c	/* ROP Background Code (WM8435+) */

/* --- Source Surface Registers --- */
#define GE_SRCBASE_OFF		0x20	/* Source Base Address */
#define GE_SRCDISPW_OFF		0x24	/* Source Display Width */
#define GE_SRCDISPH_OFF		0x28	/* Source Display Height */
#define GE_SRCAREAX_OFF		0x2c	/* Source Area X Start */
#define GE_SRCAREAY_OFF		0x30	/* Source Area Y Start */
#define GE_SRCAREAW_OFF		0x34	/* Source Area Width */
#define GE_SRCAREAH_OFF		0x38	/* Source Area Height */

/* --- Destination Surface Registers --- */
#define GE_DESTBASE_OFF		0x3c	/* Dest Base Address */
#define GE_DESTDISPW_OFF	0x40	/* Dest Display Width */
#define GE_DESTDISPH_OFF	0x44	/* Dest Display Height */
#define GE_DESTAREAX_OFF	0x48	/* Dest Area X Start */
#define GE_DESTAREAY_OFF	0x4c	/* Dest Area Y Start */
#define GE_DESTAREAW_OFF	0x50	/* Dest Area Width */
#define GE_DESTAREAH_OFF	0x54	/* Dest Area Height */

/* --- Font Color Buffers (for GECMD_TEXT) --- */
#define GE_FONT0_OFF		0x58	/* Font Color 0 */
#define GE_FONT1_OFF		0x5c	/* Font Color 1 */
#define GE_FONT2_OFF		0x60	/* Font Color 2 */
#define GE_FONT3_OFF		0x64	/* Font Color 3 */

/* --- Pattern Buffers (8 x 32-bit) --- */
#define GE_PATBUF0_OFF		0x68	/* Pattern Buffer 0 */
#define GE_PATBUF1_OFF		0x6c	/* Pattern Buffer 1 */
#define GE_PATBUF2_OFF		0x70	/* Pattern Buffer 2 */
#define GE_PATBUF3_OFF		0x74	/* Pattern Buffer 3 */
#define GE_PATBUF4_OFF		0x78	/* Pattern Buffer 4 */
#define GE_PATBUF5_OFF		0x7c	/* Pattern Buffer 5 */
#define GE_PATBUF6_OFF		0x80	/* Pattern Buffer 6 */
#define GE_PATBUF7_OFF		0x84	/* Pattern Buffer 7 */

/* --- Pattern Colors (16 entries) --- */
#define GE_PAT0C_OFF		0x88	/* Pattern 0 Color */
#define GE_PAT1C_OFF		0x8c
#define GE_PAT2C_OFF		0x90
#define GE_PAT3C_OFF		0x94
#define GE_PAT4C_OFF		0x98
#define GE_PAT5C_OFF		0x9c
#define GE_PAT6C_OFF		0xa0
#define GE_PAT7C_OFF		0xa4
#define GE_PAT8C_OFF		0xa8
#define GE_PAT9C_OFF		0xac
#define GE_PAT10C_OFF		0xb0
#define GE_PAT11C_OFF		0xb4
#define GE_PAT12C_OFF		0xb8
#define GE_PAT13C_OFF		0xbc
#define GE_PAT14C_OFF		0xc0
#define GE_PAT15C_OFF		0xc4

/* --- Color Key Registers (per-blit) --- */
#define GE_CK_SEL_OFF		0xc8	/* Color Key Select (BIT2=src, BIT3|BIT1=dst) */
#define GE_SRC_CK_OFF		0xcc	/* Source Color Key value */
#define GE_DST_CK_OFF		0xd0	/* Destination Color Key value */

/* --- Per-Blit Alpha Registers --- */
#define GE_ALPHA_SEL_OFF	0xd4	/* Alpha mode select */
#define GE_BITBLT_ALPHA_OFF	0xd8	/* Per-blit constant alpha value */

/* --- WM8650: Dest Path Enable --- */
#define GE_DES_PATH_EN_OFF	0xdc

/* --- Transform Registers --- */
#define GE_ROTATE_MODE_OFF	0xe0	/* Rotate Mode (0/1/2/3) */
#define GE_MIRROR_MODE_OFF	0xe4	/* Mirror Mode (BIT0=enable) */
#define GE_DELAY_OFF		0xe8	/* GE Start Cycle Delay */

/* --- Engine Control / Status --- */
#define GE_ENABLE_OFF		0xec	/* GE Engine Enable */
#define GE_INTEN_OFF		0xf0	/* Interrupt Enable (BIT8=complete, BIT9=timeout) */
#define GE_INTFLAG_OFF		0xf4	/* Interrupt Flag (write to clear) */
#define GE_STATUS_OFF		0xf8	/* GE Status (BIT2=busy) */
#define GE_SWID_OFF		0xfc	/* Software Identify */

/* --- Line Drawing Registers --- */
#define GE_LN_XSTART_OFF	0x100	/* Line X Start */
#define GE_LN_XEND_OFF		0x104	/* Line X End */
#define GE_LN_YSTART_OFF	0x108	/* Line Y Start */
/* 0x10c reserved */
#define GE_LN_YEND_OFF		0x110	/* Line Y End */
#define GE_LN_TCK_OFF		0x114	/* Line Thickness */

/* --- CSC / DMA control at 0x118 --- */
#define GE_CSC_BYPASS_OFF	0x118	/* CSC Bypass (WM8650) */

/* --- CSC Coefficient C1 --- */
#define GE_C1_COEF_OFF		0x11c

/* --- Line Style Registers --- */
#define GE_LN_STL_TB_OFF	0x120	/* Line Style Table */
#define GE_LN_STL_RTN_OFF	0x124	/* Line Style Return */
#define GE_LN_STL_DATA_OFF	0x128	/* Line Style Data Pattern */
#define GE_LN_STL_APA_OFF	0x12c	/* Line Style Alpha */

/* --- Bezier Curve Registers --- */
#define GE_BC_P1X_OFF		0x130	/* Bezier Control Point 1 X */
#define GE_BC_P1Y_OFF		0x134	/* Bezier Control Point 1 Y */
#define GE_BC_P2X_OFF		0x138	/* Bezier Control Point 2 X */
#define GE_BC_P2Y_OFF		0x13c	/* Bezier Control Point 2 Y */
#define GE_BC_P3X_OFF		0x140	/* Bezier Control Point 3 X */
#define GE_BC_P3Y_OFF		0x144	/* Bezier Control Point 3 Y */
#define GE_BC_COLOR_OFF		0x148	/* Bezier Color */
#define GE_BC_ALPHA_OFF		0x14c	/* Bezier Alpha */
#define GE_BC_DELTA_T_OFF	0x150	/* Bezier Delta-T step */
#define GE_BC_LSTL_OFF		0x154	/* Bezier Line Style */
#define GE_BC_LSTL_RTN_OFF	0x158	/* Bezier Line Style Return */

/* --- CSC Coefficients C2-C8 --- */
#define GE_C2_COEF_OFF		0x15c
#define GE_C3_COEF_OFF		0x160
#define GE_C4_COEF_OFF		0x164
#define GE_C5_COEF_OFF		0x168
#define GE_C6_COEF_OFF		0x16c
#define GE_C7_COEF_OFF		0x170
#define GE_C8_COEF_OFF		0x174

/* --- VQ (Virtual Queue) Registers 0x180-0x19c --- */
/* (Not used - legacy noted VQ is slower than direct register access) */

/* --- ROP4 / Mask Registers (WM8435/WM8440/WM8650) --- */
#define GE_ROP4_EN_OFF		0x1a0	/* ROP4 Enable */
#define GE_ALPHA_PLANE_OFF	0x1a4	/* Alpha Plane Enable */
#define GE_MASK_BADDR_OFF	0x1a8	/* Mask Base Address */
#define GE_MASK_DISPW_OFF	0x1ac	/* Mask Display Width */
#define GE_MASK_DISPH_OFF	0x1b0	/* Mask Display Height */
#define GE_MASK_XSTART_OFF	0x1b4	/* Mask X Start */
#define GE_MASK_YSTART_OFF	0x1b8	/* Mask Y Start */
#define GE_MASK_WIDTH_OFF	0x1bc	/* Mask Width */
#define GE_MASK_HEIGHT_OFF	0x1c0	/* Mask Height */
#define GE_DW_MASK_OFF		0x1c4	/* DW Mask Base Address */
#define GE_ALPHA_WBE_OFF	0x1c8	/* Alpha Plane Write-Back Enable */

/* --- Adaptive Blending (WM8440/WM8650) --- */
#define GE_ADAP_BLEND_OFF	0x1d0	/* Adaptive Blend Enable */
#define GE_SRC_ASEL_OFF		0x1d4	/* Source Alpha Select */
#define GE_SRC_BLEND_OFF	0x1d8	/* Source Blend Alpha */
#define GE_DST_ASEL_OFF		0x1dc	/* Dest Alpha Select */
#define GE_DST_BLEND_OFF	0x1e0	/* Dest Blend Alpha */
#define GE_ADAP_CLAMP_OFF	0x1e4	/* Adaptive Clamping Enable */

/* --- CSC Coefficient C9 + I/J/K --- */
#define GE_C9_COEF_OFF		0x1f0
#define GE_COEF_I_OFF		0x1f4
#define GE_COEF_J_OFF		0x1f8
#define GE_COEF_K_OFF		0x1fc

/* --- AMX Layer Registers (G1 and G2) --- */
#define GE_G1_CD_OFF		0x200	/* G1 Color Depth */
#define GE_G2_CD_OFF		0x204	/* G2 Color Depth */
#define GE_G1_FG_ADDR_OFF	0x210	/* G1 Foreground Address */
#define GE_G1_BG_ADDR_OFF	0x214	/* G1 Background Address */
#define GE_G1_FB_SEL_OFF	0x218	/* G1 Framebuffer Select */
#define GE_G2_FG_ADDR_OFF	0x21c	/* G2 Foreground Address */
#define GE_G2_BG_ADDR_OFF	0x220	/* G2 Background Address */
#define GE_G2_FB_SEL_OFF	0x224	/* G2 Framebuffer Select */
#define GE_G1_XSTART_OFF	0x230	/* G1 X Start */
#define GE_G1_XEND_OFF		0x234	/* G1 X End */
#define GE_G1_YSTART_OFF	0x238	/* G1 Y Start */
#define GE_G1_YEND_OFF		0x23c	/* G1 Y End */
#define GE_G2_XSTART_OFF	0x240	/* G2 X Start */
#define GE_G2_XEND_OFF		0x244	/* G2 X End */
#define GE_G2_YSTART_OFF	0x248	/* G2 Y Start */
#define GE_G2_YEND_OFF		0x24c	/* G2 Y End */
#define GE_DISP_XEND_OFF	0x250	/* Display X End */
#define GE_DISP_YEND_OFF	0x254	/* Display Y End */
#define GE_AMX_CB_OFF		0x258	/* AMX Color Bar (WM8440/8650) */

/* --- AMX Color Key / Alpha Registers --- */
#define GE_G1_CK_EN_OFF	0x298	/* G1 Color Key Enable */
#define GE_G2_CK_EN_OFF	0x29c	/* G2 Color Key Enable */
#define GE_G1_C_KEY_OFF		0x2a0	/* G1 Color Key Value */
#define GE_G2_C_KEY_OFF		0x2a4	/* G2 Color Key Value */
#define GE_G1_AMX_EN_OFF	0x2a8	/* G1 Alpha Mixing Enable */
#define GE_G2_AMX_EN_OFF	0x2ac	/* G2 Alpha Mixing Enable */

#define GE_CK2_APA_OFF		0x2b0	/* CK2 Alpha (WM8435+) */
#define GE_AMX_CTL_OFF		0x2b4	/* AMX Control */
#define GE_CK_APA_OFF		0x2b8	/* CK Alpha */
#define GE_FIX_APA_OFF		0x2bc	/* Fix Alpha */

#define GE_G1_AMX_HM_OFF	0x2c0	/* G1 AMX Hi-Color Mode */
#define GE_G2_AMX_HM_OFF	0x2c4	/* G2 AMX Hi-Color Mode */
#define GE_NH_DATA_OFF		0x2c8	/* No-Hit Data Output */
#define GE_VSYNC_STS_OFF	0x2cc	/* VSync Status (WM8440/8650) */

#define GE_REG_UPD_OFF		0x2d0	/* Register Update (write 1) */
#define GE_REG_SEL_OFF		0x2d4	/* Register Read Select */
#define GE_AMX2_CTL_OFF		0x2d8	/* AMX2 Output Control */
#define GE_FIX2_APA_OFF		0x2dc	/* Fix2 Output Alpha */

#define GE_G1_HSCALE_OFF	0x2e0	/* G1 Horizontal Scaling */
#define GE_G2_HSCALE_OFF	0x2e4	/* G2 Horizontal Scaling */
#define GE_G1_FBW_OFF		0x2e8	/* G1 Framebuffer Width */
#define GE_G1_VCROP_OFF		0x2ec	/* G1 Vertical Crop */
#define GE_G1_HCROP_OFF		0x2f0	/* G1 Horizontal Crop */
#define GE_G2_FBW_OFF		0x2f4	/* G2 Framebuffer Width */
#define GE_G2_VCROP_OFF		0x2f8	/* G2 Vertical Crop */
#define GE_G2_HCROP_OFF		0x2fc	/* G2 Horizontal Crop */

/*
 * GE Command Types (written to GE_COMMAND_OFF)
 * From legacy: ge_regs.h GECMD_* defines
 */
#define GECMD_BLIT		0x01	/* BitBlt (fill/copy with ROP) */
#define GECMD_TEXT		0x02	/* Text/Font rendering */
#define GECMD_BLIT_DMA		0x03	/* DMA Blit with conversion (WM8510) */
#define GECMD_BEZIER		0x04	/* Bezier curve */
#define GECMD_LINE		0x07	/* Line drawing */
#define GECMD_ROTATE		0x08	/* Rotation */
#define GECMD_MIRROR		0x09	/* Mirror */
#define GECMD_DMA		0x0a	/* Direct DMA (WM8510) */

/* High Color Reg */
#define GE_HIGH_COLOR_EN	BIT(31)

/* Command Reg extended bits (for rotate in modern style) */
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

/* Color Key Select bits */
#define GE_CK_SRC_EN		BIT(2)	/* Enable source color key */
#define GE_CK_DST_EN		(BIT(3) | BIT(1))  /* Enable dest color key */

/* Status bits */
#define GE_STS_BUSY		BIT(2)

/*
 * ============================================================
 *  Private state
 * ============================================================
 */

static void __iomem *regbase;

/*
 * ============================================================
 *  Helper: set up common depth/highcolor for an fb_info
 * ============================================================
 */
static inline void ge_set_depth(struct fb_info *p)
{
	writel(p->var.bits_per_pixel == 32 ? 3 :
	      (p->var.bits_per_pixel == 8 ? 0 : 1), regbase + GE_DEPTH_OFF);

	if (p->var.bits_per_pixel == 16 || p->var.bits_per_pixel == 15) {
		/*
		 * Hi-color mode: 0=RGB565, 1=RGB555, 2=RGB454
		 * Check green length to distinguish:
		 *   6 bits green -> 565 (hm=0)
		 *   5 bits green, 5 bits red -> 555 (hm=1)
		 *   5 bits green, 4 bits red -> 454 (hm=2)
		 */
		if (p->var.green.length == 5 && p->var.red.length == 5)
			writel(1, regbase + GE_HIGHCOLOR_OFF); /* RGB555 */
		else if (p->var.green.length == 5 && p->var.red.length == 4)
			writel(2, regbase + GE_HIGHCOLOR_OFF); /* RGB454 */
		else
			writel(0, regbase + GE_HIGHCOLOR_OFF); /* RGB565 */
	} else {
		writel(0, regbase + GE_HIGHCOLOR_OFF);
	}
}

/*
 * ============================================================
 *  Fill Rectangle (with raw RGB color - for IOCTL usage)
 * ============================================================
 */
void wmt_ge_fillrect_rgb(struct fb_info *p, const struct fb_fillrect *rect,
			 u32 rgb_color)
{
	unsigned long pat;

	if (p->state != FBINFO_STATE_RUNNING)
		return;

	pat = pixel_to_pat(p->var.bits_per_pixel, rgb_color);

	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);

	ge_set_depth(p);

	writel(p->fix.smem_start, regbase + GE_DESTBASE_OFF);
	writel(p->var.xres_virtual - 1, regbase + GE_DESTDISPW_OFF);
	writel(p->var.yres_virtual - 1, regbase + GE_DESTDISPH_OFF);
	writel(rect->dx, regbase + GE_DESTAREAX_OFF);
	writel(rect->dy, regbase + GE_DESTAREAY_OFF);
	writel(rect->width - 1, regbase + GE_DESTAREAW_OFF);
	writel(rect->height - 1, regbase + GE_DESTAREAH_OFF);

	writel(pat, regbase + GE_PAT0C_OFF);
	writel(GECMD_BLIT, regbase + GE_COMMAND_OFF);
	writel(rect->rop == ROP_XOR ? 0x5a : 0xf0, regbase + GE_ROPCODE_OFF);
	writel(1, regbase + GE_FIRE_OFF);
}
EXPORT_SYMBOL_GPL(wmt_ge_fillrect_rgb);

/*
 * ============================================================
 *  Fill Rectangle (standard fbdev - resolves pseudo_palette)
 * ============================================================
 */
void wmt_ge_fillrect(struct fb_info *p, const struct fb_fillrect *rect)
{
	unsigned long fg;

	if (p->fix.visual == FB_VISUAL_TRUECOLOR ||
	    p->fix.visual == FB_VISUAL_DIRECTCOLOR)
		fg = ((u32 *)(p->pseudo_palette))[rect->color];
	else
		fg = rect->color;

	wmt_ge_fillrect_rgb(p, rect, fg);
}
EXPORT_SYMBOL_GPL(wmt_ge_fillrect);

/*
 * ============================================================
 *  Copy Area (hardware blit with ROP=SRC)
 * ============================================================
 */
void wmt_ge_copyarea(struct fb_info *p, const struct fb_copyarea *area)
{
	if (p->state != FBINFO_STATE_RUNNING)
		return;

	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);

	ge_set_depth(p);

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
	writel(GECMD_BLIT, regbase + GE_COMMAND_OFF);
	writel(1, regbase + GE_FIRE_OFF);
}
EXPORT_SYMBOL_GPL(wmt_ge_copyarea);

/*
 * ============================================================
 *  Sync - Wait for GE engine idle
 * ============================================================
 */
int wmt_ge_sync(struct fb_info *p)
{
	int loops = 5000000;

	while ((readl(regbase + GE_STATUS_OFF) & GE_STS_BUSY) && --loops)
		cpu_relax();
	return loops > 0 ? 0 : -EBUSY;
}
EXPORT_SYMBOL_GPL(wmt_ge_sync);

/*
 * ============================================================
 *  Rotation (0, 90, 180, 270 degrees)
 *
 *  Uses the legacy proven approach: set rotate_mode register,
 *  then fire GECMD_ROTATE command. This matches the WM8650 BSP
 *  implementation exactly (ge_rotate in ge_regs.c).
 *
 *  Note: Rotation on VT8430/WM8510 requires LUT to be disabled.
 *  On WM8650 this is not needed — handled here for WM8650 only.
 * ============================================================
 */
int wmt_ge_rotate(struct fb_info *p, int angle)
{
	if (p->state != FBINFO_STATE_RUNNING)
		return 0;

	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);

	ge_set_depth(p);

	/* Source surface */
	writel(p->fix.smem_start, regbase + GE_SRCBASE_OFF);
	writel(p->var.xres_virtual - 1, regbase + GE_SRCDISPW_OFF);
	writel(p->var.yres_virtual - 1, regbase + GE_SRCDISPH_OFF);
	writel(0, regbase + GE_SRCAREAX_OFF);
	writel(0, regbase + GE_SRCAREAY_OFF);
	writel(p->var.xres - 1, regbase + GE_SRCAREAW_OFF);
	writel(p->var.yres - 1, regbase + GE_SRCAREAH_OFF);

	/* Destination surface */
	writel(p->fix.smem_start, regbase + GE_DESTBASE_OFF);

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

	/* Set rotation mode via the dedicated register (legacy approach) */
	switch (angle % 360) {
	case 0:   writel(0, regbase + GE_ROTATE_MODE_OFF); break;
	case 90:  writel(1, regbase + GE_ROTATE_MODE_OFF); break;
	case 180: writel(2, regbase + GE_ROTATE_MODE_OFF); break;
	case 270: writel(3, regbase + GE_ROTATE_MODE_OFF); break;
	default:  return -EINVAL;
	}

	/* Fire GECMD_ROTATE with ROP=0 (legacy: ge_set_command(GECMD_ROTATE, 0)) */
	writel(GECMD_ROTATE, regbase + GE_COMMAND_OFF);
	writel(0, regbase + GE_ROPCODE_OFF);
	writel(1, regbase + GE_ENABLE_OFF);
	writel(1, regbase + GE_FIRE_OFF);

	return 0;
}
EXPORT_SYMBOL_GPL(wmt_ge_rotate);

/*
 * ============================================================
 *  Mirror (horizontal flip)
 *
 *  Legacy: set mirror_mode register, fire GECMD_MIRROR.
 *  Mode: 0 = horizontal mirror, 1 = vertical (BIT0 controls).
 *  Source and dest are the same framebuffer.
 * ============================================================
 */
int wmt_ge_mirror(struct fb_info *p, int mode)
{
	if (p->state != FBINFO_STATE_RUNNING)
		return 0;

	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);

	ge_set_depth(p);

	/* Set source = entire visible framebuffer */
	writel(p->fix.smem_start, regbase + GE_SRCBASE_OFF);
	writel(p->var.xres_virtual - 1, regbase + GE_SRCDISPW_OFF);
	writel(p->var.yres_virtual - 1, regbase + GE_SRCDISPH_OFF);
	writel(0, regbase + GE_SRCAREAX_OFF);
	writel(0, regbase + GE_SRCAREAY_OFF);
	writel(p->var.xres - 1, regbase + GE_SRCAREAW_OFF);
	writel(p->var.yres - 1, regbase + GE_SRCAREAH_OFF);

	/* Destination = same surface */
	writel(p->fix.smem_start, regbase + GE_DESTBASE_OFF);
	writel(p->var.xres_virtual - 1, regbase + GE_DESTDISPW_OFF);
	writel(p->var.yres_virtual - 1, regbase + GE_DESTDISPH_OFF);
	writel(0, regbase + GE_DESTAREAX_OFF);
	writel(0, regbase + GE_DESTAREAY_OFF);
	writel(p->var.xres - 1, regbase + GE_DESTAREAW_OFF);
	writel(p->var.yres - 1, regbase + GE_DESTAREAH_OFF);

	/* Set mirror mode and fire */
	writel(mode & 1, regbase + GE_MIRROR_MODE_OFF);
	writel(GECMD_MIRROR, regbase + GE_COMMAND_OFF);
	writel(0, regbase + GE_ROPCODE_OFF);
	writel(1, regbase + GE_FIRE_OFF);

	return 0;
}
EXPORT_SYMBOL_GPL(wmt_ge_mirror);

/*
 * ============================================================
 *  Hardware Line Drawing
 *
 *  Draws a line using the GE line drawing engine.
 *  Supports thickness and line style patterns.
 *
 *  Legacy command: GECMD_LINE (0x07)
 *  Registers: LN_XSTART/XEND/YSTART/YEND + LN_TCK + LN_STL_*
 * ============================================================
 */
int wmt_ge_draw_line(struct fb_info *p, const struct ge_line_user *line)
{
	if (p->state != FBINFO_STATE_RUNNING)
		return 0;

	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);

	ge_set_depth(p);

	/* Set destination surface (line is drawn on the framebuffer) */
	writel(p->fix.smem_start, regbase + GE_DESTBASE_OFF);
	writel(p->var.xres_virtual - 1, regbase + GE_DESTDISPW_OFF);
	writel(p->var.yres_virtual - 1, regbase + GE_DESTDISPH_OFF);

	/* Line color via pattern register */
	writel(line->color, regbase + GE_PAT0C_OFF);

	/* Line coordinates */
	writel(line->x0, regbase + GE_LN_XSTART_OFF);
	writel(line->y0, regbase + GE_LN_YSTART_OFF);
	writel(line->x1, regbase + GE_LN_XEND_OFF);
	writel(line->y1, regbase + GE_LN_YEND_OFF);

	/* Line thickness */
	writel(line->thickness, regbase + GE_LN_TCK_OFF);

	/* Line style (0 = solid line) */
	if (line->style) {
		writel(line->style, regbase + GE_LN_STL_DATA_OFF);
		writel(line->style_count, regbase + GE_LN_STL_TB_OFF);
		writel(0, regbase + GE_LN_STL_RTN_OFF);
		writel(0xff, regbase + GE_LN_STL_APA_OFF);
	} else {
		writel(0xffffffff, regbase + GE_LN_STL_DATA_OFF);
		writel(0, regbase + GE_LN_STL_TB_OFF);
		writel(0, regbase + GE_LN_STL_RTN_OFF);
		writel(0xff, regbase + GE_LN_STL_APA_OFF);
	}

	/* ROP code and fire */
	writel(line->rop ? line->rop : 0xf0, regbase + GE_ROPCODE_OFF);
	writel(GECMD_LINE, regbase + GE_COMMAND_OFF);
	writel(1, regbase + GE_FIRE_OFF);

	return 0;
}
EXPORT_SYMBOL_GPL(wmt_ge_draw_line);

/*
 * ============================================================
 *  Hardware Bezier Curve Drawing
 *
 *  Draws a quadratic Bezier curve using 3 control points.
 *  The GE hardware computes the curve parametrically.
 *
 *  Legacy command: GECMD_BEZIER (0x04)
 *  Registers: BC_P1X/P1Y, BC_P2X/P2Y, BC_P3X/P3Y,
 *             BC_COLOR, BC_ALPHA, BC_DELTA_T
 * ============================================================
 */
int wmt_ge_draw_bezier(struct fb_info *p, const struct ge_bezier_user *bezier)
{
	if (p->state != FBINFO_STATE_RUNNING)
		return 0;

	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);

	ge_set_depth(p);

	/* Destination surface */
	writel(p->fix.smem_start, regbase + GE_DESTBASE_OFF);
	writel(p->var.xres_virtual - 1, regbase + GE_DESTDISPW_OFF);
	writel(p->var.yres_virtual - 1, regbase + GE_DESTDISPH_OFF);

	/* Control points */
	writel(bezier->p1x, regbase + GE_BC_P1X_OFF);
	writel(bezier->p1y, regbase + GE_BC_P1Y_OFF);
	writel(bezier->p2x, regbase + GE_BC_P2X_OFF);
	writel(bezier->p2y, regbase + GE_BC_P2Y_OFF);
	writel(bezier->p3x, regbase + GE_BC_P3X_OFF);
	writel(bezier->p3y, regbase + GE_BC_P3Y_OFF);

	/* Color and alpha */
	writel(bezier->color, regbase + GE_BC_COLOR_OFF);
	writel(bezier->alpha & 0xff, regbase + GE_BC_ALPHA_OFF);

	/* Parameterization step */
	writel(bezier->delta_t ? bezier->delta_t : 1, regbase + GE_BC_DELTA_T_OFF);

	/* Solid line style for the curve */
	writel(0xffffffff, regbase + GE_BC_LSTL_OFF);
	writel(0, regbase + GE_BC_LSTL_RTN_OFF);

	/* Fire bezier command */
	writel(GECMD_BEZIER, regbase + GE_COMMAND_OFF);
	writel(0, regbase + GE_ROPCODE_OFF);
	writel(1, regbase + GE_FIRE_OFF);

	return 0;
}
EXPORT_SYMBOL_GPL(wmt_ge_draw_bezier);

/*
 * ============================================================
 *  General Blit (src->dst with optional alpha and color key)
 *
 *  Performs a source-to-destination blit within the framebuffer.
 *  Supports:
 *    - Arbitrary ROP codes (0xcc=SRC copy, 0xf0=PAT, 0x5a=XOR, etc.)
 *    - Per-blit constant alpha blending (via BITBLT_ALPHA register)
 *    - Source and/or destination color key
 *
 *  All coordinates are within the current framebuffer surface.
 * ============================================================
 */
int wmt_ge_blit(struct fb_info *p, const struct ge_blit_user *blit)
{
	if (p->state != FBINFO_STATE_RUNNING)
		return 0;

	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);

	ge_set_depth(p);

	/* Source setup */
	writel(p->fix.smem_start, regbase + GE_SRCBASE_OFF);
	writel(p->var.xres_virtual - 1, regbase + GE_SRCDISPW_OFF);
	writel(p->var.yres_virtual - 1, regbase + GE_SRCDISPH_OFF);
	writel(blit->sx, regbase + GE_SRCAREAX_OFF);
	writel(blit->sy, regbase + GE_SRCAREAY_OFF);
	writel(blit->width - 1, regbase + GE_SRCAREAW_OFF);
	writel(blit->height - 1, regbase + GE_SRCAREAH_OFF);

	/* Destination setup */
	writel(p->fix.smem_start, regbase + GE_DESTBASE_OFF);
	writel(p->var.xres_virtual - 1, regbase + GE_DESTDISPW_OFF);
	writel(p->var.yres_virtual - 1, regbase + GE_DESTDISPH_OFF);
	writel(blit->dx, regbase + GE_DESTAREAX_OFF);
	writel(blit->dy, regbase + GE_DESTAREAY_OFF);
	writel(blit->width - 1, regbase + GE_DESTAREAW_OFF);
	writel(blit->height - 1, regbase + GE_DESTAREAH_OFF);

	/* Per-blit alpha */
	if (blit->alpha) {
		writel(1, regbase + GE_ALPHA_SEL_OFF);
		writel(blit->alpha & 0xff, regbase + GE_BITBLT_ALPHA_OFF);
	} else {
		writel(0, regbase + GE_ALPHA_SEL_OFF);
		writel(0, regbase + GE_BITBLT_ALPHA_OFF);
	}

	/* Source color key */
	if (blit->src_colorkey >> 24) {
		writel(blit->src_colorkey & 0xffffff, regbase + GE_SRC_CK_OFF);
		writel(readl(regbase + GE_CK_SEL_OFF) | GE_CK_SRC_EN,
		       regbase + GE_CK_SEL_OFF);
	} else {
		writel(readl(regbase + GE_CK_SEL_OFF) & ~GE_CK_SRC_EN,
		       regbase + GE_CK_SEL_OFF);
	}

	/* Destination color key */
	if (blit->dst_colorkey >> 24) {
		writel(blit->dst_colorkey & 0xffffff, regbase + GE_DST_CK_OFF);
		writel(readl(regbase + GE_CK_SEL_OFF) | GE_CK_DST_EN,
		       regbase + GE_CK_SEL_OFF);
	} else {
		writel(readl(regbase + GE_CK_SEL_OFF) & ~GE_CK_DST_EN,
		       regbase + GE_CK_SEL_OFF);
	}

	/* ROP code and fire */
	writel(blit->rop ? blit->rop : 0xcc, regbase + GE_ROPCODE_OFF);
	writel(GECMD_BLIT, regbase + GE_COMMAND_OFF);
	writel(1, regbase + GE_FIRE_OFF);

	/* Clean up: disable per-blit alpha and color keys after use */
	/* (Wait for completion first so registers are latched) */

	return 0;
}
EXPORT_SYMBOL_GPL(wmt_ge_blit);

/*
 * ============================================================
 *  Per-Blit Alpha Blending Configuration
 *
 *  Sets constant alpha for subsequent GE blit operations.
 *  This uses the BITBLT_ALPHA register (0xd8) and ALPHA_SEL (0xd4)
 *  on WM8435/WM8440/WM8650.
 *
 *  NOTE: The previous implementation wrote to offset 0x1c which is
 *  rop_bg_code on WM8650 — that was incorrect. Fixed here.
 * ============================================================
 */
int wmt_ge_rop_alpha_blend(struct fb_info *p, int alpha, int mode)
{
	if (p->state != FBINFO_STATE_RUNNING)
		return 0;

	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);

	alpha &= 0xff;

	/*
	 * Write to the correct WM8650 per-blit alpha registers:
	 *   ALPHA_SEL (0xd4): Select alpha mode (1 = constant alpha enabled)
	 *   BITBLT_ALPHA (0xd8): The actual alpha value (0-255)
	 */
	if (alpha) {
		writel(1, regbase + GE_ALPHA_SEL_OFF);
		writel(alpha, regbase + GE_BITBLT_ALPHA_OFF);
	} else {
		writel(0, regbase + GE_ALPHA_SEL_OFF);
		writel(0, regbase + GE_BITBLT_ALPHA_OFF);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(wmt_ge_rop_alpha_blend);

/*
 * ============================================================
 *  AMX Layer Alpha (Global Graphic Layer Compositing)
 *
 *  Controls the alpha between GE graphic layer (G1) and VPU output.
 *  Formula: DISPLAY = alpha * G1 + (0xFF - alpha) * VPU
 *
 *  This is the WM8650 variant (amx_set_alpha_wm8650 from legacy).
 * ============================================================
 */
int wmt_ge_amx_set_alpha(struct fb_info *p, int alpha)
{
	if (p->state != FBINFO_STATE_RUNNING)
		return 0;

	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);

	alpha &= 0xff;

	/*
	 * WM8650 AMX alpha configuration:
	 *   AMX_CTL = BIT4 (0x10)
	 *   FIX_APA = 0xffff | (alpha << 24)
	 *   CK_APA  = BIT10 (0x400)
	 *   AMX2_CTL = 0x76
	 *   FIX2_APA = 0xff
	 *   CK2_APA  = BIT11 | BIT7 | BIT3 (0x888)
	 */
	writel(BIT(4), regbase + GE_AMX_CTL_OFF);
	writel(0xffff | ((alpha & 0xff) << 24), regbase + GE_FIX_APA_OFF);
	writel(BIT(10), regbase + GE_CK_APA_OFF);
	writel(0x76, regbase + GE_AMX2_CTL_OFF);
	writel(0xff, regbase + GE_FIX2_APA_OFF);
	writel(BIT(11) | BIT(7) | BIT(3), regbase + GE_CK2_APA_OFF);

	return 0;
}
EXPORT_SYMBOL_GPL(wmt_ge_amx_set_alpha);

/*
 * ============================================================
 *  AMX Layer Enable/Disable
 * ============================================================
 */
int wmt_ge_amx_set_en(struct fb_info *p, int enable)
{
	if (p->state != FBINFO_STATE_RUNNING)
		return 0;

	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);

	/* Enable/disable G1 AMX layer */
	writel(enable ? 1 : 0, regbase + GE_G1_AMX_EN_OFF);

	/* When disabling, also disable G2 */
	if (!enable)
		writel(0, regbase + GE_G2_AMX_EN_OFF);

	/* Trigger register update */
	writel(0, regbase + GE_REG_SEL_OFF);  /* select level1 registers */
	writel(1, regbase + GE_REG_UPD_OFF);  /* update level2 registers */

	return 0;
}
EXPORT_SYMBOL_GPL(wmt_ge_amx_set_en);

/*
 * ============================================================
 *  AMX Color Key (G1 layer)
 *
 *  The colorkey argument is in A:R:G:B format (32-bit).
 *  If A (top 8 bits) is non-zero, color key is ENABLED.
 *  If A is zero, color key is DISABLED.
 * ============================================================
 */
int wmt_ge_set_colorkey(struct fb_info *p, unsigned int colorkey)
{
	int enable;
	unsigned int rgb_val;

	if (p->state != FBINFO_STATE_RUNNING)
		return 0;

	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);

	enable = (colorkey >> 24) & 0xff;
	rgb_val = colorkey & 0xffffff;

	writel(rgb_val, regbase + GE_G1_C_KEY_OFF);
	writel(enable ? 1 : 0, regbase + GE_G1_CK_EN_OFF);

	/* Trigger register update */
	writel(0, regbase + GE_REG_SEL_OFF);
	writel(1, regbase + GE_REG_UPD_OFF);

	return 0;
}
EXPORT_SYMBOL_GPL(wmt_ge_set_colorkey);

/*
 * ============================================================
 *  Source Color Key (per-blit)
 *
 *  Sets the source color key for GE blit operations.
 *  When enabled, source pixels matching this color are transparent.
 *
 *  Format: A:R:G:B (A!=0 enables, lower 24 bits = color)
 * ============================================================
 */
int wmt_ge_set_src_colorkey(struct fb_info *p, unsigned int colorkey)
{
	u32 ck_sel;

	if (p->state != FBINFO_STATE_RUNNING)
		return 0;

	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);

	ck_sel = readl(regbase + GE_CK_SEL_OFF);

	if ((colorkey >> 24) & 0xff) {
		/* Enable source color key */
		writel(colorkey & 0xffffff, regbase + GE_SRC_CK_OFF);
		ck_sel |= GE_CK_SRC_EN;
	} else {
		/* Disable source color key */
		ck_sel &= ~GE_CK_SRC_EN;
	}

	writel(ck_sel, regbase + GE_CK_SEL_OFF);

	return 0;
}
EXPORT_SYMBOL_GPL(wmt_ge_set_src_colorkey);

/*
 * ============================================================
 *  Destination Color Key (per-blit)
 *
 *  Sets the destination color key for GE blit operations.
 *  When enabled, destination pixels matching this color are overwritten.
 *
 *  Format: A:R:G:B (A!=0 enables, lower 24 bits = color)
 * ============================================================
 */
int wmt_ge_set_dst_colorkey(struct fb_info *p, unsigned int colorkey)
{
	u32 ck_sel;

	if (p->state != FBINFO_STATE_RUNNING)
		return 0;

	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);

	ck_sel = readl(regbase + GE_CK_SEL_OFF);

	if ((colorkey >> 24) & 0xff) {
		/* Enable destination color key */
		writel(colorkey & 0xffffff, regbase + GE_DST_CK_OFF);
		ck_sel |= GE_CK_DST_EN;
	} else {
		/* Disable destination color key */
		ck_sel &= ~GE_CK_DST_EN;
	}

	writel(ck_sel, regbase + GE_CK_SEL_OFF);

	return 0;
}
EXPORT_SYMBOL_GPL(wmt_ge_set_dst_colorkey);

/*
 * ============================================================
 *  CSC (Color Space Conversion) Configuration
 *
 *  Configures the RGB->YUV conversion coefficients for the
 *  AMX output path. Ported from legacy amx_set_csc().
 *
 *  Supported tables:
 *    GE_CSC_DEFAULT      (0) - No change
 *    GE_CSC_SDTV_16_235  (1) - SDTV with limited range
 *    GE_CSC_SDTV_0_255   (2) - SDTV with full range
 *    GE_CSC_HDTV_16_235  (3) - HDTV with limited range
 *    GE_CSC_HDTV_0_255   (4) - HDTV with full range
 *    GE_CSC_JFIF_0_255   (5) - JFIF/JPEG standard
 *    GE_CSC_SMPTE_170M   (6) - SMPTE 170M
 *    GE_CSC_SMPTE_240M   (7) - SMPTE 240M
 * ============================================================
 */
int wmt_ge_set_csc(struct fb_info *p, int table_id)
{
	if (p->state != FBINFO_STATE_RUNNING)
		return 0;

	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);

	switch (table_id) {
	case GE_CSC_DEFAULT:
		/* No change - leave current CSC settings */
		break;

	case GE_CSC_SDTV_16_235:
		writel(0x132, regbase + GE_C1_COEF_OFF);
		writel(0x259, regbase + GE_C2_COEF_OFF);
		writel(0x75,  regbase + GE_C3_COEF_OFF);
		writel(0x1f50, regbase + GE_C4_COEF_OFF);
		writel(0x1ea5, regbase + GE_C5_COEF_OFF);
		writel(0x20b, regbase + GE_C6_COEF_OFF);
		writel(0x20b, regbase + GE_C7_COEF_OFF);
		writel(0x1e4a, regbase + GE_C8_COEF_OFF);
		writel(0x1fab, regbase + GE_C9_COEF_OFF);
		writel(1,     regbase + GE_COEF_I_OFF);
		writel(0x101, regbase + GE_COEF_J_OFF);
		writel(0x101, regbase + GE_COEF_K_OFF);
		break;

	case GE_CSC_SDTV_0_255:
		writel(0x107, regbase + GE_C1_COEF_OFF);
		writel(0x204, regbase + GE_C2_COEF_OFF);
		writel(0x64,  regbase + GE_C3_COEF_OFF);
		writel(0x1f68, regbase + GE_C4_COEF_OFF);
		writel(0x1ed6, regbase + GE_C5_COEF_OFF);
		writel(0x1c2, regbase + GE_C6_COEF_OFF);
		writel(0x1c2, regbase + GE_C7_COEF_OFF);
		writel(0x1e78, regbase + GE_C8_COEF_OFF);
		writel(0x1fb7, regbase + GE_C9_COEF_OFF);
		writel(0x20,  regbase + GE_COEF_I_OFF);
		writel(0x101, regbase + GE_COEF_J_OFF);
		writel(0x101, regbase + GE_COEF_K_OFF);
		break;

	case GE_CSC_HDTV_16_235:
		writel(0xda,  regbase + GE_C1_COEF_OFF);
		writel(0x2dc, regbase + GE_C2_COEF_OFF);
		writel(0x4a,  regbase + GE_C3_COEF_OFF);
		writel(0x1f88, regbase + GE_C4_COEF_OFF);
		writel(0x1e6d, regbase + GE_C5_COEF_OFF);
		writel(0x20b, regbase + GE_C6_COEF_OFF);
		writel(0x20b, regbase + GE_C7_COEF_OFF);
		writel(0x1e25, regbase + GE_C8_COEF_OFF);
		writel(0x1fd0, regbase + GE_C9_COEF_OFF);
		writel(1,     regbase + GE_COEF_I_OFF);
		writel(0x101, regbase + GE_COEF_J_OFF);
		writel(0x101, regbase + GE_COEF_K_OFF);
		break;

	case GE_CSC_HDTV_0_255:
		writel(0xbb,  regbase + GE_C1_COEF_OFF);
		writel(0x275, regbase + GE_C2_COEF_OFF);
		writel(0x3f,  regbase + GE_C3_COEF_OFF);
		writel(0x1f99, regbase + GE_C4_COEF_OFF);
		writel(0x1ea6, regbase + GE_C5_COEF_OFF);
		writel(0x1c2, regbase + GE_C6_COEF_OFF);
		writel(0x1c2, regbase + GE_C7_COEF_OFF);
		writel(0x1e67, regbase + GE_C8_COEF_OFF);
		writel(0x1fd7, regbase + GE_C9_COEF_OFF);
		writel(0x21,  regbase + GE_COEF_I_OFF);
		writel(0x101, regbase + GE_COEF_J_OFF);
		writel(0x101, regbase + GE_COEF_K_OFF);
		break;

	case GE_CSC_JFIF_0_255:
		writel(0x132, regbase + GE_C1_COEF_OFF);
		writel(0x259, regbase + GE_C2_COEF_OFF);
		writel(0x75,  regbase + GE_C3_COEF_OFF);
		writel(0x1f53, regbase + GE_C4_COEF_OFF);
		writel(0x1ead, regbase + GE_C5_COEF_OFF);
		writel(0x200, regbase + GE_C6_COEF_OFF);
		writel(0x200, regbase + GE_C7_COEF_OFF);
		writel(0x1e53, regbase + GE_C8_COEF_OFF);
		writel(0x1fad, regbase + GE_C9_COEF_OFF);
		writel(0x1,   regbase + GE_COEF_I_OFF);
		writel(0x101, regbase + GE_COEF_J_OFF);
		writel(0x101, regbase + GE_COEF_K_OFF);
		break;

	case GE_CSC_SMPTE_170M:
		writel(0x132, regbase + GE_C1_COEF_OFF);
		writel(0x259, regbase + GE_C2_COEF_OFF);
		writel(0x75,  regbase + GE_C3_COEF_OFF);
		writel(0x1f50, regbase + GE_C4_COEF_OFF);
		writel(0x1ea5, regbase + GE_C5_COEF_OFF);
		writel(0x20b, regbase + GE_C6_COEF_OFF);
		writel(0x20b, regbase + GE_C7_COEF_OFF);
		writel(0x1e4a, regbase + GE_C8_COEF_OFF);
		writel(0x1fab, regbase + GE_C9_COEF_OFF);
		writel(1,     regbase + GE_COEF_I_OFF);
		writel(0x101, regbase + GE_COEF_J_OFF);
		writel(0x101, regbase + GE_COEF_K_OFF);
		break;

	case GE_CSC_SMPTE_240M:
		writel(0xd9,  regbase + GE_C1_COEF_OFF);
		writel(0x2ce, regbase + GE_C2_COEF_OFF);
		writel(0x59,  regbase + GE_C3_COEF_OFF);
		writel(0x2f89, regbase + GE_C4_COEF_OFF);
		writel(0x1e77, regbase + GE_C5_COEF_OFF);
		writel(0x200, regbase + GE_C6_COEF_OFF);
		writel(0x200, regbase + GE_C7_COEF_OFF);
		writel(0x1e38, regbase + GE_C8_COEF_OFF);
		writel(0x1fc8, regbase + GE_C9_COEF_OFF);
		writel(1,     regbase + GE_COEF_I_OFF);
		writel(0x101, regbase + GE_COEF_J_OFF);
		writel(0x101, regbase + GE_COEF_K_OFF);
		break;

	default:
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(wmt_ge_set_csc);

/*
 * ============================================================
 *  GE Engine Delay Configuration
 *
 *  Controls the start cycle delay of the GE engine.
 *  Legacy values: 0x00000 (fastest) to 0x30003 (slowest).
 *  Default from legacy: 0x10001.
 *
 *  Lower values = more aggressive memory bus usage.
 *  Higher values = more conservative, less bus contention.
 * ============================================================
 */
int wmt_ge_set_delay(struct fb_info *p, unsigned int delay)
{
	writel(delay, regbase + GE_DELAY_OFF);
	return 0;
}
EXPORT_SYMBOL_GPL(wmt_ge_set_delay);

/*
 * ============================================================
 *  Platform Driver Probe / Remove
 * ============================================================
 */
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

	/* Enable the GE engine */
	writel(1, regbase + GE_ENABLE_OFF);

	/*
	 * Configure GE start cycle delay.
	 * 0x10001 is the legacy default (balanced performance).
	 * 0x00000 = fastest (aggressive bus), 0x30003 = slowest.
	 */
	writel(0x10001, regbase + GE_DELAY_OFF);

	/*
	 * Initialize CSC to JFIF 0-255 (the legacy default for WM8650).
	 * This matches sRGB / JFIF standard for YUV<->RGB conversion.
	 */
	/* (CSC init deferred - will be set via fb driver or ioctl) */

	/*
	 * Disable per-blit color keys and alpha by default.
	 */
	writel(0, regbase + GE_CK_SEL_OFF);
	writel(0, regbase + GE_ALPHA_SEL_OFF);
	writel(0, regbase + GE_BITBLT_ALPHA_OFF);

	/*
	 * Clear interrupt flags and disable GE interrupts.
	 * We use polling (wmt_ge_sync) for synchronization.
	 */
	writel(0, regbase + GE_INTEN_OFF);
	writel(~0U, regbase + GE_INTFLAG_OFF);

	printk(KERN_INFO "WMT GE: Enabled hardware 2D acceleration "
	       "(fillrect, copyarea, rotate, mirror, line, bezier, "
	       "blit, alpha, colorkey, CSC)\n");

	return 0;
}

static void wmt_ge_rops_remove(struct platform_device *pdev)
{
	iounmap(regbase);
	regbase = NULL;
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
MODULE_DESCRIPTION("Full hardware-accelerated 2D operations using "
		   "WonderMedia Graphics Engine");
MODULE_DEVICE_TABLE(of, wmt_dt_ids);
