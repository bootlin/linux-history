/*
 *  ATI Frame Buffer Device Driver Core Definitions
 */

#include <linux/config.h>


    /*
     *  Elements of the hardware specific atyfb_par structure
     */

struct crtc {
    u32 vxres;
    u32 vyres;
    u32 xoffset;
    u32 yoffset;
    u32 bpp;
    u32 h_tot_disp;
    u32 h_sync_strt_wid;
    u32 v_tot_disp;
    u32 v_sync_strt_wid;
    u32 off_pitch;
    u32 gen_cntl;
    u32 dp_pix_width;	/* acceleration */
    u32 dp_chain_mask;	/* acceleration */
};

struct pll_514 {
    u8 m;
    u8 n;
};

struct pll_18818
{
    u32 program_bits;
    u32 locationAddr;
    u32 period_in_ps;
    u32 post_divider;
};

struct pll_ct {
    u8 pll_ref_div;
    u8 pll_gen_cntl;
    u8 mclk_fb_div;
    u8 pll_vclk_cntl;
    u8 vclk_post_div;
    u8 vclk_fb_div;
    u8 pll_ext_cntl;
    u32 dsp_config;	/* Mach64 GTB DSP */
    u32 dsp_on_off;	/* Mach64 GTB DSP */
    u8 mclk_post_div_real;
    u8 vclk_post_div_real;
};

union aty_pll {
    struct pll_ct ct;
    struct pll_514 ibm514;
    struct pll_18818 ics2595;
};


    /*
     *  The hardware parameters for each card
     */

struct atyfb_par {
    struct crtc crtc;
    union aty_pll pll;
    u32 accel_flags;
};

struct aty_cursor {
    int	enable;
    int on;
    int vbl_cnt;
    int blink_rate;
    u32 offset;
    struct {
        u16 x, y;
    } pos, hot, size;
    u32 color[2];
    u8 bits[8][64];
    u8 mask[8][64];
    u8 *ram;
    struct timer_list *timer;
};

struct fb_info_aty {
    struct fb_info fb_info;
    struct fb_info_aty *next;
    unsigned long ati_regbase;
    unsigned long clk_wr_offset;
    struct pci_mmap_map *mmap_map;
    struct aty_cursor *cursor;
    struct aty_cmap_regs *aty_cmap_regs;
    struct atyfb_par default_par;
    struct atyfb_par current_par;
    u32 features;
    u32 ref_clk_per;
    u32 pll_per;
    u32 mclk_per;
    u8 bus_type;
    u8 ram_type;
    u8 mem_refresh_rate;
    const struct aty_dac_ops *dac_ops;
    const struct aty_pll_ops *pll_ops;
    struct display_switch dispsw;
    u8 blitter_may_be_busy;
#ifdef __sparc__
    u8 mmaped;
    int open;
    int vtconsole;
    int consolecnt;
#endif
#ifdef CONFIG_PMAC_PBOOK
    unsigned char *save_framebuffer;
    unsigned long save_pll[64];
#endif
};


    /*
     *  ATI Mach64 features
     */

#define M64_HAS(feature)	((info)->features & (M64F_##feature))

#define M64F_RESET_3D		0x00000001
#define M64F_MAGIC_FIFO		0x00000002
#define M64F_GTB_DSP		0x00000004
#define M64F_FIFO_24		0x00000008
#define M64F_SDRAM_MAGIC_PLL	0x00000010
#define M64F_MAGIC_POSTDIV	0x00000020
#define M64F_INTEGRATED		0x00000040
#define M64F_CT_BUS		0x00000080
#define M64F_VT_BUS		0x00000100
#define M64F_MOBIL_BUS		0x00000200
#define M64F_GX			0x00000400
#define M64F_CT			0x00000800
#define M64F_VT			0x00001000
#define M64F_GT			0x00002000
#define M64F_MAGIC_VRAM_SIZE	0x00004000
#define M64F_G3_PB_1_1		0x00008000
#define M64F_G3_PB_1024x768	0x00010000
#define M64F_EXTRA_BRIGHT	0x00020000
#define M64F_LT_SLEEP		0x00040000
#define M64F_XL_DLL		0x00080000


    /*
     *  Register access
     */

static inline u32 aty_ld_le32(int regindex,
			      const struct fb_info_aty *info)
{
    /* Hack for bloc 1, should be cleanly optimized by compiler */
    if (regindex >= 0x400)
    	regindex -= 0x800;

#if defined(__mc68000__)
    return le32_to_cpu(*((volatile u32 *)(info->ati_regbase+regindex)));
#else
    return readl (info->ati_regbase + regindex);
#endif
}

static inline void aty_st_le32(int regindex, u32 val,
			       const struct fb_info_aty *info)
{
    /* Hack for bloc 1, should be cleanly optimized by compiler */
    if (regindex >= 0x400)
    	regindex -= 0x800;

#if defined(__mc68000__)
    *((volatile u32 *)(info->ati_regbase+regindex)) = cpu_to_le32(val);
#else
    writel (val, info->ati_regbase + regindex);
#endif
}

static inline u8 aty_ld_8(int regindex,
			  const struct fb_info_aty *info)
{
    /* Hack for bloc 1, should be cleanly optimized by compiler */
    if (regindex >= 0x400)
    	regindex -= 0x800;

    return readb (info->ati_regbase + regindex);
}

static inline void aty_st_8(int regindex, u8 val,
			    const struct fb_info_aty *info)
{
    /* Hack for bloc 1, should be cleanly optimized by compiler */
    if (regindex >= 0x400)
    	regindex -= 0x800;

    writeb (val, info->ati_regbase + regindex);
}

static inline u8 aty_ld_pll(int offset, const struct fb_info_aty *info)
{
    u8 res;

    /* write addr byte */
    aty_st_8(CLOCK_CNTL + 1, (offset << 2), info);
    /* read the register value */
    res = aty_ld_8(CLOCK_CNTL + 2, info);
    return res;
}


    /*
     *  DAC operations
     */

struct aty_dac_ops {
    int (*set_dac)(const struct fb_info_aty *info, const union aty_pll *pll,
		   u32 bpp, u32 accel);
};

extern const struct aty_dac_ops aty_dac_ibm514;		/* IBM RGB514 */
extern const struct aty_dac_ops aty_dac_ati68860b;	/* ATI 68860-B */
extern const struct aty_dac_ops aty_dac_att21c498;	/* AT&T 21C498 */
extern const struct aty_dac_ops aty_dac_unsupported;	/* unsupported */
extern const struct aty_dac_ops aty_dac_ct;		/* Integrated */


    /*
     *  Clock operations
     */

struct aty_pll_ops {
    int (*var_to_pll)(const struct fb_info_aty *info, u32 vclk_per, u8 bpp,
		      union aty_pll *pll);
    u32 (*pll_to_var)(const struct fb_info_aty *info,
	    	      const union aty_pll *pll);
    void (*set_pll)(const struct fb_info_aty *info, const union aty_pll *pll);
};

extern const struct aty_pll_ops aty_pll_ati18818_1;	/* ATI 18818 */
extern const struct aty_pll_ops aty_pll_stg1703;	/* STG 1703 */
extern const struct aty_pll_ops aty_pll_ch8398;		/* Chrontel 8398 */
extern const struct aty_pll_ops aty_pll_att20c408;	/* AT&T 20C408 */
extern const struct aty_pll_ops aty_pll_ibm514;		/* IBM RGB514 */
extern const struct aty_pll_ops aty_pll_unsupported;	/* unsupported */
extern const struct aty_pll_ops aty_pll_ct;		/* Integrated */


extern void aty_set_pll_ct(const struct fb_info_aty *info,
			   const union aty_pll *pll);
extern void aty_calc_pll_ct(const struct fb_info_aty *info,
			    struct pll_ct *pll);


    /*
     *  Hardware cursor support
     */

extern struct aty_cursor *aty_init_cursor(struct fb_info_aty *fb);
extern void atyfb_cursor(struct display *p, int mode, int x, int y);
extern void aty_set_cursor_color(struct fb_info_aty *fb);
extern void aty_set_cursor_shape(struct fb_info_aty *fb);
extern int atyfb_set_font(struct display *d, int width, int height);


    /*
     *  Hardware acceleration
     */

static inline void wait_for_fifo(u16 entries, const struct fb_info_aty *info)
{
    while ((aty_ld_le32(FIFO_STAT, info) & 0xffff) >
	   ((u32)(0x8000 >> entries)));
}

static inline void wait_for_idle(struct fb_info_aty *info)
{
    wait_for_fifo(16, info);
    while ((aty_ld_le32(GUI_STAT, info) & 1)!= 0);
    info->blitter_may_be_busy = 0;
}

extern void aty_reset_engine(const struct fb_info_aty *info);
extern void aty_init_engine(const struct atyfb_par *par,
			    struct fb_info_aty *info);
extern void aty_rectfill(int dstx, int dsty, u_int width, u_int height,
			 u_int color, struct fb_info_aty *info);


    /*
     *  Text console acceleration
     */

extern const struct display_switch fbcon_aty8;
extern const struct display_switch fbcon_aty16;
extern const struct display_switch fbcon_aty24;
extern const struct display_switch fbcon_aty32;

