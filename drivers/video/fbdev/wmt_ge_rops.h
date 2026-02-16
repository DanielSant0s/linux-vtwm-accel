/* SPDX-License-Identifier: GPL-2.0 */
/*
 * WonderMedia Graphics Engine (GE) acceleration interface
 *
 * Hardware features ported from legacy WM8650/WM8505 GE driver
 * by Vincent Chen <vincentchen@wondermedia.com.tw>
 *
 * Supported operations:
 *   - Fill rectangle (pattern ROP)
 *   - Copy area (source ROP)
 *   - Rotation (0/90/180/270)
 *   - Mirror (horizontal)
 *   - Line drawing (with style/thickness)
 *   - Bezier curve drawing
 *   - General blit (src->dst with ROP/alpha/colorkey)
 *   - Source/Destination color key
 *   - Per-blit alpha blending
 *   - AMX layer alpha (global graphic layer alpha)
 *   - AMX layer enable/disable
 *   - CSC (Color Space Conversion) configuration
 *   - GE engine timing configuration
 */

#ifndef WMT_GE_ROPS_H
#define WMT_GE_ROPS_H

#include <linux/fb.h>
#include <linux/ioctl.h>

/*
 * ============================================================
 *  IOCTL definitions for userspace GE access
 * ============================================================
 */
#define GEIO_MAGIC	'V'

/* Basic control */
#define GEIO_STOP_LOGO		_IO(GEIO_MAGIC, 0x00)
#define GEIOSET_AMX_EN		_IOW(GEIO_MAGIC, 0x01, unsigned int)
#define GEIO_LOCK		_IOW(GEIO_MAGIC, 0x03, unsigned int)

/* Alpha / blending */
#define GEIO_ALPHA_BLEND	_IOW(GEIO_MAGIC, 0x06, unsigned int)
#define GEIO_ROP_ALPHA		_IOW(GEIO_MAGIC, 0x0E, unsigned int)

/* Transform */
#define GEIO_ROTATE		_IOW(GEIO_MAGIC, 0x08, unsigned int)
#define GEIO_MIRROR		_IOW(GEIO_MAGIC, 0x0F, unsigned int)

/* Drawing primitives */
#define GEIO_FILLRECT		_IOW(GEIO_MAGIC, 0x0A, struct ge_fillrect_user)
#define GEIO_COPYAREA		_IOW(GEIO_MAGIC, 0x0B, struct ge_copyarea_user)
#define GEIO_LINE		_IOW(GEIO_MAGIC, 0x10, struct ge_line_user)
#define GEIO_BEZIER		_IOW(GEIO_MAGIC, 0x11, struct ge_bezier_user)
#define GEIO_BLIT		_IOW(GEIO_MAGIC, 0x12, struct ge_blit_user)

/* Color key */
#define GEIOSET_COLORKEY	_IOW(GEIO_MAGIC, 0x0C, unsigned int)
#define GEIO_SET_SRC_CK		_IOW(GEIO_MAGIC, 0x13, unsigned int)
#define GEIO_SET_DST_CK		_IOW(GEIO_MAGIC, 0x14, unsigned int)

/* CSC and configuration */
#define GEIO_SET_CSC		_IOW(GEIO_MAGIC, 0x15, unsigned int)
#define GEIOGET_CHIP_ID		_IOR(GEIO_MAGIC, 0x16, unsigned int)
#define GEIO_SET_DELAY		_IOW(GEIO_MAGIC, 0x17, unsigned int)

/* Sync */
#define GEIO_WAIT_SYNC		_IO(GEIO_MAGIC, 0x0D)

/*
 * ============================================================
 *  IOCTL argument structures
 * ============================================================
 */

struct ge_fillrect_user {
	unsigned int dx;
	unsigned int dy;
	unsigned int width;
	unsigned int height;
	unsigned int color;	/* Raw pixel value (RGB565/RGB32) */
	unsigned int rop;	/* ROP_COPY = 0, ROP_XOR = 1 */
};

struct ge_copyarea_user {
	unsigned int dx;
	unsigned int dy;
	unsigned int sx;
	unsigned int sy;
	unsigned int width;
	unsigned int height;
};

struct ge_line_user {
	unsigned int x0, y0;		/* Start point */
	unsigned int x1, y1;		/* End point */
	unsigned int color;		/* Line color (raw pixel) */
	unsigned int thickness;		/* Line thickness (ln_tck) */
	unsigned int style;		/* Line style pattern bits (0=solid) */
	unsigned int style_count;	/* Style pattern length */
	unsigned int rop;		/* ROP code (0xcc=copy, 0xf0=pat) */
};

struct ge_bezier_user {
	unsigned int p1x, p1y;		/* Control point 1 (start) */
	unsigned int p2x, p2y;		/* Control point 2 */
	unsigned int p3x, p3y;		/* Control point 3 (end) */
	unsigned int color;		/* Curve color (raw pixel) */
	unsigned int alpha;		/* Alpha (0-255) */
	unsigned int delta_t;		/* Parameterization step */
};

struct ge_blit_user {
	unsigned int sx, sy;		/* Source coordinates in FB */
	unsigned int dx, dy;		/* Destination coordinates in FB */
	unsigned int width, height;
	unsigned int rop;		/* ROP code: 0xcc=SRC, 0xf0=PAT, 0x5a=XOR... */
	unsigned int alpha;		/* 0=disabled, 1-255=constant alpha */
	unsigned int src_colorkey;	/* Source colorkey (0=disabled, A!=0 enables) */
	unsigned int dst_colorkey;	/* Dest colorkey (0=disabled, A!=0 enables) */
};

/*
 * CSC (Color Space Conversion) table IDs
 * For use with GEIO_SET_CSC
 */
#define GE_CSC_DEFAULT		0
#define GE_CSC_SDTV_16_235	1
#define GE_CSC_SDTV_0_255	2
#define GE_CSC_HDTV_16_235	3
#define GE_CSC_HDTV_0_255	4
#define GE_CSC_JFIF_0_255	5
#define GE_CSC_SMPTE_170M	6
#define GE_CSC_SMPTE_240M	7

/*
 * ============================================================
 *  Kernel API - exported GE functions
 * ============================================================
 */

#ifdef CONFIG_FB_WMT_GE_ROPS

/* --- Core fbdev ops (already existing) --- */
extern void wmt_ge_fillrect(struct fb_info *info,
			    const struct fb_fillrect *rect);
extern void wmt_ge_fillrect_rgb(struct fb_info *info,
			    const struct fb_fillrect *rect,
			    unsigned int rgb_color);
extern void wmt_ge_copyarea(struct fb_info *info,
			    const struct fb_copyarea *area);
extern int wmt_ge_sync(struct fb_info *info);

/* --- Transform operations --- */
extern int wmt_ge_rotate(struct fb_info *info, int angle);
extern int wmt_ge_mirror(struct fb_info *info, int mode);

/* --- Drawing primitives --- */
extern int wmt_ge_draw_line(struct fb_info *info,
			    const struct ge_line_user *line);
extern int wmt_ge_draw_bezier(struct fb_info *info,
			      const struct ge_bezier_user *bezier);

/* --- General blit with alpha/colorkey --- */
extern int wmt_ge_blit(struct fb_info *info,
		       const struct ge_blit_user *blit);

/* --- Alpha blending --- */
extern int wmt_ge_rop_alpha_blend(struct fb_info *info, int alpha, int mode);
extern int wmt_ge_amx_set_alpha(struct fb_info *info, int alpha);
extern int wmt_ge_amx_set_en(struct fb_info *info, int enable);

/* --- Color key --- */
extern int wmt_ge_set_colorkey(struct fb_info *info, unsigned int colorkey);
extern int wmt_ge_set_src_colorkey(struct fb_info *info, unsigned int colorkey);
extern int wmt_ge_set_dst_colorkey(struct fb_info *info, unsigned int colorkey);

/* --- CSC (Color Space Conversion) --- */
extern int wmt_ge_set_csc(struct fb_info *info, int table_id);

/* --- Engine configuration --- */
extern int wmt_ge_set_delay(struct fb_info *info, unsigned int delay);

#else /* !CONFIG_FB_WMT_GE_ROPS */

/*
 * Stubs when GE acceleration is not compiled in
 */
static inline int wmt_ge_sync(struct fb_info *p)
{
	return 0;
}

static inline void wmt_ge_fillrect(struct fb_info *p,
				    const struct fb_fillrect *rect)
{
	sys_fillrect(p, rect);
}

static inline void wmt_ge_copyarea(struct fb_info *p,
				     const struct fb_copyarea *area)
{
	sys_copyarea(p, area);
}

static inline int wmt_ge_rotate(struct fb_info *p, int angle)
{
	return -ENOSYS;
}

static inline int wmt_ge_mirror(struct fb_info *p, int mode)
{
	return -ENOSYS;
}

static inline int wmt_ge_draw_line(struct fb_info *p,
				   const struct ge_line_user *line)
{
	return -ENOSYS;
}

static inline int wmt_ge_draw_bezier(struct fb_info *p,
				     const struct ge_bezier_user *bezier)
{
	return -ENOSYS;
}

static inline int wmt_ge_blit(struct fb_info *p,
			      const struct ge_blit_user *blit)
{
	return -ENOSYS;
}

static inline int wmt_ge_rop_alpha_blend(struct fb_info *p, int alpha, int mode)
{
	return -ENOSYS;
}

static inline int wmt_ge_amx_set_alpha(struct fb_info *p, int alpha)
{
	return -ENOSYS;
}

static inline int wmt_ge_amx_set_en(struct fb_info *p, int enable)
{
	return -ENOSYS;
}

static inline int wmt_ge_set_colorkey(struct fb_info *p, unsigned int ck)
{
	return -ENOSYS;
}

static inline int wmt_ge_set_src_colorkey(struct fb_info *p, unsigned int ck)
{
	return -ENOSYS;
}

static inline int wmt_ge_set_dst_colorkey(struct fb_info *p, unsigned int ck)
{
	return -ENOSYS;
}

static inline int wmt_ge_set_csc(struct fb_info *p, int table_id)
{
	return -ENOSYS;
}

static inline int wmt_ge_set_delay(struct fb_info *p, unsigned int delay)
{
	return -ENOSYS;
}

#endif /* CONFIG_FB_WMT_GE_ROPS */

#endif /* WMT_GE_ROPS_H */
