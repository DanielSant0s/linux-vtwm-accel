/* SPDX-License-Identifier: GPL-2.0 */
#ifdef CONFIG_FB_WMT_GE_ROPS

extern void wmt_ge_fillrect(struct fb_info *info,
			    const struct fb_fillrect *rect);
extern void wmt_ge_fillrect_rgb(struct fb_info *info,
			    const struct fb_fillrect *rect, unsigned int rgb_color);
extern void wmt_ge_copyarea(struct fb_info *info,
			    const struct fb_copyarea *area);
extern int wmt_ge_sync(struct fb_info *info);

/*
 * Legacy GE extensions
 */
#define GEIO_MAGIC	'V'
#define GEIO_STOP_LOGO	_IO(GEIO_MAGIC, 0x00)
#define GEIOSET_AMX_EN	_IOW(GEIO_MAGIC, 0x01, unsigned int)
#define GEIO_LOCK	_IOW(GEIO_MAGIC, 0x03, unsigned int)
#define GEIO_LOCK	_IOW(GEIO_MAGIC, 0x03, unsigned int)
#define GEIO_ALPHA_BLEND _IOW(GEIO_MAGIC, 0x06, unsigned int) /* AMX Global Alpha */
#define GEIO_ROTATE	_IOW(GEIO_MAGIC, 0x08, unsigned int)
#define GEIO_ROP_ALPHA	_IOW(GEIO_MAGIC, 0x0E, unsigned int) /* Drawing/ROP Alpha */
#define GEIO_FILLRECT	_IOW(GEIO_MAGIC, 0x0A, struct ge_fillrect_user)

struct ge_fillrect_user {
	unsigned int dx;
	unsigned int dy;
	unsigned int width;
	unsigned int height;
	unsigned int color; /* ROP is assumed COPY or XOR based on mode if needed, but lets stick to color */
	unsigned int rop;   /* ROP_COPY = 0, ROP_XOR = 1 */
};

struct ge_copyarea_user {
	unsigned int dx;
	unsigned int dy;
	unsigned int sx;
	unsigned int sy;
	unsigned int width;
	unsigned int height;
};

#define GEIO_COPYAREA	_IOW(GEIO_MAGIC, 0x0B, struct ge_copyarea_user)
#define GEIOSET_COLORKEY _IOW(GEIO_MAGIC, 0x0C, unsigned int)
#define GEIO_WAIT_SYNC	_IO(GEIO_MAGIC, 0x0D)

extern int wmt_ge_rotate(struct fb_info *p, int angle);
extern int wmt_ge_rop_alpha_blend(struct fb_info *p, int alpha, int mode);
extern int wmt_ge_amx_set_alpha(struct fb_info *p, int alpha);
extern int wmt_ge_amx_set_en(struct fb_info *p, int enable);
extern int wmt_ge_set_colorkey(struct fb_info *p, unsigned int colorkey);

#else

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

#endif
