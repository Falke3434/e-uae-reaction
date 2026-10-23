/*
 * UAE - The U*nix Amiga Emulator
 *
 * Picasso96 Support Module
 *
 * Copyright 1997-2001 Brian King <Brian_King@Mitel.com, Brian_King@Cloanto.com>
 * Copyright 2000-2001 Bernd Roesch
 * Copyright 2003-2005 Richard Drummond
 *
 * Theory of operation:
 * On the Amiga side, a Picasso card consists mainly of a memory area that
 * contains the frame buffer.  On the UAE side, we allocate a block of memory
 * that will hold the frame buffer.  This block is in normal memory, it is
 * never directly on the graphics card.  All graphics operations, which are
 * mainly reads and writes into this block and a few basic operations like
 * filling a rectangle, operate on this block of memory.
 * Since the memory is not on the graphics card, some work must be done to
 * synchronize the display with the data in the Picasso frame buffer.  There
 * are various ways to do this.  One possibility is to allocate a second
 * buffer of the same size, and perform all write operations twice.  Since
 * we never read from the second buffer, it can actually be placed in video
 * memory.  The X11 driver could be made to use the Picasso frame buffer as
 * the data buffer of an XImage, which could then be XPutImage()d from time
 * to time.  Another possibility is to translate all Picasso accesses into
 * Xlib (or GDI, or whatever your graphics system is) calls.  This possibility
 * is a bit tricky, since there is a risk of generating very many single pixel
 * accesses which may be rather slow.
 *
 * TODO:
 * - we want to add a manual switch to override SetSwitch for hardware banging
 *   programs started from a Picasso workbench.
 */

#include <proto/exec.h>

#include <stdbool.h>

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "uae.h"
#include "memory.h"
#include "custom.h"
#include "newcpu.h"
#include "xwin.h"
#include "picasso96.h"
#include "uae_endian.h"

bool debug_crash = false;
#define debug_crashed(fmt,...) if (debug_crash) DebugPrintF(fmt, ##__VA_ARGS__)

#ifdef JIT
int        have_done_picasso       = 0;         /* For the JIT compiler */
# ifdef PICASSO96
static int picasso_is_special      = PIC_WRITE; /* ditto */
/* v16: reads of the graphics card memory are no longer "special" for the
 * JIT: gfxmem_bank.baseaddr points to gfxmemory (expansion.c), so translated
 * code reads the framebuffer directly instead of calling gfxmem_*get().
 * Writes stay special, they must go through gfxmem_*put() so that the
 * changed lines are copied to the screen (flush_write). */
static int picasso_is_special_read = 0;
# endif
#endif

#ifdef PICASSO96

static int p96syncrate;
int p96hsync_counter;

int p96hack_vpos, p96hack_vpos2, p96refresh_active;

#define P96TRACING_ENABLED 0
#if P96TRACING_ENABLED
#define P96TRACE(x)	do { write_log x; } while(0)
#else
#define P96TRACE(x)     do { ; } while(0)
#endif

static uae_u32 gfxmem_lget (uaecptr) REGPARAM;
static uae_u32 gfxmem_wget (uaecptr) REGPARAM;
static uae_u32 gfxmem_bget (uaecptr) REGPARAM;
static void gfxmem_lput (uaecptr, uae_u32) REGPARAM;
static void gfxmem_wput (uaecptr, uae_u32) REGPARAM;
static void gfxmem_bput (uaecptr, uae_u32) REGPARAM;
static int gfxmem_check (uaecptr addr, uae_u32 size) REGPARAM;
static uae_u8 *gfxmem_xlate (uaecptr addr) REGPARAM;

static uae_u8 all_ones_bitmap, all_zeros_bitmap;

struct picasso96_state_struct picasso96_state;
struct picasso_vidbuf_description picasso_vidinfo;

/* These are the maximum resolutions. They are filled in by
 * GetSupportedResolutions() */
/* have to fill this in, otherwise problems occur
 * @@@ ??? what problems?
 */
struct ScreenResolution planar      = { 320, 240 };
struct ScreenResolution chunky      = { 640, 480 };
struct ScreenResolution hicolour    = { 640, 480 };
struct ScreenResolution truecolour  = { 640, 480 };
struct ScreenResolution alphacolour = { 640, 480 };

uae_u16 picasso96_pixel_format = RGBFF_CHUNKY;

struct PicassoResolution DisplayModes[MAX_PICASSO_MODES];

static int mode_count = 0;

static int set_gc_called = 0;
static int set_panning_called = 0;
/* Address of the screen in the Amiga frame buffer at the time of the last
   SetPanning call.  */
//static uaecptr oldscr;

static uae_u32 p2ctab[256][2];

bool output_clut_needs_update = false;
extern void output_update_clut(void);


/*
 * Picasso96 seems to have a bug which screws the palette emulation in
 * ARGB32 modes. We work around this by using a BGRA32 framebuffer instead,
 * and byte-swapping each pixel when output to the real screen
 */
static int need_argb32_hack = 0;

/* This stuff should probably be moved elsewhere */
#if (defined __powerpc__ || defined __ppc__ || defined __POWERPC__ \
     || defined __PPC__) && defined __GNUC__
STATIC_INLINE void memcpy_bswap32 (void *dst, void *src, int n)
{
    int words = n / 4;

    if (words > 1) {
	uae_u32 tmp;
	asm volatile (
	    "addi    %2, %2, -1		\n\
	     mtctr   %2			\n\
	     lwz     %3, 0(%1)		\n\
	1:   stwbrx  %3, 0, %0		\n\
	     addi    %0, %0, 4  	\n\
	     lwzu    %3, 4(%1)		\n\
	     bdnz    1b         	\n\
	     stwbrx  %3, 0, %0"
	: "+r" (dst), "+r" (src), "+r" (words), "=r" (tmp)
	:
	:  "ctr", "memory");
   } else {
	uae_u32 tmp;
	asm volatile (
	    "lwz     %2, 0(%1)		\n\
	     stwbrx  %2, 0, %0"
	: "+r" (dst), "+r" (src), "=r" (tmp)
	:
	: "memory");
   }
}
#else
STATIC_INLINE void memcpy_bswap32 (void *dst, void *src, int n)
{
    int i = n / 4;
    uae_u32 *dstp = (uae_u32 *)dst;
    uae_u32 *srcp = (uae_u32 *)src;
    for ( ; i; i--)
	*dstp++ = bswap_32 (*srcp++);
}
#endif


/*
 * Debugging dumps
 */

static void DumpModeInfoStructure (uaecptr amigamodeinfoptr)
{
    write_log ("ModeInfo Structure Dump:\n");
    write_log ("  Node.ln_Succ  = 0x%x\n", get_long (amigamodeinfoptr));
    write_log ("  Node.ln_Pred  = 0x%x\n", get_long (amigamodeinfoptr + 4));
    write_log ("  Node.ln_Type  = 0x%x\n", get_byte (amigamodeinfoptr + 8));
    write_log ("  Node.ln_Pri   = %d\n",   get_byte (amigamodeinfoptr + 9));
    /*write_log ("  Node.ln_Name  = %s\n", uaememptr->Node.ln_Name); */
    write_log ("  OpenCount     = %d\n",   get_word (amigamodeinfoptr + PSSO_ModeInfo_OpenCount));
    write_log ("  Active        = %d\n",   get_byte (amigamodeinfoptr + PSSO_ModeInfo_Active));
    write_log ("  Width         = %d\n",   get_word (amigamodeinfoptr + PSSO_ModeInfo_Width));
    write_log ("  Height        = %d\n",   get_word (amigamodeinfoptr + PSSO_ModeInfo_Height));
    write_log ("  Depth         = %d\n",   get_byte (amigamodeinfoptr + PSSO_ModeInfo_Depth));
    write_log ("  Flags         = %d\n",   get_byte (amigamodeinfoptr + PSSO_ModeInfo_Flags));
    write_log ("  HorTotal      = %d\n",   get_word (amigamodeinfoptr + PSSO_ModeInfo_HorTotal));
    write_log ("  HorBlankSize  = %d\n",   get_word (amigamodeinfoptr + PSSO_ModeInfo_HorBlankSize));
    write_log ("  HorSyncStart  = %d\n",   get_word (amigamodeinfoptr + PSSO_ModeInfo_HorSyncStart));
    write_log ("  HorSyncSize   = %d\n",   get_word (amigamodeinfoptr + PSSO_ModeInfo_HorSyncSize));
    write_log ("  HorSyncSkew   = %d\n",   get_byte (amigamodeinfoptr + PSSO_ModeInfo_HorSyncSkew));
    write_log ("  HorEnableSkew = %d\n",   get_byte (amigamodeinfoptr + PSSO_ModeInfo_HorEnableSkew));
    write_log ("  VerTotal      = %d\n",   get_word (amigamodeinfoptr + PSSO_ModeInfo_VerTotal));
    write_log ("  VerBlankSize  = %d\n",   get_word (amigamodeinfoptr + PSSO_ModeInfo_VerBlankSize));
    write_log ("  VerSyncStart  = %d\n",   get_word (amigamodeinfoptr + PSSO_ModeInfo_VerSyncStart));
    write_log ("  VerSyncSize   = %d\n",   get_word (amigamodeinfoptr + PSSO_ModeInfo_VerSyncSize));
    write_log ("  Clock         = %d\n",   get_byte (amigamodeinfoptr + PSSO_ModeInfo_first_union));
    write_log ("  ClockDivide   = %d\n",   get_byte (amigamodeinfoptr + PSSO_ModeInfo_second_union));
    write_log ("  PixelClock    = %d\n",   get_long (amigamodeinfoptr + PSSO_ModeInfo_PixelClock));
}

static void DumpLibResolutionStructure (uaecptr amigalibresptr)
{
    int i;
    uaecptr amigamodeinfoptr;
    struct LibResolution *uaememptr = (struct LibResolution *)
				      get_mem_bank (amigalibresptr).xlateaddr (amigalibresptr);

    return;

    write_log ("LibResolution Structure Dump:\n");

    if (get_long (amigalibresptr + PSSO_LibResolution_DisplayID) == 0xFFFFFFFF) {
	write_log ("  Finished With LibResolutions...\n");
    } else {
	write_log ("  Name      = %s\n", uaememptr->P96ID);
	write_log ("  DisplayID = 0x%x\n", get_long (amigalibresptr + PSSO_LibResolution_DisplayID));
	write_log ("  Width     = %d\n",   get_word (amigalibresptr + PSSO_LibResolution_Width));
	write_log ("  Height    = %d\n",   get_word (amigalibresptr + PSSO_LibResolution_Height));
	write_log ("  Flags     = %d\n",   get_word (amigalibresptr + PSSO_LibResolution_Flags));
	for (i = 0; i < MAXMODES; i++) {
	    amigamodeinfoptr = get_long (amigalibresptr + PSSO_LibResolution_Modes + i * 4);
	    write_log ("  ModeInfo[%d] = 0x%x\n", i, amigamodeinfoptr);
	    if (amigamodeinfoptr)
		DumpModeInfoStructure (amigamodeinfoptr);
	}
	write_log ("  BoardInfo = 0x%x\n", get_long (amigalibresptr + PSSO_LibResolution_BoardInfo));
    }
}

static char binary_byte[9];

static char *BuildBinaryString (uae_u8 value)
{
    int i;
    for (i = 0; i < 8; i++) {
	binary_byte[i] = (value & (1 << (7 - i))) ? '#' : '.';
    }
    binary_byte[8] = '\0';
    return binary_byte;
}

static void DumpPattern (struct Pattern *patt)
{
/*
    uae_u8 *mem;
    int row, col;
    for (row = 0; row < (1 << patt->Size); row++) {
	mem = patt->Memory + row * 2;
	for (col = 0; col < 2; col++) {
	    write_log ("%s", BuildBinaryString (*mem++));
	}
	write_log ("\n");
    }
*/
}

static void DumpTemplate (struct Template *tmp, uae_u16 w, uae_u16 h)
{
/*
    uae_u8 *mem = tmp->Memory;
    int row, col, width;
    width = (w + 7) >> 3;
    write_log ("xoffset = %d, bpr = %d\n", tmp->XOffset, tmp->BytesPerRow);
    for (row = 0; row < h; row++) {
	mem = tmp->Memory + row * tmp->BytesPerRow;
	for (col = 0; col < width; col++) {
	    write_log ("%s", BuildBinaryString (*mem++));
	}
	write_log ("\n");
    }
*/
  }

int picasso_nr_resolutions (void)
{
    return mode_count;
}

static void ShowSupportedResolutions (void)
{
    int i;

    return;

    for (i = 0; i < mode_count; i++)
	write_log ("%s\n", DisplayModes[i].name);
}

STATIC_INLINE uae_u8 GetBytesPerPixel (uae_u32 RGBfmt)
{
    switch (RGBfmt) {
    case RGBFB_CLUT:
	return 1;

    case RGBFB_A8R8G8B8:
    case RGBFB_A8B8G8R8:
    case RGBFB_R8G8B8A8:
    case RGBFB_B8G8R8A8:
	return 4;

    case RGBFB_B8G8R8:
    case RGBFB_R8G8B8:
	return 3;

    case RGBFB_R5G5B5:
    case RGBFB_R5G6B5:
    case RGBFB_R5G6B5PC:
    case RGBFB_R5G5B5PC:
    case RGBFB_B5G6R5PC:
    case RGBFB_B5G5R5PC:
	return 2;
    default:
	P96TRACE (("P96: ERROR - GetBytesPerPixel() was unsuccessful with 0x%x?!\n", RGBfmt));
	return 0;
    }
}

/*
 * Amiga <-> native structure conversion functions
 */
static int CopyRenderInfoStructureA2U (uaecptr amigamemptr,
				       struct RenderInfo *ri)
{
    uaecptr memp = get_long (amigamemptr + PSSO_RenderInfo_Memory);

    if (valid_address (memp, PSSO_RenderInfo_sizeof)) {
	ri->AMemory     = memp;
	ri->Memory      = get_real_address (memp);
	ri->BytesPerRow = get_word (amigamemptr + PSSO_RenderInfo_BytesPerRow);
	ri->RGBFormat   = get_long (amigamemptr + PSSO_RenderInfo_RGBFormat);
	return 1;
    }
    write_log ("P96: ERROR - Invalid RenderInfo memory area.\n");
    return 0;
}

static int CopyPatternStructureA2U (uaecptr amigamemptr,
				    struct Pattern *pattern)
{
    uaecptr memp = get_long (amigamemptr + PSSO_Pattern_Memory);

    if (valid_address (memp, PSSO_Pattern_sizeof)) {
	pattern->Memory   = get_real_address (memp);
	pattern->XOffset  = get_word (amigamemptr + PSSO_Pattern_XOffset);
	pattern->YOffset  = get_word (amigamemptr + PSSO_Pattern_YOffset);
	pattern->FgPen    = get_long (amigamemptr + PSSO_Pattern_FgPen);
	pattern->BgPen    = get_long (amigamemptr + PSSO_Pattern_BgPen);
	pattern->Size     = get_byte (amigamemptr + PSSO_Pattern_Size);
	pattern->DrawMode = get_byte (amigamemptr + PSSO_Pattern_DrawMode);
	return 1;
    }
    write_log ("P96: ERROR - Invalid Pattern memory area.\n");
    return 0;
}

static void CopyColorIndexMappingA2U (uaecptr amigamemptr,
				      struct ColorIndexMapping *cim)
{
    int i;
    cim->ColorMask = get_long (amigamemptr);
    for (i = 0; i < 256; i++, amigamemptr += 4)
	cim->Colors[i] = get_long (amigamemptr + 4);
}

static int CopyBitMapStructureA2U (uaecptr amigamemptr, struct BitMap *bm)
{
    int i;

    bm->BytesPerRow = get_word (amigamemptr + PSSO_BitMap_BytesPerRow);
    bm->Rows        = get_word (amigamemptr + PSSO_BitMap_Rows);
    bm->Flags       = get_byte (amigamemptr + PSSO_BitMap_Flags);
    bm->Depth       = get_byte (amigamemptr + PSSO_BitMap_Depth);

    if (bm->Depth > 8) {
	write_log ("P96: WARNING - Can't handle %d-bit deep bitmap\n",
		   bm->Depth);
	return 0;
    }

    for (i = 0; i < bm->Depth; i++) {
	uaecptr plane = get_long (amigamemptr + PSSO_BitMap_Planes + i * 4);
	switch (plane) {
	case 0:
	    bm->Planes[i] = &all_zeros_bitmap;
	    break;
	case 0xFFFFFFFF:
	    bm->Planes[i] = &all_ones_bitmap;
	    break;
	default:
	    if (valid_address (plane, bm->BytesPerRow * bm->Rows))
		bm->Planes[i] = get_real_address (plane);
	    else
		return 0;
	    break;
	}
    }
    return 1;
}

static int CopyTemplateStructureA2U (uaecptr amigamemptr,
				     struct Template *tmpl)
{
    uaecptr memp = get_long (amigamemptr + PSSO_Template_Memory);

    if (valid_address (memp, 1 /* FIXME */ )) {
	tmpl->Memory      = get_real_address (memp);
	tmpl->BytesPerRow = get_word (amigamemptr + PSSO_Template_BytesPerRow);
	tmpl->XOffset     = get_byte (amigamemptr + PSSO_Template_XOffset);
	tmpl->DrawMode    = get_byte (amigamemptr + PSSO_Template_DrawMode);
	tmpl->FgPen       = get_long (amigamemptr + PSSO_Template_FgPen);
	tmpl->BgPen       = get_long (amigamemptr + PSSO_Template_BgPen);
	return 1;
    }
    write_log ("P96: ERROR - Invalid Template memory area.\n");
    return 0;
}

static void CopyLibResolutionStructureU2A (struct LibResolution *libres,
					   uaecptr amigamemptr)
{
    uae_u8 *uaememptr = 0;
    int i;

    /* I know that amigamemptr is inside my gfxmem chunk, so I can just
     * do the xlate() */
    uaememptr = gfxmem_xlate (amigamemptr);

    /* zero out our LibResolution structure */
    memset (uaememptr, 0, PSSO_LibResolution_sizeof);

    strcpy   ((char*)uaememptr   + PSSO_LibResolution_P96ID, libres->P96ID);
    put_long (amigamemptr + PSSO_LibResolution_DisplayID, libres->DisplayID);
    put_word (amigamemptr + PSSO_LibResolution_Width, libres->Width);
    put_word (amigamemptr + PSSO_LibResolution_Height, libres->Height);
    put_word (amigamemptr + PSSO_LibResolution_Flags, libres->Flags);
    for (i = 0; i < MAXMODES; i++)
	put_long (amigamemptr + PSSO_LibResolution_Modes + i * 4, libres->Modes[i]);
#if 0
    put_long (amigamemptr, libres->Node.ln_Succ);
    put_long (amigamemptr + 4, libres->Node.ln_Pred);
    put_byte (amigamemptr + 8, libres->Node.ln_Type);
    put_byte (amigamemptr + 9, libres->Node.ln_Pri);
#endif
    put_long (amigamemptr + 10, amigamemptr + PSSO_LibResolution_P96ID);
    put_long (amigamemptr + PSSO_LibResolution_BoardInfo, libres->BoardInfo);
}

/* l is Amiga address of list, in correct endian format for UAE
 * n is Amiga address of node, in correct endian format for UAE */
static void AmigaListAddTail (uaecptr l, uaecptr n)
{
    put_long (n + 0, l + 4);		// n->ln_Succ = (struct Node *)&l->lh_Tail;
    put_long (n + 4, get_long (l + 8)); // n->ln_Pred = l->lh_TailPred;
    put_long (get_long (l + 8) + 0, n); // l->lh_TailPred->ln_Succ = n;
    put_long (l + 8, n);		// l->lh_TailPred = n;
}

/*
 * Functions to perform an action on the real screen
 */

/* v16: deferred screen update (defined below) */
static void mark_dirty_rect (int x, int y, int width, int height);
static void p96_flush_dirty (void);

/*
 * Fill a rectangle on the screen.  src points to the start of a line of the
 * filled rectangle in the frame buffer; it can be used as a memcpy source if
 * there is no OS specific function to fill the rectangle.
 */
static void do_fillrect (uae_u8 *src, int x, int y, int width, int height,
			 uae_u32 pen, int Bpp, RGBFTYPE rgbtype)
{
    P96TRACE(("P96: do_fillrect (src:%08x x:%d y:%d w:%d h:%d pen:%08x)\n",
	       src, x, y, width, height, pen));

    /* Try OS specific fillrect function here; and return if successful. Make
     * sure we adjust for the pen values if we're doing 8-bit
     * display-emulation on a 16-bit or higher screen. */

	if (picasso_vidinfo.rgbformat == picasso96_state.RGBFormat)
	{
		if (DX_Fill (x, y, width, height, pen, rgbtype)) return;
	}
	else
	{
		output_update_clut();

		if (DX_Fill (x, y, width, height, picasso_vidinfo.clut[src[0]], rgbtype))
		return;
	}

	P96TRACE (("P96: WARNING - do_fillrect() using fall-back routine!\n"));

	/* v16: copied from the framebuffer with the other changed lines at
	 * the end of the frame (the fill is already in the framebuffer) */
	mark_dirty_rect (x, y, width, height);
}

/*
 * This routine modifies the real screen buffer after a blit has been
 * performed in the save area. If can_do_blit is nonzero, the blit can
 * be performed within the real screen buffer; otherwise, this routine
 * must do it by hand using the data in the save area, pointed to by
 * srcp.
 */
static void do_blit (struct RenderInfo *ri, int Bpp, int srcx, int srcy,
		     int dstx, int dsty, int width, int height,
		     BLIT_OPCODE opcode, int can_do_blit)
{
	int xoff = picasso96_state.XOffset;
	int yoff = picasso96_state.YOffset;

debug_crashed("%s:%d\n",__FUNCTION__,__LINE__);

	// Clipping. 

	dstx -= xoff;
	dsty -= yoff;

	if (srcy < yoff || srcx < xoff
		|| srcx - xoff + width > picasso96_state.Width
		|| srcy - yoff + height > picasso96_state.Height)
	{
		can_do_blit = 0;
	}

	if (dstx < 0)
	{
		srcx -= dstx;
		width += dstx;
		dstx = 0;
	}

	if (dsty < 0)
	{
		srcy -= dsty;
		height += dsty;
		dsty = 0;
	}

	if (dstx + width > picasso96_state.Width) width = picasso96_state.Width - dstx;
	if (dsty + height > picasso96_state.Height) height = picasso96_state.Height - dsty;
	if (width <= 0 || height <= 0) return;

	// If this RenderInfo points at something else than the currently visible
	// screen, we must ignore the blit.

	if (can_do_blit)
	{
		// Call OS blitting function that can do it in video memory.
		// Should return if it was successful
		// v16: the host bitmap is the source, so the changed lines that
		// are only remembered so far must be in it first.

		p96_flush_dirty ();
		if (DX_Blit (srcx, srcy, dstx, dsty, width, height, opcode)) return;
	}

	// v16: no host blit - the changed area is copied from the framebuffer
	// with the other changed lines at the end of the frame (one lock).
	mark_dirty_rect (dstx, dsty, width, height);
}

/*
 * Invert a rectangle on the screen.
 */

static void do_invertrect (struct RenderInfo *ri, int Bpp, int x, int y,
			   int width, int height)
{
#if 0
    /* Clipping.  */
    x -= picasso96_state.XOffset;
    y -= picasso96_state.YOffset;
    if (x < 0) {
	width += x;
	x = 0;
    }
    if (y < 0) {
	height += y;
	y = 0;
    }
    if (x + width > picasso96_state.Width)
	width = picasso96_state.Width - x;
    if (y + height > picasso96_state.Height)
	height = picasso96_state.Height - y;

    if (width <= 0 || height <= 0)
	return;

#endif
    /* TODO: Try OS specific invertrect function here; and return if successful.  */

    do_blit (ri, Bpp, x, y, x, y, width, height, BLIT_SRC, 0);
}



/*
 * Successive writes to the P96 framebuffer frequently occur in the same line. We
 * optimize for this case by not flushing each write immediately to the screen.
 * Instead, we delay until data is written to a different framebuffer line.
 * Then we flush the line containing the previous data written.
 */

/* When successive writes to the framebuffer occur on the same line, we need to
 * remember these details about that line. Note: these are address in Amiga
 * memory space.
 */
static uaecptr currline_start;   /* start address of the line in the framebuffer */
static uaecptr currline_end;     /* end address of the line in the framebuffer plus one */
static uaecptr currline_min;     /* address of the left-most byte modified */
static uaecptr currline_max;     /* address of the right-most byte modfied plus one */
static int     currline_y;       /* row number of this line */


/*
 * v16: deferred screen update.
 *
 * Before, every change of the framebuffer line (CPU writes through
 * gfxmem_*put, flush_write) locked the host bitmap, copied that one line
 * and unlocked it again - for a window being drawn line by line that is a
 * LockBitMap()/UnlockBitMap() pair per line. Now the changed byte range of
 * each visible line is only remembered (dirty_lo/dirty_hi) and all changed
 * lines are copied with a single lock once per frame (picasso_handle_vsync),
 * or earlier when a host blit (DX_Blit) needs the host bitmap up to date.
 * The Amiga framebuffer (gfxmemory) stays the only source of the picture,
 * so copying late always gives the current contents.
 */
static uae_u16 *dirty_lo, *dirty_hi;  /* changed bytes [lo, hi) per visible line, hi == 0: clean */
static int      dirty_alloc;          /* lines in the arrays */
static int      dirty_first = 1, dirty_last = 0;   /* first > last: nothing changed */

static void dirty_clear (void)
{
    int y;
    for (y = dirty_first; y <= dirty_last && y < dirty_alloc; y++)
	dirty_lo[y] = dirty_hi[y] = 0;
    dirty_first = 1;
    dirty_last = 0;
}

/* line: visible line (0 = top of the shown screen), bytes [lo, hi) of it */
static void mark_dirty (int line, int lo, int hi)
{
    if (line < 0 || line >= picasso96_state.Height || hi <= lo)
	return;
    if (picasso96_state.Height > dirty_alloc) {
	int n = picasso96_state.Height + 64;
	uae_u16 *l = (uae_u16 *) calloc (n, sizeof (uae_u16));
	uae_u16 *h = (uae_u16 *) calloc (n, sizeof (uae_u16));
	if (!l || !h) {
	    free (l);
	    free (h);
	    return;
	}
	if (dirty_lo && dirty_first <= dirty_last) {
	    memcpy (l, dirty_lo, dirty_alloc * sizeof (uae_u16));
	    memcpy (h, dirty_hi, dirty_alloc * sizeof (uae_u16));
	}
	free (dirty_lo);
	free (dirty_hi);
	dirty_lo = l;
	dirty_hi = h;
	dirty_alloc = n;
    }
    if (hi > 0xffff)
	hi = 0xffff;
    if (dirty_hi[line] == 0) {
	dirty_lo[line] = lo;
	dirty_hi[line] = hi;
    } else {
	if (lo < dirty_lo[line])
	    dirty_lo[line] = lo;
	if (hi > dirty_hi[line])
	    dirty_hi[line] = hi;
    }
    if (dirty_first > dirty_last) {
	dirty_first = dirty_last = line;
    } else {
	if (line < dirty_first)
	    dirty_first = line;
	if (line > dirty_last)
	    dirty_last = line;
    }
}

/* rectangle in visible pixels */
static void mark_dirty_rect (int x, int y, int width, int height)
{
    int Bpp = picasso96_state.BytesPerPixel;
    int i;
    for (i = 0; i < height; i++)
	mark_dirty (y + i, x * Bpp, (x + width) * Bpp);
}

/*
 * Write one line from the framebuffer to the locked host bitmap
 *
 * dstbase    = locked host bitmap (gfx_lock_picasso)
 * srcp       = Pointer to first byte in the source line to write
 * line_no    = Number of line to write (relative to visible screen).
 * first_byte = Offset in bytes from start of line to first byte to write.
 * byte_count = Number of bytes to write from source line.
 */
STATIC_INLINE void write_line_locked (uae_u8 *dstbase, uae_u8 *srcp, int line_no, int first_byte, int byte_count)
{
	uae_u8 *dstp = dstbase;
	int Bpp = GetBytesPerPixel (picasso_vidinfo.rgbformat);

	if (picasso_vidinfo.rgbformat == picasso96_state.RGBFormat)
	{
		dstp += line_no * picasso_vidinfo.rowbytes + first_byte;

		if (need_argb32_hack && Bpp == 4)
			memcpy_bswap32 (dstp, srcp, byte_count);
		else
			memcpy (dstp, srcp, byte_count);
	}
	else
	{
		dstp += line_no * picasso_vidinfo.rowbytes + first_byte * Bpp;

		switch (Bpp)
		{
			case 2:
				{
					int i;
					uae_u16 *dstp16 = (uae_u16*) dstp;
					for (i = 0; i < byte_count; i++)
						*dstp16++ = picasso_vidinfo.clut[srcp[i]];
				}
				break;

			case 4:
				{
					int i;
					for (i = 0; i < byte_count; i++)
						*((uae_u32 *) dstp + i) = picasso_vidinfo.clut[srcp[i]];
				}
				break;
		}
	}
}

/* Copy all changed lines to the host bitmap: one lock for all of them */
static void p96_flush_dirty (void)
{
    uae_u8 *dst, *fb;
    int y, last, fb_bpp, maxbytes;

    if (dirty_first > dirty_last)
	return;
    if (!picasso_on || !gfxmemory || !picasso96_state.Address) {
	dirty_clear ();
	return;
    }
    last = dirty_last;
    if (last >= picasso96_state.Height)
	last = picasso96_state.Height - 1;
    for (y = dirty_first; y <= last; y++)
	if (dirty_hi[y])
	    DX_Invalidate (y, y);
    if (!picasso_vidinfo.extra_mem) {
	dirty_clear ();
	return;
    }

    fb_bpp   = picasso96_state.BytesPerPixel;
    maxbytes = picasso96_state.Width * fb_bpp;
    fb = gfxmemory + (picasso96_state.Address - gfxmem_start)
	 + picasso96_state.YOffset * picasso96_state.BytesPerRow
	 + picasso96_state.XOffset * fb_bpp;

    if (picasso_vidinfo.rgbformat != picasso96_state.RGBFormat)
	output_update_clut ();
    dst = gfx_lock_picasso ();
    if (dst) {
	for (y = dirty_first; y <= last; y++) {
	    int lo = dirty_lo[y], hi = dirty_hi[y];
	    if (!hi)
		continue;
	    if (hi > maxbytes)
		hi = maxbytes;
	    if (hi > lo)
		write_line_locked (dst, fb + y * picasso96_state.BytesPerRow + lo, y, lo, hi - lo);
	}
	gfx_unlock_picasso ();
    }
    dirty_clear ();
}

/* The current line of successive CPU writes goes into the dirty lines */
static void flush_currline (void)
{
    int line_no = currline_y - picasso96_state.YOffset;

    /* We only need this line if it is actually visible on screen */
    if (line_no >= 0 && line_no < picasso96_state.Height) {
	uae_u8 fb_bpp  = picasso96_state.BytesPerPixel;
	int byte_count = (currline_max - currline_min);
	int first_byte = currline_min - currline_start
			 - (picasso96_state.XOffset * fb_bpp);

	if (first_byte < 0) {
	    byte_count += first_byte;
	    first_byte = 0;
	}
	if ((first_byte + byte_count) > (picasso96_state.Width * fb_bpp))
	    byte_count = picasso96_state.Width * fb_bpp - first_byte;

	if (byte_count > 0)
	    mark_dirty (line_no, first_byte, first_byte + byte_count);
    }
    currline_start = 0xFFFFFFFF;
}

/* Only remember the pending CPU line (no host bitmap access) - used by the
 * blit functions; do_blit() flushes before a host blit */
STATIC_INLINE void wgfx_mergeline (void)
{
    if (currline_start != 0xFFFFFFFF && picasso_on)
	flush_currline ();
}

/* Bring the host bitmap up to date (frame end, panning) */
STATIC_INLINE void wgfx_flushline (void)
{
    wgfx_mergeline ();
    p96_flush_dirty ();
}

STATIC_INLINE int renderinfo_is_current_screen (struct RenderInfo *ri)
{
    if (!picasso_on)
	return 0;
    if (ri->Memory != gfxmemory + (picasso96_state.Address - gfxmem_start))
	return 0;

    return 1;
}

/* Clear our screen, since we've got a new Picasso screen-mode, and refresh
 * with the proper contents. This is called on several occasions:
 * 1. Amiga-->Picasso transition, via SetSwitch()
 * 2. Picasso-->Picasso transition, via SetPanning().
 * 3. whenever the graphics code notifies us that the screen contents have
 *    been lost.
 */
extern unsigned int new_beamcon0;



void picasso_refresh (int call_setpalette)
{
	struct RenderInfo ri;

	if (!picasso_on) return;

	{ // for higher P96 mousedraw rate
		extern uae_u16 vtotal;
		if (p96hack_vpos2)
		{
			vtotal = p96hack_vpos2;
			new_beamcon0 |= 0x80;
			p96refresh_active=1;
		}
		else new_beamcon0 |= 0x20;
	}

#ifdef JIT
    have_done_picasso=1;
#endif

	// Make sure that the first time we show a Picasso video mode, we don't
	// blit any crap. We can do this by checking if we have an Address yet. 

	if (picasso96_state.Address)
	{
		unsigned int width, height;

		/* blit the stuff from our static frame-buffer to the gfx-card */

		ri.Memory = gfxmemory + (picasso96_state.Address - gfxmem_start);
		ri.BytesPerRow = picasso96_state.BytesPerRow;
		ri.RGBFormat = picasso96_state.RGBFormat;

		if (set_panning_called)
		{
			width = picasso96_state.VirtualWidth;
			height = picasso96_state.VirtualHeight;
		}
		else
		{
			width = picasso96_state.Width;
			height = picasso96_state.Height;
		}

		do_blit (&ri, picasso96_state.BytesPerPixel, 0, 0, 0, 0, width, height, BLIT_SRC, 0);
	}
	else
		write_log ("P96: ERROR - picasso_refresh() can't refresh!\n");

}

/*
* Functions to perform an action on the frame-buffer
*/
STATIC_INLINE void do_blitrect_frame_buffer (struct RenderInfo *ri,
					     struct RenderInfo *dstri,
					     unsigned long srcx, unsigned long srcy,
					     unsigned long dstx, unsigned long dsty,
					     unsigned long width, unsigned long height,
					     uae_u8 mask, BLIT_OPCODE opcode)
{
    uae_u8 *src, *dst, *tmp, *tmp2, *tmp3;
    uae_u8 Bpp = GetBytesPerPixel (ri->RGBFormat);
    unsigned long total_width = width * Bpp;
    unsigned long linewidth = (total_width + 15) & ~15;
    unsigned long lines;
/*    int can_do_visible_blit = 0; */

    src = ri->Memory + srcx*Bpp + srcy*ri->BytesPerRow;
    dst = dstri->Memory + dstx*Bpp + dsty*dstri->BytesPerRow;

    if (mask != 0xFF && Bpp > 1) {
	P96TRACE (("P96: WARNING - BlitRect() has mask 0x%x with Bpp %d.\n", mask, Bpp));
    }

    if (mask == 0xFF || Bpp > 1) {
	if( opcode == BLIT_SRC ) {
	    /* handle normal case efficiently */
	    if (ri->Memory == dstri->Memory && dsty == srcy) {
		unsigned long i;
		for (i = 0; i < height; i++, src += ri->BytesPerRow, dst += dstri->BytesPerRow)
		    memmove (dst, src, total_width);
	    } else if (dsty < srcy) {
		unsigned long i;
		for (i = 0; i < height; i++, src += ri->BytesPerRow, dst += dstri->BytesPerRow)
		    memcpy (dst, src, total_width);
	    } else {
		unsigned long i;
		src += (height-1) * ri->BytesPerRow;
		dst += (height-1) * dstri->BytesPerRow;
		for (i = 0; i < height; i++, src -= ri->BytesPerRow, dst -= dstri->BytesPerRow)
		    memcpy (dst, src, total_width);
	    }
	    return;
	} else {
	    uae_u8  *src2 = src;
	    uae_u8  *dst2 = dst;
	    uae_u32 *src2_32 = (uae_u32*)src;
	    uae_u32 *dst2_32 = (uae_u32*)dst;
	    unsigned int y;

	    for (y = 0; y < height; y++) {
		uae_u32 *bound = (uae_u32 *)(src + total_width - 4);
		    //copy now the longs
		    for (src2_32 = (uae_u32*)src, dst2_32 = (uae_u32*)dst; src2_32 < bound; src2_32++, dst2_32++ ) {
			switch ((uae_u8) opcode) {
			    case BLIT_FALSE:
				*dst2_32 = 0;
				break;
			    case BLIT_NOR:
				*dst2_32 = ~(*src2_32 | *dst2_32);
				break;
			    case BLIT_ONLYDST:
				*dst2_32 = *dst2_32 & ~(*src2_32);
				break;
			    case BLIT_NOTSRC:
				*dst2_32 = ~(*src2_32);
				break;
			    case BLIT_ONLYSRC:
				*dst2_32 = *src2_32 & ~(*dst2_32);
				break;
			    case BLIT_NOTDST:
				*dst2_32 = ~(*dst2_32);
				break;
			    case BLIT_EOR:
				*dst2_32 = *src2_32 ^ *dst2_32;
				break;
			    case BLIT_NAND:
				*dst2_32 = ~(*src2_32 & *dst2_32);
				break;
			    case BLIT_AND:
				*dst2_32 = *src2_32 & *dst2_32;
				break;
			    case BLIT_NEOR:
				*dst2_32 = ~(*src2_32 ^ *dst2_32);
				break;
			    case BLIT_DST:
				write_log ("P96: ERROR - do_blitrect_frame_buffer shouldn't get BLIT_DST!\n");
				break;
			    case BLIT_NOTONLYSRC:
				*dst2_32 = ~(*src2_32) | *dst2_32;
				break;
			    case BLIT_SRC:
				write_log ("P96: ERROR - do_blitrect_frame_buffer shouldn't get BLIT_SRC!\n");
				break;
			    case BLIT_NOTONLYDST:
				*dst2_32 = ~(*dst2_32) | *src2_32;
				break;
			    case BLIT_OR:
				*dst2_32 = *src2_32 | *dst2_32;
				break;
			    case BLIT_TRUE:
				*dst2_32 = 0xFFFFFFFF;
				break;
			    case 30: {
				/* code for swap source with dest in byte */
				uae_u32 temp;
				temp     = *src2_32;
				*src2_32 = *dst2_32;
				*dst2_32 = temp;
				break;
			    }
			    case BLIT_LAST:
				write_log ( "P96: ERROR - do_blitrect_frame_buffer shouldn't get BLIT_LAST!\n");
				break;
			} /* switch opcode */
		    } // for end
		    //now copy the rest few bytes
		    for (src2 = (uae_u8*)src2_32, dst2 = (uae_u8*)dst2_32; src2 < src + total_width; src2++, dst2++ ) {
			switch ((uae_u8)opcode) {
			    case BLIT_FALSE:
				*dst2 = 0;
				break;
			    case BLIT_NOR:
				*dst2 = ~(*src2 | *dst2);
				break;
			    case BLIT_ONLYDST:
				*dst2 = *dst2 & ~(*src2);
				break;
			    case BLIT_NOTSRC:
				*dst2 = ~(*src2);
				break;
			    case BLIT_ONLYSRC:
				*dst2 = *src2 & ~(*dst2);
				break;
			    case BLIT_NOTDST:
				*dst2 = ~(*dst2);
				break;
			    case BLIT_EOR:
				*dst2 = *src2 ^ *dst2;
				break;
			    case BLIT_NAND:
				*dst2 = ~(*src2 & *dst2);
				break;
			    case BLIT_AND:
				*dst2 = *src2 & *dst2;
				break;
			    case BLIT_NEOR:
				*dst2 = ~(*src2 ^ *dst2);
				break;
			    case BLIT_DST:
				write_log ("P96: ERROR - do_blitrect_frame_buffer shouldn't get BLIT_DST!\n");
				break;
			    case BLIT_NOTONLYSRC:
				*dst2 = ~(*src2) | *dst2;
				break;
			    case BLIT_SRC:
				write_log ("P96: ERROR - do_blitrect_frame_buffer shouldn't get BLIT_SRC!\n");
				break;
			    case BLIT_NOTONLYDST:
				*dst2 = ~(*dst2) | *src2;
				break;
			    case BLIT_OR:
				*dst2 = *src2 | *dst2;
				break;
			    case BLIT_TRUE:
				*dst2 = 0xFF;
				break;
			    case BLIT_LAST:
				write_log ("P96: ERROR - do_blitrect_frame_buffer shouldn't get BLIT_LAST!\n");
				break;
			    case 30: {
				/* code for swap source with dest in long */
				uae_u8 temp;
				temp  = *src2;
				*src2 = *dst2;
				*dst2 = temp;
				break;
			    }
			} /* switch opcode */
		    } /* for width */
		src += ri->BytesPerRow;
		dst += dstri->BytesPerRow;
	    } /* for height */
	}
	return;
    }
    // (mask != 0xFF && Bpp <= 1)
    tmp3 = tmp2 = tmp = xmalloc (linewidth * height); /* allocate enough memory for the src-rect */
    if (!tmp)
	return;

    /* copy the src-rect into our temporary buffer space */
    for (lines = 0; lines < height; lines++, src += ri->BytesPerRow, tmp2 += linewidth)
	memcpy (tmp2, src, total_width);

    /* copy the temporary buffer to the destination */
    for (lines = 0; lines < height; lines++, dst += dstri->BytesPerRow, tmp += linewidth) {
	unsigned long cols;
	for (cols = 0; cols < width; cols++) {
	    dst[cols] &= ~mask;
	    dst[cols] |= tmp[cols] & mask;
	}
    }
    /* free the temp-buf */
    free (tmp3);
}

/*
 * BOOL FindCard(struct BoardInfo *bi);       and
 *
 * FindCard is called in the first stage of the board initialisation and
 * configuration and is used to look if there is a free and unconfigured
 * board of the type the driver is capable of managing. If it finds one,
 * it immediately reserves it for use by Picasso96, usually by clearing
 * the CDB_CONFIGME bit in the flags field of the ConfigDev struct of
 * this expansion card. But this is only a common example, a driver can
 * do whatever it wants to mark this card as used by the driver. This
 * mechanism is intended to ensure that a board is only configured and
 * used by one driver. FindBoard also usually fills some fields of the
 * BoardInfo struct supplied by the caller, the rtg.library, for example
 * the MemoryBase, MemorySize and RegisterBase fields.
 */
uae_u32 REGPARAM2 picasso_FindCard (struct regstruct *regs)
{
    uaecptr AmigaBoardInfo = m68k_areg (regs, 0);
    /* NOTES: See BoardInfo struct definition in Picasso96 dev info */

    if (allocated_gfxmem && !picasso96_state.CardFound) {
	/* Fill in MemoryBase, MemorySize */
	put_long (AmigaBoardInfo + PSSO_BoardInfo_MemoryBase, gfxmem_start);
	/* size of memory, minus a 32K chunk: 16K for pattern bitmaps, 16K for
	 * resolution list */
	put_long (AmigaBoardInfo + PSSO_BoardInfo_MemorySize,
		  allocated_gfxmem - 32768);

	picasso96_state.CardFound = 1;	/* mark our "card" as being found */
	return -1;
    } else
	return 0;
}

static void FillBoardInfo (uaecptr amigamemptr, struct LibResolution *res,
			   struct PicassoResolution *dm)
{
    uae_u8 *uaememptr;
    switch (dm->depth) {
    case 1:
	res->Modes[CHUNKY] = amigamemptr;
	break;
    case 2:
	res->Modes[HICOLOR] = amigamemptr;
	break;
    case 3:
	res->Modes[TRUECOLOR] = amigamemptr;
	break;
    default:
	res->Modes[TRUEALPHA] = amigamemptr;
	break;
    }
    /* I know that amigamemptr is inside my gfxmem chunk, so I can just do
     * the xlate() */
    uaememptr = gfxmem_xlate (amigamemptr);

    /* zero out our ModeInfo struct */
    memset (uaememptr, 0, PSSO_ModeInfo_sizeof);

    put_word (amigamemptr + PSSO_ModeInfo_Width, dm->res.width);
    put_word (amigamemptr + PSSO_ModeInfo_Height, dm->res.height);
    put_byte (amigamemptr + PSSO_ModeInfo_Depth, dm->depth * 8);
    put_byte (amigamemptr + PSSO_ModeInfo_Flags, 0);
    put_word (amigamemptr + PSSO_ModeInfo_HorTotal, dm->res.width);
    put_word (amigamemptr + PSSO_ModeInfo_HorBlankSize, 0);
    put_word (amigamemptr + PSSO_ModeInfo_HorSyncStart, 0);
    put_word (amigamemptr + PSSO_ModeInfo_HorSyncSize, 0);
    put_byte (amigamemptr + PSSO_ModeInfo_HorSyncSkew, 0);
    put_byte (amigamemptr + PSSO_ModeInfo_HorEnableSkew, 0);

    put_word (amigamemptr + PSSO_ModeInfo_VerTotal, dm->res.height);
    put_word (amigamemptr + PSSO_ModeInfo_VerBlankSize, 0);
    put_word (amigamemptr + PSSO_ModeInfo_VerSyncStart, 0);
    put_word (amigamemptr + PSSO_ModeInfo_VerSyncSize, 0);

    put_byte (amigamemptr + PSSO_ModeInfo_first_union, 98);
    put_byte (amigamemptr + PSSO_ModeInfo_second_union, 14);

    put_long (amigamemptr + PSSO_ModeInfo_PixelClock,
	      dm->res.width * dm->res.height * dm->refresh);
}

struct modeids {
    int width, height;
    int id;
};
static const struct modeids mi[] =
{
    /* "original" modes */

    {  320, 200, 0 },
    {  320, 240, 1 },
    {  640, 400, 2 },
    {  640, 480, 3 },
    {  800, 600, 4 },
    { 1024, 768, 5 },
    { 1152, 864, 6 },
    { 1280,1024, 7 },
    { 1600,1280, 8 },

    /* new modes */
 
    {  704, 480, 129 },
    {  704, 576, 130 },
    {  720, 480, 131 },
    {  720, 576, 132 },
    {  768, 483, 133 },
    {  768, 576, 134 },
    {  800, 480, 135 },
    {  848, 480, 136 },
    {  854, 480, 137 },
    {  948, 576, 138 },
    { 1024, 576, 139 },
    { 1152, 768, 140 },
    { 1152, 864, 141 },
    { 1280, 720, 142 },
    { 1280, 768, 143 },
    { 1280, 800, 144 },
    { 1280, 854, 145 },
    { 1280, 960, 146 },
    { 1366, 768, 147 },
    { 1440, 900, 148 },
    { 1440, 960, 149 },
    { 1600,1200, 150 },
    { 1680,1050, 151 },
    { 1920,1080, 152 },
    { 1920,1200, 153 },
    { 2048,1152, 154 },
    { 2048,1536, 155 },
    { 2560,1600, 156 },
    { 2560,2048, 157 },
    {  400, 300, 158 },
    {  512, 384, 159 },
    {  640, 432, 160 },
    { 1360, 768, 161 },
    { 1360,1024, 162 },
    { 1400,1050, 163 },
    { 1792,1344, 164 },
    { 1800,1440, 165 },
    { 1856,1392, 166 },
    { 1920,1440, 167 },
    {  480, 360, 168 },
    {  640, 350, 169 },
    { 1600, 900, 170 },
    {  960, 600, 171 },
    { 1088, 612, 172 },
    {   -1,  -1, 0 }
};

static uae_u32 AssignModeID (int w, int h, unsigned int *non_standard_count)
{
    unsigned int i;

    for (i = 0; mi[i].width > 0; i++) {
	if (w == mi[i].width && h == mi[i].height)
	    return 0x50001000 | (mi[i].id * 0x10000);
    }
    (*non_standard_count)++;
    write_log ("P96: Non-stanard mode %dx%d\n", w, h);
    return 0x51001000 - (*non_standard_count) * 0x10000;
}

/****************************************
* InitCard()
*
* a2: BoardInfo structure ptr - Amiga-based address in Intel endian-format
*
* Job - fill in the following structure members:
* gbi_RGBFormats: the pixel formats that the host-OS of UAE supports
*     If UAE is running in a window, it should ONLY report the pixel format of
*     the host-OS desktop.
*     If UAE is running full-screen, it should report ALL pixel formats that
*     the host-OS can handle in full-screen.
*     NOTE: If full-screen, and the user toggles to windowed-mode, all hell
*     will break loose visually.  Must inform user that they're doing
*     something stupid (unless their desktop and full-screen colour modes
*     match).
* gbi_SoftSpriteFlags: should be the same as above for now, until actual
*     cursor support is added
* gbi_BitsPerCannon: could be 6 or 8 or ???, depending on the host-OS gfx-card
* gbi_MaxHorResolution: fill this in for all modes (even if you don't support
*     them).
* gbi_MaxVerResolution: fill this in for all modes (even if you don't support
*     them).
*/
static void init_card_modes (uaecptr AmigaBoardInfo)
{
    struct LibResolution res;
    int i;
    unsigned int non_standard_count = 0;
    int ModeInfoStructureCount = 1, LibResolutionStructureCount = 0;
    uaecptr amigamemptr = 0;

    put_word (AmigaBoardInfo + PSSO_BoardInfo_BitsPerCannon,	    DX_BitsPerCannon ());
    put_word (AmigaBoardInfo + PSSO_BoardInfo_RGBFormats,	    picasso96_pixel_format);
    put_word (AmigaBoardInfo + PSSO_BoardInfo_SoftSpriteFlags,	    picasso96_pixel_format);
    put_long (AmigaBoardInfo + PSSO_BoardInfo_BoardType,	    BT_uaegfx);
    put_word (AmigaBoardInfo + PSSO_BoardInfo_MaxHorResolution + 0, planar.width);
    put_word (AmigaBoardInfo + PSSO_BoardInfo_MaxHorResolution + 2, chunky.width);
    put_word (AmigaBoardInfo + PSSO_BoardInfo_MaxHorResolution + 4, hicolour.width);
    put_word (AmigaBoardInfo + PSSO_BoardInfo_MaxHorResolution + 6, truecolour.width);
    put_word (AmigaBoardInfo + PSSO_BoardInfo_MaxHorResolution + 8, alphacolour.width);
    put_word (AmigaBoardInfo + PSSO_BoardInfo_MaxVerResolution + 0, planar.height);
    put_word (AmigaBoardInfo + PSSO_BoardInfo_MaxVerResolution + 2, chunky.height);
    put_word (AmigaBoardInfo + PSSO_BoardInfo_MaxVerResolution + 4, hicolour.height);
    put_word (AmigaBoardInfo + PSSO_BoardInfo_MaxVerResolution + 6, truecolour.height);
    put_word (AmigaBoardInfo + PSSO_BoardInfo_MaxVerResolution + 8, alphacolour.height);

    for (i = 0; i < mode_count;) {
	int j = i;
	/* Add a LibResolution structure to the ResolutionsList MinList in our BoardInfo */
	res.DisplayID = AssignModeID (DisplayModes[i].res.width,  DisplayModes[i].res.height, &non_standard_count);
	res.BoardInfo = AmigaBoardInfo;
	res.Width     = DisplayModes[i].res.width;
	res.Height    = DisplayModes[i].res.height;
	res.Flags     = P96F_PUBLIC;
	res.P96ID[0] = 'P';
	res.P96ID[1] = '9';
	res.P96ID[2] = '6';
	res.P96ID[3] = '-';
	res.P96ID[4] = '0';
	res.P96ID[5] = ':';
	strcpy  (res.Name, "uaegfx:");
	strncat (res.Name, DisplayModes[i].name,
		 strchr (DisplayModes[i].name, ',') - DisplayModes[i].name);
	res.Modes[PLANAR] = 0;
	res.Modes[CHUNKY] = 0;
	res.Modes[HICOLOR] = 0;
	res.Modes[TRUECOLOR] = 0;
	res.Modes[TRUEALPHA] = 0;

	do {
	    /* Handle this display mode's depth */
	    /* Only add the modes when there is enough P96 RTG memory to hold the bitmap */
	    unsigned long required = DisplayModes[i].res.width * DisplayModes[i].res.height
				     * DisplayModes[i].depth;
	    if (allocated_gfxmem - 32768 > required) {
		amigamemptr = gfxmem_start + allocated_gfxmem
			      - (PSSO_ModeInfo_sizeof * ModeInfoStructureCount++);
		FillBoardInfo (amigamemptr, &res, &DisplayModes[i]);
	    }
	    i++;
	} while (i < mode_count
		 && DisplayModes[i].res.width == DisplayModes[j].res.width
		 && DisplayModes[i].res.height == DisplayModes[j].res.height);

	amigamemptr = gfxmem_start + allocated_gfxmem - 16384
		      + (PSSO_LibResolution_sizeof * LibResolutionStructureCount++);

	CopyLibResolutionStructureU2A (&res, amigamemptr);
	DumpLibResolutionStructure (amigamemptr);

	AmigaListAddTail (AmigaBoardInfo + PSSO_BoardInfo_ResolutionsList, amigamemptr);
    }
}

/* InitCard of the external uaegfx.card (Picasso96 package), called through
 * uaelib function 29 with the BoardInfo in a2. The card sets its own
 * function pointers and flags. */
uae_u32 REGPARAM2 picasso_InitCard (struct regstruct *regs)
{
    init_card_modes (m68k_areg (regs, 2));
    return 0;
}

extern int x_size, y_size;

/*
 * SetSwitch:
 * a0:	struct BoardInfo
 * d0.w:	BOOL state
 * this function should set a board switch to let the Amiga signal pass
 * through when supplied with a 0 in d0 and to show the board signal if
 * a 1 is passed in d0. You should remember the current state of the
 * switch to avoid unneeded switching. If your board has no switch, then
 * simply supply a function that does nothing except a RTS.
 *
 * NOTE: Return the opposite of the switch-state. BDK
*/
uae_u32 REGPARAM2 picasso_SetSwitch (struct regstruct *regs)
{
    uae_u16 flag = m68k_dreg (regs, 0) & 0xFFFF;

    /* Do not switch immediately.  Tell the custom chip emulation about the
     * desired state, and wait for custom.c to call picasso_enablescreen
     * whenever it is ready to change the screen state.  */
    picasso_requested_on = !!flag;

    P96TRACE (("P96: SetSwitch() - trying to show %s screen\n", flag ? "picasso96" : "amiga"));

    flush_icache_hard ("picasso96 switch"); /* Changing the screen mode might make gfx memory
			 directly accessible, or no longer thus accessible */

    /* Put old switch-state in D0 */
    return !flag;
}

void picasso_enablescreen (int on)
{
    currline_start = 0xFFFFFFFF;
    dirty_clear ();             /* v16: picasso_refresh() marks everything */
    picasso_refresh (1);
}

static int first_color_changed = 256;
static int last_color_changed = -1;

void picasso_handle_vsync (void)
{
    if (first_color_changed < last_color_changed) {
	DX_SetPalette (first_color_changed,
		       last_color_changed - first_color_changed);
	/* If we're emulating a CLUT mode, we need to redraw the entire
	 * screen. */
	if (picasso_vidinfo.rgbformat != picasso96_state.RGBFormat)
	    picasso_refresh (1);
    }

    first_color_changed = 256;
    last_color_changed = -1;

#ifdef JIT
    /* v16: with comp_trust "direct"/"afterPic" a translated block may write
     * into the framebuffer directly (its history saw a different address),
     * past flush_write(). Copy the whole screen now and then so such writes
     * do not stay invisible. Not needed with "indirect" (the default). */
    if (picasso_on && currprefs.cachesize
	&& (currprefs.comptrustbyte != 1 || currprefs.comptrustword != 1 || currprefs.comptrustlong != 1)) {
	static int frames;
	if (++frames >= 50) {
	    frames = 0;
	    mark_dirty_rect (0, 0, picasso96_state.Width, picasso96_state.Height);
	}
    }
#endif

    /* Flush any cached changes to the screen: all changed lines with one
     * lock of the host bitmap (v16) */
    wgfx_flushline ();
}

void picasso_handle_hsync (void)
{
#if 0
    static int p96hsync;

    if (currprefs.gfxmem_size == 0)
	return;
    if (WIN32GFX_IsPicassoScreen () && currprefs.gfx_pfullscreen && currprefs.gfx_vsync) {
	if (DirectDraw_GetVerticalBlankStatus ())
	    p96hsync = 0;
    } else
	p96hsync--;

    if (p96hsync <= 0) {
	rtarea[get_long (RTAREA_BASE + 36) + 12 - 1]++;
	p96hsync = p96syncrate;
    }
#endif
}

void init_hz_p96 (void)
{
    int rate = 0;
    p96syncrate = maxvpos * vblank_hz;
#if 0
    if (isfullscreen ())
	rate = DirectDraw_CurrentRefreshRate ();
    else
	rate = currprefs.gfx_refreshrate;
#endif
    if (rate <= 0)
	rate = 60;
    p96syncrate /= rate;
}

void picasso_clip_mouse (int *px, int *py)
{
    int xoff = picasso96_state.XOffset;
    int yoff = picasso96_state.YOffset;

    if (*px < -xoff)
	*px = -xoff;
    if (*px + xoff > picasso_vidinfo.width)
	*px = picasso_vidinfo.width - xoff;
    if (*py < -yoff)
	*py = -yoff;
    if (*py + yoff > picasso_vidinfo.height)
	*py = picasso_vidinfo.height - yoff;
}

/*
 * SetColorArray:
 * a0: struct BoardInfo
 * d0.w: startindex
 * d1.w: count
 * when this function is called, your driver has to fetch "count" color
 * values starting at "startindex" from the CLUT field of the BoardInfo
 * structure and write them to the hardware. The color values are always
 * between 0 and 255 for each component regardless of the number of bits
 * per cannon your board has. So you might have to shift the colors
 * before writing them to the hardware.
 */

uae_u32 REGPARAM2 picasso_SetColorArray (struct regstruct *regs)
{
	// Fill in some static UAE related structure about this new CLUT setting.
	// We need this for CLUT-based displays, and for mapping CLUT to hi/true
	// colour 

	uaecptr boardinfo = m68k_areg (regs, 0);
	uae_u16 start     = m68k_dreg (regs, 0);
	uae_u16 count     = m68k_dreg (regs, 1);

	uaecptr clut = boardinfo + PSSO_BoardInfo_CLUT + start * 3;
	int changed = 0;
	int i;

	for (i = start; i < start + count; i++)
	{
		int r = get_byte (clut);
		int g = get_byte (clut + 1);
		int b = get_byte (clut + 2);

		changed |=   (picasso96_state.CLUT[i].Red   != r
		   || picasso96_state.CLUT[i].Green != g
		   || picasso96_state.CLUT[i].Blue  != b);

		picasso96_state.CLUT[i].Red   = r;
		picasso96_state.CLUT[i].Green = g;
		picasso96_state.CLUT[i].Blue  = b;

		clut += 3;
    	}

	if (changed)
	{
		output_clut_needs_update = true;	// we can't wait for vsync!!

		if (start < first_color_changed)
			first_color_changed = start;

		if (start + count > last_color_changed)
			last_color_changed = start + count;
	}

	return 1;
}

/*
 * SetDAC:
 * a0: struct BoardInfo
 * d7: RGBFTYPE RGBFormat
 * This function is called whenever the RGB format of the display changes,
 * e.g. from chunky to TrueColor. Usually, all you have to do is to set
 * the RAMDAC of your board accordingly.
 */
uae_u32 REGPARAM2 picasso_SetDAC (struct regstruct *regs)
{
    /* Fill in some static UAE related structure about this new DAC setting.
     * Lets us keep track of what pixel format the Amiga is thinking about in
     * our frame-buffer */
    P96TRACE (("P96: SetDAC()\n"));
    return 1;
}

static void init_picasso_screen (void)
{
    int width   = picasso96_state.Width;
    int height  = picasso96_state.Height;
    int vwidth  = picasso96_state.VirtualWidth;
    int vheight = picasso96_state.VirtualHeight;
    int xoff    = 0;
    int yoff    = 0;

    if (!set_gc_called)
	return;

    if (set_panning_called) {
	picasso96_state.Extent = picasso96_state.Address
				 + (picasso96_state.BytesPerRow * vheight);
	xoff = picasso96_state.XOffset;
	yoff = picasso96_state.YOffset;
    }

    gfx_set_picasso_modeinfo (width, height, picasso96_state.GC_Depth,
			      picasso96_state.RGBFormat);
    DX_SetPalette (0, 256);

    currline_start = 0xFFFFFFFF;
    dirty_clear ();             /* v16: old mode's lines; picasso_refresh() marks everything */
    picasso_refresh (1);
}

/*
 * SetGC:
 * a0: struct BoardInfo
 * a1: struct ModeInfo
 * d0: BOOL Border
 * This function is called whenever another ModeInfo has to be set. This
 * function simply sets up the CRTC and TS registers to generate the
 * timing used for that screen mode. You should not set the DAC, clocks
 * or linear start adress. They will be set when appropriate by their
 * own functions.
 */
uae_u32 REGPARAM2 picasso_SetGC (struct regstruct *regs)
{
    /* Fill in some static UAE related structure about this new ModeInfo
     * setting */
    uaecptr modeinfo = m68k_areg (regs, 1);

    picasso96_state.Width = get_word (modeinfo + PSSO_ModeInfo_Width);
    picasso96_state.VirtualWidth = picasso96_state.Width;	/* in case SetPanning doesn't get called */

    picasso96_state.Height = get_word (modeinfo + PSSO_ModeInfo_Height);
    picasso96_state.VirtualHeight = picasso96_state.Height;

    picasso96_state.GC_Depth = get_byte (modeinfo + PSSO_ModeInfo_Depth);
    picasso96_state.GC_Flags = get_byte (modeinfo + PSSO_ModeInfo_Flags);

    P96TRACE (("P96: SetGC (%d,%d,%d)\n", picasso96_state.Width, picasso96_state.Height,
	       picasso96_state.GC_Depth));

    set_gc_called = 1;		/* @@@ when do we need to reset this? */
    init_picasso_screen ();
    init_hz_p96 ();
    return 1;
}

/*
 * SetPanning:
 * a0: struct BoardInfo
 * a1: UBYTE *Memory
 * d0: uae_u16 Width
 * d1: WORD XOffset
 * d2: WORD YOffset
 * d7: RGBFTYPE RGBFormat
 * This function sets the view origin of a display which might also be
 * overscanned. In register a1 you get the start address of the screen
 * bitmap on the Amiga side. You will have to subtract the starting
 * address of the board memory from that value to get the memory start
 * offset within the board. Then you get the offset in pixels of the
 * left upper edge of the visible part of an overscanned display. From
 * these values you will have to calculate the LinearStartingAddress
 * fields of the CRTC registers.

 * NOTE: SetPanning() can be used to know when a Picasso96 screen is
 * being opened.  Better to do the appropriate clearing of the
 * background here than in SetSwitch() derived functions,
 * because SetSwitch() is not called for subsequent Picasso screens.
 */
uae_u32 REGPARAM2 picasso_SetPanning (struct regstruct *regs)
{
    uaecptr bmeptr;
    uaecptr bi                =           m68k_areg (regs, 0);
    uaecptr start_of_screen   =           m68k_areg (regs, 1);
    uae_u16 Width             =           m68k_dreg (regs, 0);
    picasso96_state.XOffset   = (uae_s16) m68k_dreg (regs, 1);
    picasso96_state.YOffset   = (uae_s16) m68k_dreg (regs, 2);
    picasso96_state.RGBFormat =           m68k_dreg (regs, 7);

    wgfx_flushline();

    /* Get our BoardInfo ptr's BitMapExtra ptr */
    bmeptr = get_long (bi + PSSO_BoardInfo_BitMapExtra);

    picasso96_state.Address       = start_of_screen; /* Amiga-side address */
    picasso96_state.VirtualWidth  = get_word (bmeptr + PSSO_BitMapExtra_Width);
    picasso96_state.VirtualHeight = get_word (bmeptr + PSSO_BitMapExtra_Height);
    picasso96_state.BytesPerPixel = GetBytesPerPixel (picasso96_state.RGBFormat);
    picasso96_state.BytesPerRow   = Width * picasso96_state.BytesPerPixel;

    set_panning_called = 1;

    P96TRACE (("P96: SetPanning(%d, %d, %d) Start 0x%x, BPR %d\n",
	       Width, picasso96_state.XOffset, picasso96_state.YOffset,
	       start_of_screen, picasso96_state.BytesPerRow));

    init_picasso_screen ();

    return 1;
}

static void do_xor8 (uae_u8 * ptr, long len, uae_u32 val)
{
    int i;
#if 0 && defined ALIGN_POINTER_TO32
    int align_adjust = ALIGN_POINTER_TO32 (ptr);
    int len2;

    len -= align_adjust;
    while (align_adjust) {
	*ptr ^= val;
	ptr++;
	align_adjust--;
    }
    len2 = len >> 2;
    len -= len2 << 2;
    for (i = 0; i < len2; i++, ptr += 4) {
	*(uae_u32 *) ptr ^= val;
    }
    while (len) {
	*ptr ^= val;
	ptr++;
	len--;
    }
    return;
#endif
    for (i = 0; i < len; i++, ptr++) {
	do_put_mem_byte (ptr, do_get_mem_byte (ptr) ^ val);
    }
}

/*
 * InvertRect:
 *
 * Inputs:
 * a0:struct BoardInfo *bi
 * a1:struct RenderInfo *ri
 * d0.w:X
 * d1.w:Y
 * d2.w:Width
 * d3.w:Height
 * d4.l:Mask
 * d7.l:RGBFormat
 *
 * This function is used to invert a rectangular area on the board. It is
 * called by BltBitMap, BltPattern and BltTemplate.
 */
uae_u32 REGPARAM2 picasso_InvertRect (struct regstruct *regs)
{
    uaecptr renderinfo   =          m68k_areg (regs, 1);
    unsigned long X      = (uae_u16)m68k_dreg (regs, 0);
    unsigned long Y      = (uae_u16)m68k_dreg (regs, 1);
    unsigned long Width  = (uae_u16)m68k_dreg (regs, 2);
    unsigned long Height = (uae_u16)m68k_dreg (regs, 3);
    uae_u8 mask          = (uae_u8) m68k_dreg (regs, 4);
    int Bpp     = GetBytesPerPixel (m68k_dreg (regs, 7));

    uae_u32 xorval;
    unsigned int lines;
    struct RenderInfo ri;
    uae_u8 *uae_mem, *rectstart;
    unsigned long width_in_bytes;
    int result = 0;

    wgfx_mergeline ();

    if (CopyRenderInfoStructureA2U (renderinfo, &ri)) {
	P96TRACE (("P96: InvertRect %dbpp 0x%lx\n", Bpp, (long)mask));

	if (mask != 0xFF && Bpp > 1)
	    mask = 0xFF;

	xorval = 0x01010101 * (mask & 0xFF);
	width_in_bytes = Bpp * Width;
	rectstart = uae_mem = ri.Memory + Y*ri.BytesPerRow + X*Bpp;

	for (lines = 0; lines < Height; lines++, uae_mem += ri.BytesPerRow)
	    do_xor8 (uae_mem, width_in_bytes, xorval);

	if (renderinfo_is_current_screen (&ri)) {
	    if (mask == 0xFF)
		do_invertrect (&ri, Bpp, X, Y, Width, Height);
	    else
		do_blit (&ri, Bpp, X, Y, X, Y, Width, Height, BLIT_SRC, 0);
	}
	result = 1;
    }
    return result;
}

/*
* Fill a rectangle in the screen.
*/
STATIC_INLINE void do_fillrect_frame_buffer (struct RenderInfo *ri,
					     int X, int Y,
					     int Width, int Height,
					     uae_u32 Pen, int Bpp,
					     RGBFTYPE RGBFormat)
{
    int cols;
    uae_u8 *start, *oldstart;
    uae_u8 *src, *dst;
    int lines;

    /* Do our virtual frame-buffer memory.  First, we do a single line
     * fill by hand */
    oldstart = start = src = ri->Memory + X * Bpp + Y * ri->BytesPerRow;

    switch (Bpp) {
	case 1:
	    memset (start, Pen, Width);
	    break;
	case 2:
	    for (cols = 0; cols < Width; cols++) {
		do_put_mem_word ((uae_u16 *)start, (uae_u16)Pen);
		start += 2;
	    }
	    break;
	case 3:
	    for (cols = 0; cols < Width; cols++) {
		do_put_mem_byte (start, (uae_u8)Pen);
		start++;
		*(uae_u16 *)(start) = (Pen & 0x00FFFF00) >> 8;
		start+=2;
	    }
	    break;
	case 4:
	    for (cols = 0; cols < Width; cols++) {
		do_put_mem_long ((uae_u32 *)start, Pen);
		start += 4;
	    }
	    break;
    } /* switch (Bpp) */

    src = oldstart;
    dst = src + ri->BytesPerRow;

    /* next, we do the remaining line fills via memcpy() for > 1 BPP,
     * otherwise some more memset() calls */
    if (Bpp > 1) {
	for (lines = 0; lines < (Height - 1); lines++, dst += ri->BytesPerRow)
	    memcpy (dst, src, Width * Bpp);
    } else {
	for (lines = 0; lines < (Height - 1); lines++, dst += ri->BytesPerRow)
	    memset( dst, Pen, Width );
    }
}

/***********************************************************
FillRect:
***********************************************************
* a0: 	struct BoardInfo *
* a1:	struct RenderInfo *
* d0: 	WORD X
* d1: 	WORD Y
* d2: 	WORD Width
* d3: 	WORD Height
* d4:	uae_u32 Pen
* d5:	UBYTE Mask
* d7:	uae_u32 RGBFormat
***********************************************************/
uae_u32 REGPARAM2 picasso_FillRect (struct regstruct *regs)
{
    uaecptr renderinfo =          m68k_areg (regs, 1);
    uae_u32 X          = (uae_u16)m68k_dreg (regs, 0);
    uae_u32 Y          = (uae_u16)m68k_dreg (regs, 1);
    uae_u32 Width      = (uae_u16)m68k_dreg (regs, 2);
    uae_u32 Height     = (uae_u16)m68k_dreg (regs, 3);
    uae_u32 Pen        =          m68k_dreg (regs, 4);
    uae_u8 Mask        = (uae_u8) m68k_dreg (regs, 5);
    RGBFTYPE RGBFormat =          m68k_dreg (regs, 7);

    uae_u8 *src;
    uae_u8 *oldstart;
    int Bpp;
    struct RenderInfo ri;
    uae_u32 result = 0;

#ifdef JIT
    special_mem |= picasso_is_special_read | picasso_is_special;
#endif

    wgfx_mergeline ();

    if (CopyRenderInfoStructureA2U (renderinfo, &ri) && Y != 0xFFFF) {
	if (ri.RGBFormat != RGBFormat)
	    write_log ("Weird Stuff!\n");

	Bpp = GetBytesPerPixel (RGBFormat);

	P96TRACE (("P96: FillRect(%d, %d, %d, %d) Pen 0x%x BPP %d BPR %d"
		   " Mask 0x%x\n", X, Y, Width, Height, Pen, Bpp,
		   ri.BytesPerRow, Mask));

	if (Bpp > 1)
	    Mask = 0xFF;

	if (Mask == 0xFF) {
	    if ((Width == 1) || (Height == 1)) {
		unsigned int i;
		uaecptr addr;
		if (renderinfo_is_current_screen (&ri)) {
		    uae_u32 offset = X * Bpp + Y * ri.BytesPerRow;
		    addr = gfxmem_start + (uaecptr)(ri.Memory - gfxmemory) + offset;

		    if (Width == 1) {
			for (i = 0; i < Height; i++) {
			    if( Bpp == 4 )
				gfxmem_lput (addr + (i * picasso96_state.BytesPerRow), Pen);
			    else if (Bpp == 2)
				gfxmem_wput (addr + (i * picasso96_state.BytesPerRow), Pen);
			    else
				gfxmem_bput (addr + (i * picasso96_state.BytesPerRow), Pen);
			}
		    } else if (Height == 1) {
			for (i = 0; i < Width; i++) {
			    if (Bpp == 4)
				gfxmem_lput (addr + (i*Bpp), Pen);
			    else if (Bpp == 2)
				gfxmem_wput (addr + (i*Bpp), Pen);
			    else
				gfxmem_bput( addr + (i*Bpp), Pen );
			}
		    }
		    return 1;
		}
	    }

	    /* Do the fill-rect in the frame-buffer */
	    do_fillrect_frame_buffer (&ri, X, Y, Width, Height, Pen, Bpp, RGBFormat);

	    /* Now we do the on-screen display, if renderinfo points to it */
	    if (renderinfo_is_current_screen (&ri)) {
		src = ri.Memory + X * Bpp + Y * ri.BytesPerRow;
		X = X - picasso96_state.XOffset;
		Y = Y - picasso96_state.YOffset;
		if ((int)X < 0) {Width = Width + X; X=0;}
		if ((int)Width < 1) return 1;
		if ((int)Y < 0) {Height = Height + Y; Y=0;}
		if ((int)Height < 1) return 1;

		/* Argh - why does P96Speed do this to me, with FillRect only?! */
		if ((X < picasso96_state.Width) && (Y < picasso96_state.Height)) {
		    if (X + Width > picasso96_state.Width)
			Width = picasso96_state.Width - X;
		    if (Y+Height > picasso96_state.Height)
			Height = picasso96_state.Height - Y;

		    do_fillrect (src, X, Y, Width, Height, Pen, Bpp, RGBFormat);
		}
	    }
	    result = 1;
	} else {
	    /* We get here only if Mask != 0xFF */
	    if (Bpp != 1) {
		P96TRACE (("P96: WARNING - FillRect() has unhandled mask 0x%x with Bpp %d."\
			   " Using fall-back routine.\n", Mask, Bpp));
	    } else {
		Pen &= Mask;
		Mask = ~Mask;
		oldstart = ri.Memory + Y * ri.BytesPerRow + X * Bpp;
		{
		    uae_u8 *start = oldstart;
		    uae_u8 *end = start + Height * ri.BytesPerRow;
		    for (; start != end; start += ri.BytesPerRow) {
			uae_u8 *p = start;
			unsigned long cols;
			for (cols = 0; cols < Width; cols++) {
			    uae_u32 tmpval = do_get_mem_byte (p + cols) & Mask;
			    do_put_mem_byte (p + cols, (uae_u8)(Pen | tmpval));
			}
		    }
		}
		if (renderinfo_is_current_screen (&ri))
		    do_blit (&ri, Bpp, X, Y, X, Y, Width, Height, BLIT_SRC, 0);
		result = 1;
	    }
	}
    }
    return result;
}


/*
* BlitRect() is a generic (any chunky pixel format) rectangle copier.
* NOTE: If dstri is NULL, then we're only dealing with one RenderInfo area,
* and called from picasso_BlitRect().
*
* OpCodes:
* 0 = FALSE:    dst = 0
* 1 = NOR:      dst = ~(src | dst)
* 2 = ONLYDST:  dst = dst & ~src
* 3 = NOTSRC:   dst = ~src
* 4 = ONLYSRC:  dst = src & ~dst
* 5 = NOTDST:   dst = ~dst
* 6 = EOR:      dst = src^dst
* 7 = NAND:     dst = ~(src & dst)
* 8 = AND:      dst = (src & dst)
* 9 = NEOR:     dst = ~(src ^ dst)
*10 = DST:      dst = dst
*11 = NOTONLYSRC: dst = ~src | dst
*12 = SRC:      dst = src
*13 = NOTONLYDST: dst = ~dst | src
*14 = OR:       dst = src | dst
*15 = TRUE:     dst = 0xFF
*/
struct blitdata
{
    struct RenderInfo  ri_struct;
    struct RenderInfo  dstri_struct;
    struct RenderInfo *ri; /* Self-referencing pointers */
    struct RenderInfo *dstri;
    unsigned long      srcx;
    unsigned long      srcy;
    unsigned long      dstx;
    unsigned long      dsty;
    unsigned long      width;
    unsigned long      height;
    uae_u8             mask;
    BLIT_OPCODE        opcode;
} blitrectdata;

STATIC_INLINE int BlitRectHelper (void)
{
    struct RenderInfo *ri    = blitrectdata.ri;
    struct RenderInfo *dstri = blitrectdata.dstri;
    unsigned long srcx       = blitrectdata.srcx;
    unsigned long srcy       = blitrectdata.srcy;
    unsigned long dstx       = blitrectdata.dstx;
    unsigned long dsty       = blitrectdata.dsty;
    unsigned long width      = blitrectdata.width;
    unsigned long height     = blitrectdata.height;
    uae_u8 mask              = blitrectdata.mask;
    BLIT_OPCODE opcode       = blitrectdata.opcode;

    uae_u8 Bpp = GetBytesPerPixel (ri->RGBFormat);
    int can_do_visible_blit = 0;

    if (opcode == BLIT_DST) {
	write_log ("P96: WARNING - BlitRect() being called with opcode of BLIT_DST\n");
	return 1;
    }

    /*
     * If we have no destination RenderInfo, then we're dealing with a
     * single-buffer action, called from picasso_BlitRect(). The code in
     * do_blitrect_frame_buffer() deals with the frame-buffer, while the
     * do_blit() code deals with the visible screen.
     *
     * If we have a destination RenderInfo, then we've been called from
     * picasso_BlitRectNoMaskComplete() and we need to put the results on the
     * screen from the frame-buffer.
     */
    if (dstri == NULL) {
	if (mask != 0xFF && Bpp > 1) {
	    mask = 0xFF;
	}
	dstri = ri;
	can_do_visible_blit = 1;
    }

    /* Do our virtual frame-buffer memory first */
    do_blitrect_frame_buffer (ri, dstri, srcx, srcy, dstx, dsty, width, height,
			      mask, opcode);


    /* Now we do the on-screen display, if renderinfo points to it */
    if (renderinfo_is_current_screen (dstri)) {
	if (mask == 0xFF || Bpp > 1) {
	    if (can_do_visible_blit)
		do_blit (dstri, Bpp, srcx, srcy, dstx, dsty, width, height, opcode, 1);
	    else
		do_blit (dstri, Bpp, dstx, dsty, dstx, dsty, width, height, opcode, 0);
	} else
	    do_blit (dstri, Bpp, dstx, dsty, dstx, dsty, width, height, opcode, 0);

	P96TRACE (("P96: Did do_blit 1 in BlitRect()\n"));
    } else {
	P96TRACE (("P96: Did not do_blit 1 in BlitRect()\n"));
    }

    return 1;
}

STATIC_INLINE int BlitRect (uaecptr ri, uaecptr dstri,
			    unsigned long srcx, unsigned long srcy,
			    unsigned long dstx, unsigned long dsty,
			    unsigned long width, unsigned long height,
			    uae_u8 mask, BLIT_OPCODE opcode)
{
    /* Set up the params */
    CopyRenderInfoStructureA2U (ri, &blitrectdata.ri_struct);
    blitrectdata.ri = &blitrectdata.ri_struct;

    if (dstri) {
	CopyRenderInfoStructureA2U( dstri, &blitrectdata.dstri_struct );
	blitrectdata.dstri = &blitrectdata.dstri_struct;
    } else
	blitrectdata.dstri = NULL;

    blitrectdata.srcx   = srcx;
    blitrectdata.srcy   = srcy;
    blitrectdata.dstx   = dstx;
    blitrectdata.dsty   = dsty;
    blitrectdata.width  = width;
    blitrectdata.height = height;
    blitrectdata.mask   = mask;
    blitrectdata.opcode = opcode;

    return BlitRectHelper();
}

/***********************************************************
BlitRect:
***********************************************************
* a0:   struct BoardInfo
* a1:   struct RenderInfo
* d0:   WORD SrcX
* d1:   WORD SrcY
* d2:   WORD DstX
* d3:   WORD DstY
* d4:   WORD Width
* d5:   WORD Height
* d6:   UBYTE Mask
* d7:   uae_u32 RGBFormat
***********************************************************/
uae_u32 REGPARAM2 picasso_BlitRect (struct regstruct *regs)
{
    uaecptr renderinfo   =          m68k_areg (regs, 1);
    unsigned long srcx   = (uae_u16)m68k_dreg (regs, 0);
    unsigned long srcy   = (uae_u16)m68k_dreg (regs, 1);
    unsigned long dstx   = (uae_u16)m68k_dreg (regs, 2);
    unsigned long dsty   = (uae_u16)m68k_dreg (regs, 3);
    unsigned long width  = (uae_u16)m68k_dreg (regs, 4);
    unsigned long height = (uae_u16)m68k_dreg (regs, 5);
    uae_u8  Mask         = (uae_u8) m68k_dreg (regs, 6);

    int result = 0;

#ifdef JIT
    special_mem |= picasso_is_special_read | picasso_is_special;
#endif

    wgfx_mergeline ();

    P96TRACE (("P96: BlitRect(%d, %d, %d, %d, %d, %d, 0x%x)\n", srcx, srcy,
	       dstx, dsty, width, height, Mask));

    result = BlitRect (renderinfo, 0, srcx, srcy, dstx, dsty,
		       width, height, Mask, BLIT_SRC);

    return result;
}

/***********************************************************
BlitRectNoMaskComplete:
***********************************************************
* a0: 	struct BoardInfo
* a1:	struct RenderInfo (src)
* a2:   struct RenderInfo (dst)
* d0: 	WORD SrcX
* d1: 	WORD SrcY
* d2: 	WORD DstX
* d3: 	WORD DstY
* d4:   WORD Width
* d5:   WORD Height
* d6:	UBYTE OpCode
* d7:	uae_u32 RGBFormat
* NOTE: MUST return 0 in D0 if we're not handling this operation
*       because the RGBFormat or opcode aren't supported.
*       OTHERWISE return 1
***********************************************************/
uae_u32 REGPARAM2 picasso_BlitRectNoMaskComplete (struct regstruct *regs)
{
    uaecptr srcri        =          m68k_areg (regs, 1);
    uaecptr dstri        =          m68k_areg (regs, 2);
    unsigned long srcx   = (uae_u16)m68k_dreg (regs, 0);
    unsigned long srcy   = (uae_u16)m68k_dreg (regs, 1);
    unsigned long dstx   = (uae_u16)m68k_dreg (regs, 2);
    unsigned long dsty   = (uae_u16)m68k_dreg (regs, 3);
    unsigned long width  = (uae_u16)m68k_dreg (regs, 4);
    unsigned long height = (uae_u16)m68k_dreg (regs, 5);
    uae_u8 OpCode        =          m68k_dreg (regs, 6);
/*    uae_u32 RGBFmt       =          m68k_dreg (regs, 7); */

    int result = 0;

#ifdef JIT
    special_mem |= picasso_is_special_read | picasso_is_special;
#endif

    wgfx_mergeline ();

    P96TRACE (("P96: BlitRectNoMaskComplete() op 0x%2x, xy(%4d,%4d) -->"
	       " xy(%4d,%4d), wh(%4d,%4d)\n",
	       OpCode, srcx, srcy, dstx, dsty, width, height));

    result = BlitRect (srcri, dstri, srcx, srcy, dstx, dsty,
		       width, height, 0xFF, OpCode);

    return result;
}

/***********************************************************
BlitRectTransparent:  (v16, from WinUAE)
***********************************************************
* a0:	struct BoardInfo
* a1:	struct RenderInfo (src)
* a2:	struct RenderInfo (dst)
* d0:	WORD SrcX
* d1:	WORD SrcY
* d2:	WORD DstX
* d3:	WORD DstY
* d4:	WORD Width
* d5:	WORD Height
* d6:	ULONG transparent colour
* d7:	RGBFTYPE RGBFormat
* Copies all pixels whose colour differs from the transparent colour.
* Colours are compared as they are stored in Amiga memory (big endian),
* without the unused alpha / padding bits.
***********************************************************/
static uae_u32 transparent_mask (uae_u32 RGBFmt)
{
    switch (RGBFmt) {
	case RGBFB_CLUT:      return 0xff;
	case RGBFB_R8G8B8:
	case RGBFB_B8G8R8:    return 0xffffff;
	case RGBFB_A8R8G8B8:
	case RGBFB_A8B8G8R8:  return 0x00ffffff;
	case RGBFB_R8G8B8A8:
	case RGBFB_B8G8R8A8:  return 0xffffff00;
	case RGBFB_R5G5B5:    return 0x7fff;
	case RGBFB_R5G5B5PC:
	case RGBFB_B5G5R5PC:  return 0xff7f;
	default:              return 0xffff;
    }
}

STATIC_INLINE uae_u32 read_pixel_be (const uae_u8 *p, int Bpp)
{
    switch (Bpp) {
	case 1:  return p[0];
	case 2:  return (p[0] << 8) | p[1];
	case 3:  return (p[0] << 16) | (p[1] << 8) | p[2];
	default: return ((uae_u32) p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
    }
}

uae_u32 REGPARAM2 picasso_BlitRectTransparent (struct regstruct *regs)
{
    uaecptr srcri        =          m68k_areg (regs, 1);
    uaecptr dstri        =          m68k_areg (regs, 2);
    unsigned long srcx   = (uae_u16)m68k_dreg (regs, 0);
    unsigned long srcy   = (uae_u16)m68k_dreg (regs, 1);
    unsigned long dstx   = (uae_u16)m68k_dreg (regs, 2);
    unsigned long dsty   = (uae_u16)m68k_dreg (regs, 3);
    unsigned long width  = (uae_u16)m68k_dreg (regs, 4);
    unsigned long height = (uae_u16)m68k_dreg (regs, 5);
    uae_u32 color        =          m68k_dreg (regs, 6);
    uae_u32 RGBFmt       = (uae_u8) m68k_dreg (regs, 7);

    struct RenderInfo sri, dri;
    uae_u8 *tmp = NULL, *src, *dst;
    unsigned long y, x, srcbpr;
    uae_u32 mask;
    int Bpp;

#ifdef JIT
    special_mem |= picasso_is_special_read | picasso_is_special;
#endif

    wgfx_mergeline ();

    if (!CopyRenderInfoStructureA2U (srcri, &sri) || !CopyRenderInfoStructureA2U (dstri, &dri))
	return 0;
    Bpp = GetBytesPerPixel (RGBFmt);
    if (Bpp == 0 || width == 0 || height == 0)
	return 0;

    mask   = transparent_mask (RGBFmt);
    color &= mask;
    src    = sri.Memory + srcx * Bpp + srcy * sri.BytesPerRow;
    dst    = dri.Memory + dstx * Bpp + dsty * dri.BytesPerRow;
    srcbpr = sri.BytesPerRow;

    /* Same bitmap (possibly overlapping): copy the source rectangle first */
    if (sri.Memory == dri.Memory) {
	tmp = xmalloc (width * Bpp * height);
	if (!tmp)
	    return 0;
	for (y = 0; y < height; y++)
	    memcpy (tmp + y * width * Bpp, src + y * sri.BytesPerRow, width * Bpp);
	src    = tmp;
	srcbpr = width * Bpp;
    }

    for (y = 0; y < height; y++) {
	const uae_u8 *s = src + y * srcbpr;
	uae_u8 *d = dst + y * dri.BytesPerRow;
	for (x = 0; x < width; x++, s += Bpp, d += Bpp) {
	    if ((read_pixel_be (s, Bpp) & mask) != color)
		memcpy (d, s, Bpp);
	}
    }
    if (tmp)
	free (tmp);

    if (renderinfo_is_current_screen (&dri))
	do_blit (&dri, Bpp, dstx, dsty, dstx, dsty, width, height, BLIT_SRC, 0);

    P96TRACE (("P96: BlitRectTransparent(%d,%d -> %d,%d, %dx%d, colour %08x, fmt %d)\n",
	       srcx, srcy, dstx, dsty, width, height, color, RGBFmt));
    return 1;
}

/*
 * Hardware sprite (mouse pointer), v16, after WinUAE picasso96_win.cpp.
 * The emulation only keeps the image and position; the host side mixes the
 * pointer into the output picture (gfx-amigaos/ami-win.c), the frame
 * buffer itself is never touched.
 */
struct picasso96_sprite picasso96_sprite;

/*
 * SetSpritePosition:
 * a0: struct BoardInfo *bi
 * d7: RGBFTYPE RGBFormat
 * The new position is in bi->MouseX / bi->MouseY (hot spot already applied
 * by Picasso96, as in WinUAE).
 */
uae_u32 REGPARAM2 picasso_SetSpritePosition (struct regstruct *regs)
{
    uaecptr bi = m68k_areg (regs, 0);

    if (!picasso96_sprite.enabled)
	return 0;
    picasso96_sprite.x = (uae_s16) get_word (bi + PSSO_BoardInfo_MouseX) - picasso96_state.XOffset;
    picasso96_sprite.y = (uae_s16) get_word (bi + PSSO_BoardInfo_MouseY) - picasso96_state.YOffset;
    return 1;
}

/*
 * SetSpriteColor:
 * a0: struct BoardInfo *bi
 * d0.b: index (0-2)
 * d1.b: red, d2.b: green, d3.b: blue
 * d7: RGBFTYPE RGBFormat
 */
uae_u32 REGPARAM2 picasso_SetSpriteColor (struct regstruct *regs)
{
    int idx = (m68k_dreg (regs, 0) & 0xff) + 1;
    uae_u32 rgb = ((m68k_dreg (regs, 1) & 0xff) << 16)
		| ((m68k_dreg (regs, 2) & 0xff) << 8)
		|  (m68k_dreg (regs, 3) & 0xff);

    if (!picasso96_sprite.enabled || idx < 1 || idx > 3)
	return 0;
    if (picasso96_sprite.colors[idx] != rgb) {
	picasso96_sprite.colors[idx] = rgb;
	picasso96_sprite.serial++;
    }
    return 1;
}

static uae_u32 load_sprite_image (uaecptr bi)
{
    struct picasso96_sprite *spr = &picasso96_sprite;
    int w = get_byte (bi + PSSO_BoardInfo_MouseWidth);
    int h = get_byte (bi + PSSO_BoardInfo_MouseHeight);
    uae_u32 flags = get_long (bi + PSSO_BoardInfo_Flags);
    int hires = (flags & BIF_HIRESSPRITE) ? 2 : 1;
    int big   = (flags & BIF_BIGSPRITE) ? 1 : 0;
    uaecptr image = get_long (bi + PSSO_BoardInfo_MouseImage);
    int datasize = 4 * hires + h * 4 * hires;
    int x, y, yy;

    spr->valid = 0;
    spr->serial++;
    if (w <= 0 || h <= 0 || !image || !valid_address (image, datasize))
	return 1;
    if (w > P96_SPRITE_MAXWIDTH)
	w = P96_SPRITE_MAXWIDTH;
    if (h > P96_SPRITE_MAXHEIGHT)
	h = P96_SPRITE_MAXHEIGHT;

    /* Image: one header line, then per line two bit planes (two words, or
     * two longwords with BIF_HIRESSPRITE). BIF_BIGSPRITE: every pixel is
     * doubled horizontally and vertically - width/height are already
     * doubled, the image data is not. */
    memset (spr->pixels, 0, sizeof spr->pixels);
    for (y = 0, yy = 0; y < h; y++, yy++) {
	uae_u8 *p = spr->pixels + y * P96_SPRITE_MAXWIDTH;
	uae_u8 *prev = p;
	uaecptr img = image + 4 * hires + yy * 4 * hires;
	uae_u32 d1 = get_long (img);
	uae_u32 d2 = get_long (img + 2 * hires);
	int bits = 0;

	x = 0;
	while (x < w && bits < 16 * hires) {
	    uae_u8 c = ((d2 & 0x80000000) ? 2 : 0) + ((d1 & 0x80000000) ? 1 : 0);
	    d1 <<= 1;
	    d2 <<= 1;
	    bits++;
	    *p++ = c;
	    x++;
	    if (big && x < w) {
		*p++ = c;
		x++;
	    }
	}
	if (big && y + 1 < h) {
	    y++;
	    memcpy (spr->pixels + y * P96_SPRITE_MAXWIDTH, prev, w);
	}
    }
    spr->width  = w;
    spr->height = h;
    spr->valid  = 1;
    return 1;
}

/*
 * SetSpriteImage:
 * a0: struct BoardInfo *bi
 * d7: RGBFTYPE RGBFormat
 */
uae_u32 REGPARAM2 picasso_SetSpriteImage (struct regstruct *regs)
{
    if (!picasso96_sprite.enabled)
	return 0;
    return load_sprite_image (m68k_areg (regs, 0));
}

/*
 * SetSprite:
 * a0: struct BoardInfo *bi
 * d0: BOOL activate
 * d7: RGBFTYPE RGBFormat
 */
uae_u32 REGPARAM2 picasso_SetSprite (struct regstruct *regs)
{
    if (!picasso96_sprite.enabled)
	return 0;
    if (m68k_dreg (regs, 0)) {
	load_sprite_image (m68k_areg (regs, 0));
	picasso96_sprite.visible = 1;
    } else
	picasso96_sprite.visible = 0;
    picasso96_sprite.serial++;
    return 1;
}


/* This utility function is used both by BlitTemplate() and BlitPattern() */
STATIC_INLINE void PixelWrite1 (uae_u8 * mem, int bits, uae_u32 fgpen, uae_u32 mask)
{
    if (mask != 0xFF)
	fgpen = (fgpen & mask) | (do_get_mem_byte (mem + bits) & ~mask);
    do_put_mem_byte (mem + bits, fgpen);
}

STATIC_INLINE void PixelWrite2 (uae_u8 * mem, int bits, uae_u32 fgpen)
{
    do_put_mem_word (((uae_u16 *) mem) + bits, fgpen);
}

STATIC_INLINE void PixelWrite3 (uae_u8 * mem, int bits, uae_u32 fgpen)
{
    do_put_mem_byte (mem + bits * 3, fgpen & 0x000000FF);
    *(uae_u16 *) (mem + bits * 3 + 1) = (fgpen & 0x00FFFF00) >> 8;
}

STATIC_INLINE void PixelWrite4 (uae_u8 * mem, int bits, uae_u32 fgpen)
{
    do_put_mem_long (((uae_u32 *) mem) + bits, fgpen);
}

STATIC_INLINE void PixelWrite (uae_u8 * mem, int bits, uae_u32 fgpen, uae_u8 Bpp, uae_u32 mask)
{
    switch (Bpp) {
    case 1:
	if (mask != 0xFF)
	    fgpen = (fgpen & mask) | (do_get_mem_byte (mem + bits) & ~mask);
	do_put_mem_byte (mem + bits, fgpen);
	break;
    case 2:
	do_put_mem_word (((uae_u16 *) mem) + bits, fgpen);
	break;
    case 3:
	do_put_mem_byte (mem + bits * 3, fgpen & 0x000000FF);
	*(uae_u16 *) (mem + bits * 3 + 1) = (fgpen & 0x00FFFF00) >> 8;
	break;
    case 4:
	do_put_mem_long (((uae_u32 *) mem) + bits, fgpen);
	break;
    }
}

/*
 * BlitPattern:
 *
 * Synopsis:BlitPattern(bi, ri, pattern, X, Y, Width, Height, Mask, RGBFormat);
 * Inputs:
 * a0:struct BoardInfo *bi
 * a1:struct RenderInfo *ri
 * a2:struct Pattern *pattern
 * d0.w:X
 * d1.w:Y
 * d2.w:Width
 * d3.w:Height
 * d4.w:Mask
 * d7.l:RGBFormat
 *
 * This function is used to paint a pattern on the board memory using the
 * blitter. It is called by BltPattern, if a AreaPtrn is used with positive
 * AreaPtSz. The pattern consists of a b/w image using a single plane of image
 * data which will be expanded repeatedly to the destination RGBFormat using
 * ForeGround and BackGround pens as well as draw modes. The width of the
 * pattern data is always 16 pixels (one word) and the height is calculated as
 * 2^Size. The data must be shifted up and to the left by XOffset and YOffset
 * pixels at the beginning.
 */
uae_u32 REGPARAM picasso_BlitPattern (struct regstruct *regs)
{
    uaecptr rinf    =          m68k_areg (regs, 1);
    uaecptr pinf    =          m68k_areg (regs, 2);
    unsigned long X = (uae_u16)m68k_dreg (regs, 0);
    unsigned long Y = (uae_u16)m68k_dreg (regs, 1);
    unsigned long W = (uae_u16)m68k_dreg (regs, 2);
    unsigned long H = (uae_u16)m68k_dreg (regs, 3);
    uae_u8 Mask     = (uae_u8) m68k_dreg (regs, 4);
    uae_u32 RGBFmt  =          m68k_dreg (regs, 7);

    uae_u8 Bpp = GetBytesPerPixel (RGBFmt);
    int inversion = 0;
    struct RenderInfo ri;
    struct Pattern pattern;
    unsigned long rows;
    uae_u32 fgpen;
    uae_u8 *uae_mem;
    int xshift;
    unsigned long ysize_mask;
    int result = 0;

#ifdef JIT
    special_mem |= picasso_is_special_read | picasso_is_special;
#endif

    wgfx_mergeline ();

    if (CopyRenderInfoStructureA2U (rinf, &ri)
		&& CopyPatternStructureA2U (pinf, &pattern)) {
	Bpp = GetBytesPerPixel (ri.RGBFormat);
	uae_mem = ri.Memory + Y*ri.BytesPerRow + X*Bpp; /* offset with address */

	if (pattern.DrawMode & INVERS)
	    inversion = 1;

	pattern.DrawMode &= 0x03;

	if (Mask != 0xFF) {
	    if( Bpp > 1 )
		Mask = 0xFF;

	    if( pattern.DrawMode == COMP)
		 P96TRACE (("P96: WARNING - BlitPattern() has unhandled mask 0x%x with"\
			    " COMP DrawMode. Using fall-back routine.\n", Mask));
	    else
		result = 1;
	} else
	    result = 1;

	if (result) {
#           ifdef P96TRACING_ENABLED
		DumpPattern (&pattern);
#           endif
	    ysize_mask = (1 << pattern.Size) - 1;
	    xshift = pattern.XOffset & 15;

	    for (rows = 0; rows < H; rows++, uae_mem += ri.BytesPerRow) {
		unsigned long prow = (rows + pattern.YOffset) & ysize_mask;
		unsigned int d = do_get_mem_word (((uae_u16 *)pattern.Memory) + prow);
		uae_u8 *uae_mem2 = uae_mem;
		unsigned long cols;

		if (xshift != 0)
		    d = (d << xshift) | (d >> (16 - xshift));

		for (cols = 0; cols < W; cols += 16, uae_mem2 += Bpp << 4) {
		    long bits;
		    long max = W - cols;
		    unsigned int data = d;

		    if (max > 16)
			max = 16;

		    for (bits = 0; bits < max; bits++) {
			int bit_set = data & 0x8000;
			data <<= 1;
			switch (pattern.DrawMode) {
			    case JAM1:
				if (inversion)
				    bit_set = !bit_set;
				if (bit_set)
				    PixelWrite (uae_mem2, bits, pattern.FgPen, Bpp, Mask);
				break;
			    case JAM2:
				if (inversion)
				    bit_set = !bit_set;
				if (bit_set)
				    PixelWrite (uae_mem2, bits, pattern.FgPen, Bpp, Mask);
				else
				    PixelWrite (uae_mem2, bits, pattern.BgPen, Bpp, Mask);
				break;
			    case COMP:
				if (bit_set) {
				    fgpen = pattern.FgPen;

				    switch (Bpp) {
					case 1: {
					    uae_u8 *addr = uae_mem2 + bits;
					    do_put_mem_byte (addr, (uae_u8)(do_get_mem_byte (addr) ^ fgpen));
					    break;
					}
					case 2: {
					    uae_u16 *addr = ((uae_u16 *)uae_mem2) + bits;
					    do_put_mem_word (addr, (uae_u16)(do_get_mem_word (addr) ^ fgpen));
					    break;
					}
					case 3: {
					    uae_u32 *addr = (uae_u32 *)(uae_mem2 + bits * 3);
					    do_put_mem_long (addr, do_get_mem_long (addr) ^ (fgpen & 0x00FFFFFF));
					    break;
					}
					case 4: {
					    uae_u32 *addr = ((uae_u32 *)uae_mem2) + bits;
					    do_put_mem_long (addr, do_get_mem_long (addr) ^ fgpen);
					    break;
					}
				    } /* switch (Bpp) */
				}
				break;
			} /* switch (pattern.DrawMode) */
		    } /* for (bits) */
		} /* for (cols) */
	    } /* for (rows) */

	    /* If we need to update a second-buffer (extra_mem is set), then do it only if visible! */
	    if (picasso_vidinfo.extra_mem && renderinfo_is_current_screen (&ri))
		do_blit (&ri, Bpp, X, Y, X, Y, W, H, BLIT_SRC, 0);

	    result = 1;
	}
    }
    return result;
}

/*************************************************
BlitTemplate:
**************************************************
* Synopsis: BlitTemplate(bi, ri, template, X, Y, Width, Height, Mask, RGBFormat);
* a0: struct BoardInfo *bi
* a1: struct RenderInfo *ri
* a2: struct Template *template
* d0.w: X
* d1.w: Y
* d2.w: Width
* d3.w: Height
* d4.w: Mask
* d7.l: RGBFormat
*
* This function is used to paint a template on the board memory using the
* blitter. It is called by BltPattern and BltTemplate. The template consists
* of a b/w image using a single plane of image data which will be expanded to
* the destination RGBFormat using ForeGround and BackGround pens as well as
* draw modes.
***********************************************************************************/
uae_u32 REGPARAM2 picasso_BlitTemplate (struct regstruct *regs)
{
    uaecptr rinf    =          m68k_areg (regs, 1);
    uaecptr tmpl    =          m68k_areg (regs, 2);
    unsigned long X = (uae_u16)m68k_dreg (regs, 0);
    unsigned long Y = (uae_u16)m68k_dreg (regs, 1);
    unsigned long W = (uae_u16)m68k_dreg (regs, 2);
    unsigned long H = (uae_u16)m68k_dreg (regs, 3);
    uae_u16 Mask    = (uae_u16)m68k_dreg (regs, 4);

    uae_u8 inversion = 0;
    struct Template tmp;
    struct RenderInfo ri;
    unsigned long rows;
    int bitoffset;
    uae_u32 fgpen;
    uae_u8 *uae_mem, Bpp;
    uae_u8 *tmpl_base;
    uae_u32 result = 0;

#ifdef JIT
    special_mem |= picasso_is_special_read | picasso_is_special;
#endif

    wgfx_mergeline ();

    if (CopyRenderInfoStructureA2U (rinf, &ri) && CopyTemplateStructureA2U (tmpl, &tmp)) {
	Bpp = GetBytesPerPixel (ri.RGBFormat);
	uae_mem = ri.Memory + Y*ri.BytesPerRow + X*Bpp; /* offset into address */

	if (tmp.DrawMode & INVERS)
	    inversion = 1;

	tmp.DrawMode &= 0x03;

	if (Mask != 0xFF) {
	    if (Bpp > 1)
		Mask = 0xFF;

	    if (tmp.DrawMode == COMP) {
		P96TRACE (("P96: WARNING - BlitTemplate() has unhandled mask 0x%x with"\
			   " COMP DrawMode. Using fall-back routine.\n", Mask));
#		if 0 //def _WIN32
		    flushpixels();  //only need in the windows Version
#		endif
		return 0;
	    } else
		result = 1;
	} else
	    result = 1;

#if 1
	if (tmp.DrawMode == COMP) {
	    /* workaround, let native blitter handle COMP mode */
#	    if 0 //def _WIN32
		flushpixels();
#	    endif
	    return 0;
	}
#endif

	if (result) {
	    P96TRACE (("P96: BlitTemplate() xy(%d,%d), wh(%d,%d) draw 0x%x fg 0x%x bg 0x%x\n",
		X, Y, W, H, tmp.DrawMode, tmp.FgPen, tmp.BgPen));

	    bitoffset = tmp.XOffset % 8;

#	    if defined( P96TRACING_ENABLED ) && ( P96TRACING_LEVEL > 0 )
		DumpTemplate (&tmp, W, H);
#	    endif

	    tmpl_base = tmp.Memory + tmp.XOffset / 8;

	    for (rows = 0; rows < H; rows++, uae_mem += ri.BytesPerRow, tmpl_base += tmp.BytesPerRow) {
		unsigned long cols;
		uae_u8 *tmpl_mem = tmpl_base;
		uae_u8 *uae_mem2 = uae_mem;
		unsigned int data = *tmpl_mem;

		for (cols = 0; cols < W; cols += 8, uae_mem2 += Bpp << 3) {
		    unsigned int byte;
		    long bits;
		    long max = W - cols;

		    if (max > 8)
			max = 8;

		    data <<= 8;
		    data |= *++tmpl_mem;

		    byte = data >> (8 - bitoffset);

		    for (bits = 0; bits < max; bits++) {
			int bit_set = (byte & 0x80);
			byte <<= 1;
			switch (tmp.DrawMode) {
			    case JAM1:
				if (inversion)
				    bit_set = !bit_set;
				if (bit_set) {
				    fgpen = tmp.FgPen;
				    PixelWrite (uae_mem2, bits, fgpen, Bpp, Mask);
				}
				break;
			    case JAM2:
				if (inversion)
				    bit_set = !bit_set;
				fgpen = tmp.BgPen;
				if (bit_set)
				    fgpen = tmp.FgPen;
				PixelWrite (uae_mem2, bits, fgpen, Bpp, Mask);
				break;
			    case COMP:
				if (bit_set) {
				    fgpen = tmp.FgPen;

				    switch (Bpp) {
					case 1: {
					    uae_u8 *addr = uae_mem2 + bits;
					    do_put_mem_byte (addr, (uae_u8) (do_get_mem_byte (addr) ^ fgpen));
					    break;
					}
					case 2: {
					    uae_u16 *addr = ((uae_u16 *)uae_mem2) + bits;
					    do_put_mem_word (addr, (uae_u16) (do_get_mem_word (addr) ^ fgpen));
					    break;
					}
					case 3: {
					    uae_u32 *addr = (uae_u32 *)(uae_mem2 + bits * 3);
					    do_put_mem_long (addr, do_get_mem_long (addr) ^ (fgpen & 0x00FFFFFF));
					    break;
					}
					case 4: {
					    uae_u32 *addr = ((uae_u32 *)uae_mem2) + bits;
					    do_put_mem_long (addr, do_get_mem_long (addr) ^ fgpen);
					    break;
					}
				    } /* switch (Bpp) */
				} /* if (bit_set) */
				break;
			} /* switch (tmp.DrawMode) */
		    } /* for (bits) */
		} /* for (cols) */
	    } /* for (rows) */

	    /* If we need to update a second-buffer (extra_mem is set), then do it only if visible! */
	    if (picasso_vidinfo.extra_mem && renderinfo_is_current_screen (&ri))
		do_blit (&ri, Bpp, X, Y, X, Y, W, H, BLIT_SRC, 0);
	}
    }
    return 1;
}

/*
 * CalculateBytesPerRow:
 * a0: 	struct BoardInfo
 * d0: 	uae_u16 Width
 * d7:	RGBFTYPE RGBFormat
 * This function calculates the amount of bytes needed for a line of
 * "Width" pixels in the given RGBFormat.
 */
uae_u32 REGPARAM2 picasso_CalculateBytesPerRow (struct regstruct *regs)
{
    uae_u16 width = m68k_dreg (regs, 0);
    uae_u32 type  = m68k_dreg (regs, 7);

    width = GetBytesPerPixel (type) * width;
    P96TRACE (("P96: CalculateBytesPerRow() = %d\n", width));

    return width;
}

/*
 * SetDisplay:
 * a0:	struct BoardInfo
 * d0:	BOOL state
 * This function enables and disables the video display.
 *
 * NOTE: return the opposite of the state
 */
uae_u32 REGPARAM2 picasso_SetDisplay (struct regstruct *regs)
{
    uae_u32 state = m68k_dreg (regs, 0);
    P96TRACE (("P96: SetDisplay(%d)\n", state));
    return !state;
}

/*
 * WaitVerticalSync:
 * a0:	struct BoardInfo
 * This function waits for the next horizontal retrace.
 */
uae_u32 REGPARAM2 picasso_WaitVerticalSync (struct regstruct *regs)
{
    /*write_log ("WaitVerticalSync()\n"); */
    return 1;
}

/* NOTE: Watch for those planeptrs of 0x00000000 and 0xFFFFFFFF for all zero / all one bitmaps !!!! */
static void PlanarToChunky (struct RenderInfo *ri, struct BitMap *bm,
			    unsigned long srcx,  unsigned long srcy,
			    unsigned long dstx,  unsigned long dsty,
			    unsigned long width, unsigned long height,
			    uae_u8 mask)
{
    int j;

    uae_u8 *PLANAR[8];
    uae_u8 *image = ri->Memory + dstx * GetBytesPerPixel (ri->RGBFormat)
		  + dsty * ri->BytesPerRow;
    int Depth = bm->Depth;
    unsigned long rows, bitoffset = srcx & 7;
    long eol_offset;

    /* Set up our bm->Planes[] pointers to the right horizontal offset */
    for (j = 0; j < Depth; j++) {
	uae_u8 *p = bm->Planes[j];
	if (p != &all_zeros_bitmap && p != &all_ones_bitmap)
	    p += srcx / 8 + srcy * bm->BytesPerRow;
	PLANAR[j] = p;
	if ((mask & (1 << j)) == 0)
	    PLANAR[j] = &all_zeros_bitmap;
    }
    eol_offset = (long) bm->BytesPerRow - (long) ((width + 7) >> 3);
    for (rows = 0; rows < height; rows++, image += ri->BytesPerRow) {
	unsigned long cols;

	for (cols = 0; cols < width; cols += 8) {
	    int k;
	    uae_u32 a = 0, b = 0;
	    unsigned int msk = 0xFF;
	    long tmp = cols + 8 - width;
	    if (tmp > 0) {
		msk <<= tmp;
		b = do_get_mem_long ((uae_u32 *) (image + cols + 4));
		if (tmp < 4)
		    b &= 0xFFFFFFFF >> (32 - tmp * 8);
		else if (tmp > 4) {
		    a = do_get_mem_long ((uae_u32 *) (image + cols));
		    a &= 0xFFFFFFFF >> (64 - tmp * 8);
		}
	    }
	    for (k = 0; k < Depth; k++) {
		unsigned int data;
		if (PLANAR[k] == &all_zeros_bitmap)
		    data = 0;
		else if (PLANAR[k] == &all_ones_bitmap)
		    data = 0xFF;
		else {
		    data = (uae_u8) (do_get_mem_word ((uae_u16 *) PLANAR[k]) >> (8 - bitoffset));
		    PLANAR[k]++;
		}
		data &= msk;
		a |= p2ctab[data][0] << k;
		b |= p2ctab[data][1] << k;
	    }
	    do_put_mem_long ((uae_u32 *) (image + cols), a);
	    do_put_mem_long ((uae_u32 *) (image + cols + 4), b);
	}
	for (j = 0; j < Depth; j++) {
	    if (PLANAR[j] != &all_zeros_bitmap && PLANAR[j] != &all_ones_bitmap) {
		PLANAR[j] += eol_offset;
	    }
	}
    }
}

/*
 * BlitPlanar2Chunky:
 * a0: struct BoardInfo *bi
 * a1: struct BitMap *bm - source containing planar information and assorted details
 * a2: struct RenderInfo *ri - dest area and its details
 * d0.w: SrcX
 * d1.w: SrcY
 * d2.w: DstX
 * d3.w: DstY
 * d4.w: SizeX
 * d5.w: SizeY
 * d6.b: MinTerm - uh oh!
 * d7.b: Mask - uh oh!
 *
 * This function is currently used to blit from planar bitmaps within system
 * memory to chunky bitmaps on the board. Watch out for plane pointers that
 * are 0x00000000 (represents a plane with all bits "0") or 0xffffffff
 * (represents a plane with all bits "1").
 */
uae_u32 REGPARAM2 picasso_BlitPlanar2Chunky (struct regstruct *regs)
{
    uaecptr bm           =           m68k_areg (regs, 1);
    uaecptr ri           =           m68k_areg (regs, 2);
    unsigned long srcx   = (uae_u16) m68k_dreg (regs, 0);
    unsigned long srcy   = (uae_u16) m68k_dreg (regs, 1);
    unsigned long dstx   = (uae_u16) m68k_dreg (regs, 2);
    unsigned long dsty   = (uae_u16) m68k_dreg (regs, 3);
    unsigned long width  = (uae_u16) m68k_dreg (regs, 4);
    unsigned long height = (uae_u16) m68k_dreg (regs, 5);
    uae_u8 minterm       =           m68k_dreg (regs, 6) & 0xFF;
    uae_u8 mask          =           m68k_dreg (regs, 7) & 0xFF;

    struct RenderInfo local_ri;
    struct BitMap local_bm;

    wgfx_mergeline ();

    if (minterm != 0x0C) {
	P96TRACE (("P96: ERROR - BlitPlanar2Chunky() has minterm 0x%x, which"
		   " I don't handle. Using fall-back routine.\n", minterm));
	return 0;
    }
    if (!CopyRenderInfoStructureA2U (ri, &local_ri)
		|| !CopyBitMapStructureA2U (bm, &local_bm))
	return 0;

    P96TRACE (("P96: BlitPlanar2Chunky(%d, %d, %d, %d, %d, %d) Minterm 0x%x,"
	       " Mask 0x%x, Depth %d\n", srcx, srcy, dstx, dsty, width, height,
	       minterm, mask, local_bm.Depth));
    P96TRACE (("P96: P2C - BitMap has %d BPR, %d rows\n",
	       local_bm.BytesPerRow, local_bm.Rows));

    PlanarToChunky (&local_ri, &local_bm, srcx, srcy, dstx, dsty,
		    width, height, mask);
    if (renderinfo_is_current_screen (&local_ri)) {
	do_blit (&local_ri, GetBytesPerPixel (local_ri.RGBFormat), dstx, dsty,
		 dstx, dsty, width, height, BLIT_SRC, 0);
    }
    return 1;
}

static void PlanarToDirect (struct RenderInfo *ri, struct BitMap *bm,
			    unsigned long srcx,  unsigned long srcy,
			    unsigned long dstx,  unsigned long dsty,
			    unsigned long width, unsigned long height,
			    uae_u8 mask, struct ColorIndexMapping *cim)
{
    int j;
    int bpp = GetBytesPerPixel (ri->RGBFormat);
    uae_u8 *PLANAR[8];
    uae_u8 *image = ri->Memory + dstx * bpp + dsty * ri->BytesPerRow;
    int Depth = bm->Depth;
    unsigned long rows;
    long eol_offset;

    /* Set up our bm->Planes[] pointers to the right horizontal offset */
    for (j = 0; j < Depth; j++) {
	uae_u8 *p = bm->Planes[j];
	if (p != &all_zeros_bitmap && p != &all_ones_bitmap)
	    p += srcx / 8 + srcy * bm->BytesPerRow;
	PLANAR[j] = p;
	if ((mask & (1 << j)) == 0)
	    PLANAR[j] = &all_zeros_bitmap;
    }

    eol_offset = (long) bm->BytesPerRow - (long) ((width + (srcx & 7)) >> 3);
    for (rows = 0; rows < height; rows++, image += ri->BytesPerRow) {
	unsigned long cols;
	uae_u8 *image2 = image;
	unsigned int bitoffs = 7 - (srcx & 7);
	int i;

	for (cols = 0; cols < width; cols++) {
	    int v = 0, k;
	    for (k = 0; k < Depth; k++) {
		if (PLANAR[k] == &all_ones_bitmap)
		    v |= 1 << k;
		else if (PLANAR[k] != &all_zeros_bitmap) {
		    v |= ((*PLANAR[k] >> bitoffs) & 1) << k;
		}
	    }

	    switch (bpp) {
	    case 2:
		do_put_mem_word ((uae_u16 *) image2, cim->Colors[v]);
		image2 += 2;
		break;
	    case 3:
		do_put_mem_byte (image2++, cim->Colors[v] & 0x000000FF);
		do_put_mem_word ((uae_u16 *) image2, (cim->Colors[v] & 0x00FFFF00) >> 8);
		image2 += 2;
		break;
	    case 4:
		do_put_mem_long ((uae_u32 *) image2, cim->Colors[v]);
		image2 += 4;
		break;
	    }
	    bitoffs--;
	    bitoffs &= 7;
	    if (bitoffs == 7) {
		int k;
		for (k = 0; k < Depth; k++) {
		    if (PLANAR[k] != &all_zeros_bitmap && PLANAR[k] != &all_ones_bitmap) {
			PLANAR[k]++;
		    }
		}
	    }
	}

	for (i = 0; i < Depth; i++) {
	    if (PLANAR[i] != &all_zeros_bitmap && PLANAR[i] != &all_ones_bitmap) {
		PLANAR[i] += eol_offset;
	    }
	}
    }
}

/*
 * BlitPlanar2Direct:
 *
 * Synopsis:
 * BlitPlanar2Direct(bi, bm, ri, cim, SrcX, SrcY, DstX, DstY, SizeX, SizeY,
 *                   MinTerm, Mask);
 * Inputs:
 * a0:struct BoardInfo *bi
 * a1:struct BitMap *bm
 * a2:struct RenderInfo *ri
 * a3:struct ColorIndexMapping *cmi
 * d0.w:SrcX
 * d1.w:SrcY
 * d2.w:DstX
 * d3.w:DstY
 * d4.w:SizeX
 * d5.w:SizeY
 * d6.b:MinTerm
 * d7.b:Mask
 *
 * This function is currently used to blit from planar bitmaps within system
 * memory to direct color bitmaps (15, 16, 24 or 32 bit) on the board. Watch
 * out for plane pointers that are 0x00000000 (represents a plane with all
 * bits "0") or 0xffffffff (represents a plane with all bits "1"). The
 * ColorIndexMapping is used to map the color index of each pixel formed by
 * the bits in the bitmap's planes to a direct color value which is written to
 * the destination RenderInfo. The color mask and all colors within the mapping
 * are words, triple bytes or longwords respectively similar to the color
 * values used in FillRect(), BlitPattern() or BlitTemplate().
 */

uae_u32 REGPARAM2 picasso_BlitPlanar2Direct (struct regstruct *regs)
{
    uaecptr bm      =          m68k_areg (regs, 1);
    uaecptr ri      =          m68k_areg (regs, 2);
    uaecptr cim     =          m68k_areg (regs, 3);
    uae_u16 srcx    = (uae_u16)m68k_dreg (regs, 0);
    uae_u16 srcy    = (uae_u16)m68k_dreg (regs, 1);
    uae_u16 dstx    = (uae_u16)m68k_dreg (regs, 2);
    uae_u16 dsty    = (uae_u16)m68k_dreg (regs, 3);
    uae_u16 width   = (uae_u16)m68k_dreg (regs, 4);
    uae_u16 height  = (uae_u16)m68k_dreg (regs, 5);
    uae_u8  minterm =          m68k_dreg (regs, 6);
    uae_u8  Mask; //=          m68k_dreg (regs, 7);

    struct RenderInfo local_ri;
    struct BitMap local_bm;
    struct ColorIndexMapping local_cim;
    int result = 0;

#ifdef JIT
    special_mem |= picasso_is_special_read | picasso_is_special;
#endif

    wgfx_mergeline ();

    if (minterm == BLIT_SRC) {
	if (CopyRenderInfoStructureA2U (ri, &local_ri)
		&& CopyBitMapStructureA2U (bm, &local_bm)) {

	    Mask = 0xFF;
	    CopyColorIndexMappingA2U (cim, &local_cim);

	    P96TRACE (("P96: BlitPlanar2Direct(%d, %d, %d, %d, %d, %d) Minterm"
		       " 0x%x, Mask 0x%x, Depth %d\n", srcx, srcy, dstx, dsty,
		       width, height, minterm, Mask, local_bm.Depth));

	    PlanarToDirect (&local_ri, &local_bm, srcx, srcy, dstx, dsty,
			    width, height, Mask, &local_cim);

	    if (renderinfo_is_current_screen (&local_ri)) {
		do_blit (&local_ri, GetBytesPerPixel (local_ri.RGBFormat),
			 dstx, dsty, dstx, dsty, width, height, BLIT_SRC, 0);
	    }
	    result = 1;
	} else {
	    P96TRACE (("P96: WARNING - BlitPlanar2Direct() failed."
		       " Using fall-back routine.\n"));
	}
    } else {
	P96TRACE (("P96: WARNING - BlitPlanar2Direct () has unhandled op-code"
		   " 0x%x. Using fall-back routine.\n", minterm));
    }
    return result;
}



/* @@@ - Work to be done here!
 *
 * The address is the offset into our Picasso96 frame-buffer (pointed to by
 * gfxmem_start) where the value was put.
 *
 * Porting work: on some machines you may not need these functions, ie. if the
 * memory for the Picasso96 frame-buffer is directly viewable or directly
 * blittable. On Win32 with DirectX, this is not the case. So I provide some
 * write-through functions (as per Mathias' orders!)
 */

/*
 * Check whether data written to the P96 framebuffer needs to be flushed
 * to the real screen
 *
 * addr = address in Amiga memory of data written.
 * size = size of data written in bytes
 */
STATIC_INLINE void flush_write (uaecptr addr, uae_u8 size)
{
    if (picasso_on) {
	/* Is this data on the same framebuffer line as previous
	 * successive writes? */
	if (addr >= currline_start && (addr + size) <= currline_end) {
	    /* Yes. Don't flush now. We'll flush the whole line later */
	    if (addr < currline_min)
		currline_min = addr;
	    if ((addr + size) > currline_max)
		currline_max = addr + size;
	} else {
	    /* Otherwise, this data is on different line, or
	     * we don't have a current line */
	    int line_no;

	    /* If there is a current line, flush it to the display now */
	    if (currline_start != 0xFFFFFFFF)
		 flush_currline ();

	    /* Now we need to set up a new current line, starting with this
	     * write.*/
	    line_no = (addr - picasso96_state.Address)/picasso96_state.BytesPerRow;

	    /* If this line is outside of the p96 screen in the framebuffer
	     * we ignore it */
	    if ((line_no >= picasso96_state.YOffset) && (line_no < picasso96_state.YOffset + picasso96_state.Height)) {
		currline_start = picasso96_state.Address + line_no * picasso96_state.BytesPerRow;
		currline_end   = currline_start + picasso96_state.BytesPerRow;
		currline_min   = addr;
		currline_max   = addr+size;
		currline_y     = line_no;
	    }
	}
    }
}

/*
 * Access routines to the P96 framebuffer
 */
static uae_u32 REGPARAM2 gfxmem_lget (uaecptr addr)
{
    uae_u32 *m;

#ifdef JIT
    special_mem |= picasso_is_special_read;
#endif
    addr -= gfxmem_start;
    addr &= gfxmem_mask;
    m = (uae_u32 *) (gfxmemory + addr);
    return do_get_mem_long (m);
}

static uae_u32 REGPARAM2 gfxmem_wget (uaecptr addr)
{
    uae_u16 *m;

#ifdef JIT
    special_mem |= picasso_is_special_read;
#endif
    addr -= gfxmem_start;
    addr &= gfxmem_mask;
    m = (uae_u16 *) (gfxmemory + addr);
    return do_get_mem_word (m);
}

static uae_u32 REGPARAM2 gfxmem_bget (uaecptr addr)
{
#ifdef JIT
    special_mem |= picasso_is_special_read;
#endif
    addr -= gfxmem_start;
    addr &= gfxmem_mask;
    return gfxmemory[addr];
}

static void REGPARAM2 gfxmem_lput (uaecptr addr, uae_u32 l)
{
    uae_u32 *m;
#ifdef JIT
    special_mem |= picasso_is_special;
#endif
    m = (uae_u32 *)(((addr - gfxmem_start) & gfxmem_mask) + gfxmemory);
    do_put_mem_long (m, l);

    /* Flush long written to the display */
    flush_write (addr, 4);
}

static void REGPARAM2 gfxmem_wput (uaecptr addr, uae_u32 w)
{
    uae_u16 *m;
#ifdef JIT
    special_mem |= picasso_is_special;
#endif
    m = (uae_u16 *)(((addr - gfxmem_start) & gfxmem_mask) + gfxmemory);
    do_put_mem_word (m, (uae_u16)w);

    /* Flush word written to the display */
    flush_write (addr, 2);
}

static void REGPARAM2 gfxmem_bput (uaecptr addr, uae_u32 b)
{
    uae_u8 *m;
#ifdef JIT
    special_mem |= picasso_is_special;
#endif
    m = (uae_u8 *)(((addr - gfxmem_start) & gfxmem_mask) + gfxmemory);
    *m = (uae_u8) b;

    /* Flush byte written to the display */
    flush_write (addr, 1);
}

static int REGPARAM2 gfxmem_check (uaecptr addr, uae_u32 size)
{
    addr -= gfxmem_start;
    addr &= gfxmem_mask;
    return (addr + size) < allocated_gfxmem;
}

static uae_u8 REGPARAM2 *gfxmem_xlate (uaecptr addr)
{
    addr -= gfxmem_start;
    addr &= gfxmem_mask;
    return gfxmemory + addr;
}

addrbank gfxmem_bank = {
    gfxmem_lget, gfxmem_wget, gfxmem_bget,
    gfxmem_lput, gfxmem_wput, gfxmem_bput,
    gfxmem_xlate, gfxmem_check, NULL
};

int picasso_display_mode_index (uae_u32 x, uae_u32 y, uae_u32 d)
{
    int i;
    for (i = 0; i < mode_count; i++) {
	if (DisplayModes[i].res.width == x
	    && DisplayModes[i].res.height == y
	    && DisplayModes[i].depth == (int)d)
	    break;
    }
    if (i == mode_count)
	i = -1;
    return i;
}

static int resolution_compare (const void *a, const void *b)
{
    struct PicassoResolution *ma = (struct PicassoResolution *) a;
    struct PicassoResolution *mb = (struct PicassoResolution *) b;
    if (ma->res.width > mb->res.width)
	return -1;
    if (ma->res.width < mb->res.width)
	return 1;
    if (ma->res.height > mb->res.height)
	return -1;
    if (ma->res.height < mb->res.height)
	return 1;
    return ma->depth - mb->depth;
}


/****************************************************************************
 *
 * Built-in uaegfx.card (v16, after WinUAE uaegfx_card_install() /
 * inituaegfxfuncs())
 *
 * E-UAE used to need the uaegfx.card file of the Picasso96 package, which
 * calls the emulation through uaelib (functions 16-40 in uaelib.c). Now the
 * card is a library in the boot ROM area: a Resident (RTF_AUTOINIT) added in
 * filesys_diagentry() creates "uaegfx.card" at boot, so rtg.library finds it
 * in memory and never loads a card file from LIBS:Picasso96. Only the
 * Picasso96 system (rtg.library) and the monitor file DEVS:Monitors/UAEGFX
 * are needed on the Amiga side.
 *
 * InitCard() of the built-in card fills all function slots of the BoardInfo
 * with small 68k stubs that call the emulation through traps. The stubs are
 * written once into the rtarea by uaegfx_install_code() (rtarea_init()).
 *
 * The uaelib path stays for the old card file; it is only used when the
 * built-in card is not in memory (e.g. no graphics card RAM at boot).
 ****************************************************************************/

#include "autoconf.h"
#include "traps.h"

#define UAEGFX_VERSION  3
#define UAEGFX_REVISION 5

static uaecptr uaegfx_base;            /* library base on the Amiga side, 0 = not created */
static int     uaegfx_interrupt_on;    /* SetInterrupt (TRUE) */
static uaecptr uaegfx_resname, uaegfx_resid, uaegfx_prefix, uaegfx_vblankname;
static uaecptr uaegfx_initstruct;      /* rt_Init for RTF_AUTOINIT */
static uaecptr uaegfx_rts;             /* plain RTS */

enum {
    SLOT_DEFAULT, /* trap; d0 == 0 -> jump to the default function of the slot */
    SLOT_TRAP,    /* trap, return value in d0 */
    SLOT_PASS,    /* jump to the default function of the slot */
    SLOT_FIXED,   /* fixed 68k code */
    SLOT_NONE     /* RTS */
};

struct uaegfx_slot {
    int     offset;     /* PSSO_BoardInfo_xxx */
    int     type;
    uaecptr code;       /* 68k address, set by uaegfx_install_code() */
    int     need;       /* 0 = always, 1 = hardware sprite, 2 = vblank interrupt */
};

#define MAX_UAEGFX_SLOTS 48
static struct uaegfx_slot uaegfx_slots[MAX_UAEGFX_SLOTS];
static int uaegfx_nslots;

/* Simple traps get the CPU registers; TrapContext starts with them. */
#define P96TRAP(name) \
    static uae_u32 REGPARAM2 trap_##name (TrapContext *ctx) { return picasso_##name (&ctx->regs); }

P96TRAP (FillRect)
P96TRAP (InvertRect)
P96TRAP (BlitRect)
P96TRAP (BlitRectNoMaskComplete)
P96TRAP (BlitRectTransparent)
P96TRAP (BlitPattern)
P96TRAP (BlitTemplate)
P96TRAP (BlitPlanar2Chunky)
P96TRAP (BlitPlanar2Direct)
P96TRAP (SetSwitch)
P96TRAP (SetColorArray)
P96TRAP (SetDAC)
P96TRAP (SetGC)
P96TRAP (SetPanning)
P96TRAP (SetDisplay)
P96TRAP (CalculateBytesPerRow)
P96TRAP (SetSprite)
P96TRAP (SetSpritePosition)
P96TRAP (SetSpriteImage)
P96TRAP (SetSpriteColor)

/*
 * SetInterrupt:
 * a0: struct BoardInfo *bi
 * d0: BOOL state
 * Switches the VBLANK interrupt of the card on or off. While it is on,
 * picasso_trigger_vblank() makes the card's VERTB server Cause() the
 * SoftInterrupt of the BoardInfo once per frame.
 */
static uae_u32 REGPARAM2 trap_SetInterrupt (TrapContext *ctx)
{
    uaegfx_interrupt_on = m68k_dreg (&ctx->regs, 0) ? 1 : 0;
    return uaegfx_interrupt_on;
}

void picasso_trigger_vblank (void)
{
    uaecptr bi;

    if (!uaegfx_base || !uaegfx_interrupt_on || !currprefs.rtg_hardwareinterrupt)
	return;
    bi = get_long (uaegfx_base + CARD_BOARDINFO);
    if (!bi)
	return;
    put_long (uaegfx_base + CARD_IRQPTR, bi + PSSO_BoardInfo_SoftInterrupt);
    /* Bit 0 of the first byte: the VERTB server of the card (runs in the
     * Amiga's own vertical blank) clears it and calls Cause(). */
    put_byte (uaegfx_base + CARD_IRQFLAG, 1);
}

static void add_slot (int offset, int type, uaecptr code, int need)
{
    if (uaegfx_nslots < MAX_UAEGFX_SLOTS) {
	uaegfx_slots[uaegfx_nslots].offset = offset;
	uaegfx_slots[uaegfx_nslots].type   = type;
	uaegfx_slots[uaegfx_nslots].code   = code;
	uaegfx_slots[uaegfx_nslots].need   = need;
	uaegfx_nslots++;
    }
}

/* trap; if it returns 0, continue in the default function (WinUAE RTGCALL):
 *   movem.l d0,-(sp) / trap / tst.l d0 / movem.l (sp)+,d0 / bne.s rts /
 *   move.l default(a0),-(sp) / rts */
static void stub_default (int offset, int defoffset, TrapHandler h)
{
    add_slot (offset, SLOT_DEFAULT, here (), 0);
    dl (0x48e78000);
    calltrap (deftrap (h));
    dw (0x4a80);
    dl (0x4cdf0001);
    dw (0x6604);
    dw (0x2f28);
    dw (defoffset);
    dw (RTS);
}

static void stub_trap (int offset, TrapHandler h, int need)
{
    add_slot (offset, SLOT_TRAP, here (), need);
    calltrap (deftrap (h));
    dw (RTS);
}

/* only the default function: move.l default(a0),-(sp) / rts */
static void stub_pass (int offset, int defoffset)
{
    add_slot (offset, SLOT_PASS, here (), 0);
    dw (0x2f28);
    dw (defoffset);
    dw (RTS);
}

static uae_u32 REGPARAM2 trap_gfx_open (TrapContext *ctx)
{
    put_word (uaegfx_base + 32, get_word (uaegfx_base + 32) + 1);   /* lib_OpenCnt */
    return uaegfx_base;
}

static uae_u32 REGPARAM2 trap_gfx_close (TrapContext *ctx)
{
    put_word (uaegfx_base + 32, get_word (uaegfx_base + 32) - 1);
    return 0;
}

static uae_u32 REGPARAM2 trap_gfx_expunge (TrapContext *ctx)
{
    return 0;    /* never expunged (lives in the boot ROM) */
}

static uae_u32 REGPARAM2 trap_FindCard (TrapContext *ctx)
{
    return picasso_FindCard (&ctx->regs);
}

/*
 * Init routine of the library (RTF_AUTOINIT): d0 = library base, a6 = exec.
 * Fills the card fields and the VBLANK interrupt structure; the 68k code
 * around this trap then calls AddIntServer (INTB_VERTB).
 */
static uae_u32 REGPARAM2 trap_gfx_init (TrapContext *ctx)
{
    uaecptr base = m68k_dreg (&ctx->regs, 0);
    uaecptr irq  = base + CARD_VBLANKIRQ;
    uaecptr c    = base + CARD_IRQCODE;
    uaecptr exec = get_long (4);

    uaegfx_base = base;
    uaegfx_interrupt_on = 0;

    put_long (base + CARD_EXECBASE, exec);
    put_long (base + CARD_IRQEXECBASE, exec);
    put_long (base + CARD_NAME, uaegfx_resname);
    put_long (base + CARD_BOARDINFO, 0);
    put_long (base + CARD_IRQFLAG, 0);

    /* struct Interrupt: ln_Type NT_INTERRUPT, ln_Pri 5, ln_Name, is_Data, is_Code */
    put_word (irq + 8, 0x0205);
    put_long (irq + 10, uaegfx_vblankname);
    put_long (irq + 14, base + CARD_IRQFLAG);
    put_long (irq + 18, c);

    /* Server code (a1 = is_Data = CARD_IRQFLAG):
     *   bclr #0,(a1) / beq.s out / move.l 8(a1),a6 / move.l 4(a1),a1 /
     *   jsr Cause(a6) / out: moveq #0,d0 / rts */
    put_long (c, 0x08910000); c += 4;
    put_word (c, 0x670e);     c += 2;
    put_long (c, 0x2c690008); c += 4;
    put_long (c, 0x22690004); c += 4;
    put_long (c, 0x4eaeff4c); c += 4;
    put_word (c, 0x7000);     c += 2;
    put_word (c, RTS);

    write_log ("uaegfx.card %d.%d (built-in) init @$%08x\n", UAEGFX_VERSION, UAEGFX_REVISION, base);
    return base;
}

/*
 * InitCard of the built-in card:
 * a0: struct BoardInfo *bi, a1: tooltypes
 */
static uae_u32 REGPARAM2 trap_InitCard (TrapContext *ctx)
{
    uaecptr bi = m68k_areg (&ctx->regs, 0);
    uae_u32 flags;
    int i;

    init_card_modes (bi);

    put_long (bi + PSSO_BoardInfo_BoardName, uaegfx_prefix);
    put_long (bi + PSSO_BoardInfo_GraphicsControllerType, 0);
    put_long (bi + PSSO_BoardInfo_PaletteChipType, 0);
    put_long (bi + PSSO_BoardInfo_MemoryClock, 200000000);
    for (i = 0; i < MAXMODES; i++) {
	put_long (bi + PSSO_BoardInfo_PixelClockCount + i * 4, 1);       /* only one clock */
	put_word (bi + PSSO_BoardInfo_MaxHorValue + i * 2, 0x4000);
	put_word (bi + PSSO_BoardInfo_MaxVerValue + i * 2, 0x4000);
    }

    /* user flags (upper half, e.g. NOBLITTER from the monitor tooltypes) stay */
    flags = get_long (bi + PSSO_BoardInfo_Flags) & 0xffff0000;
    if (flags & BIF_NOBLITTER)
	write_log ("P96: Blitter disabled in DEVS:Monitors/UAEGFX!\n");
    else
	flags |= BIF_BLITTER;
    flags |= BIF_NOMEMORYMODEMIX | BIF_GRANTDIRECTACCESS | BIF_INDISPLAYCHAIN;

    memset (&picasso96_sprite, 0, sizeof picasso96_sprite);
    picasso96_sprite.enabled = currprefs.rtg_hardwaresprite ? 1 : 0;
    if (picasso96_sprite.enabled && !gfx_rtg_sprite_supported ()) {
	/* 8 bit host screen: no composite, Picasso96 uses its software pointer */
	write_log ("P96: hardware sprite not offered (8 bit host screen)\n");
	picasso96_sprite.enabled = 0;
    }
    if (picasso96_sprite.enabled) {
	flags |= BIF_HARDWARESPRITE;
	put_word (bi + PSSO_BoardInfo_SoftSpriteFlags, 0);
    } else
	put_word (bi + PSSO_BoardInfo_SoftSpriteFlags, picasso96_pixel_format);
    if (currprefs.rtg_hardwareinterrupt)
	flags |= BIF_VBLANKINTERRUPT;
    put_long (bi + PSSO_BoardInfo_Flags, flags);

    for (i = 0; i < uaegfx_nslots; i++) {
	struct uaegfx_slot *sl = &uaegfx_slots[i];
	if (sl->need == 1 && !picasso96_sprite.enabled)
	    continue;
	if (sl->need == 2 && !currprefs.rtg_hardwareinterrupt)
	    continue;
	put_long (bi + sl->offset, sl->code);
    }

    if (uaegfx_base) {
	put_long (uaegfx_base + CARD_BOARDINFO, bi);
	/* as in WinUAE: the interrupt structure of the card for rtg.library */
	for (i = 0; i < 22; i++)
	    put_byte (bi + PSSO_BoardInfo_HardInterrupt + i, get_byte (uaegfx_base + CARD_VBLANKIRQ + i));
    }

    write_log ("P96: built-in uaegfx.card, hardware sprite %s, vblank interrupt %s\n",
	       picasso96_sprite.enabled ? "on" : "off",
	       currprefs.rtg_hardwareinterrupt ? "on" : "off");
    return -1;
}

/*
 * Write the 68k code of the card into the rtarea. Called from rtarea_init
 * (main.c) on every start of the emulation, after the traps were reset.
 */
void uaegfx_install_code (void)
{
    uaecptr openfunc, closefunc, expungefunc, findcardfunc, initcardfunc;
    uaecptr functable, datatable, initcode;

    uaegfx_nslots = 0;
    uaegfx_base   = 0;

    uaegfx_resname    = ds ("uaegfx.card");
    uaegfx_resid      = ds ("UAE Graphics Card 3.5 (E-UAE built-in)");
    uaegfx_prefix     = ds ("UAE");
    uaegfx_vblankname = ds ("UAE Graphics Card VBLANK");

    uaegfx_rts = here ();
    dw (RTS);

    /* ResolvePixelClock: move.l d0,gmi_PixelClock(a1) / moveq #0,d0 /
     * move.b #98,gmi_Numerator(a1) / move.b #14,gmi_Denominator(a1) / rts */
    add_slot (PSSO_BoardInfo_ResolvePixelClock, SLOT_FIXED, here (), 0);
    dl (0x2340002c);
    dw (0x7000);
    dl (0x137c0062); dw (0x002a);
    dl (0x137c000e); dw (0x002b);
    dw (RTS);

    /* GetPixelClock: move.l #clock,d0 / rts */
    add_slot (PSSO_BoardInfo_GetPixelClock, SLOT_FIXED, here (), 0);
    dw (0x203c);
    dl (100227260);
    dw (RTS);

    /* CalculateMemory: move.l a1,d0 / rts (no planar modes) */
    add_slot (PSSO_BoardInfo_CalculateMemory, SLOT_FIXED, here (), 0);
    dw (0x2009);
    dw (RTS);

    /* GetCompatibleFormats: all chunky formats can coexist */
    add_slot (PSSO_BoardInfo_GetCompatibleFormats, SLOT_FIXED, here (), 0);
    dw (0x203c);
    dl (RGBMASK_8BIT | RGBMASK_15BIT | RGBMASK_16BIT | RGBMASK_24BIT | RGBMASK_32BIT);
    dw (RTS);

    add_slot (PSSO_BoardInfo_SetClock,         SLOT_NONE, uaegfx_rts, 0);
    add_slot (PSSO_BoardInfo_SetMemoryMode,    SLOT_NONE, uaegfx_rts, 0);
    add_slot (PSSO_BoardInfo_SetWriteMask,     SLOT_NONE, uaegfx_rts, 0);
    add_slot (PSSO_BoardInfo_SetClearMask,     SLOT_NONE, uaegfx_rts, 0);
    add_slot (PSSO_BoardInfo_SetReadPlane,     SLOT_NONE, uaegfx_rts, 0);
    add_slot (PSSO_BoardInfo_WaitVerticalSync, SLOT_NONE, uaegfx_rts, 0);
    add_slot (PSSO_BoardInfo_WaitBlitter,      SLOT_NONE, uaegfx_rts, 0);

    stub_default (PSSO_BoardInfo_BlitPlanar2Direct,      PSSO_BoardInfo_BlitPlanar2DirectDefault,      trap_BlitPlanar2Direct);
    stub_default (PSSO_BoardInfo_FillRect,               PSSO_BoardInfo_FillRectDefault,               trap_FillRect);
    stub_default (PSSO_BoardInfo_BlitRect,               PSSO_BoardInfo_BlitRectDefault,               trap_BlitRect);
    stub_default (PSSO_BoardInfo_BlitPlanar2Chunky,      PSSO_BoardInfo_BlitPlanar2ChunkyDefault,      trap_BlitPlanar2Chunky);
    stub_default (PSSO_BoardInfo_BlitTemplate,           PSSO_BoardInfo_BlitTemplateDefault,           trap_BlitTemplate);
    stub_default (PSSO_BoardInfo_InvertRect,             PSSO_BoardInfo_InvertRectDefault,             trap_InvertRect);
    stub_default (PSSO_BoardInfo_BlitRectNoMaskComplete, PSSO_BoardInfo_BlitRectNoMaskCompleteDefault, trap_BlitRectNoMaskComplete);
    stub_default (PSSO_BoardInfo_BlitRectTransparent,    PSSO_BoardInfo_BlitRectTransparentDefault,    trap_BlitRectTransparent);
    stub_default (PSSO_BoardInfo_BlitPattern,            PSSO_BoardInfo_BlitPatternDefault,            trap_BlitPattern);

    stub_trap (PSSO_BoardInfo_SetSwitch,            trap_SetSwitch, 0);
    stub_trap (PSSO_BoardInfo_SetColorArray,        trap_SetColorArray, 0);
    stub_trap (PSSO_BoardInfo_SetDAC,               trap_SetDAC, 0);
    stub_trap (PSSO_BoardInfo_SetGC,                trap_SetGC, 0);
    stub_trap (PSSO_BoardInfo_SetPanning,           trap_SetPanning, 0);
    stub_trap (PSSO_BoardInfo_SetDisplay,           trap_SetDisplay, 0);
    stub_trap (PSSO_BoardInfo_CalculateBytesPerRow, trap_CalculateBytesPerRow, 0);

    stub_trap (PSSO_BoardInfo_SetSprite,            trap_SetSprite, 1);
    stub_trap (PSSO_BoardInfo_SetSpritePosition,    trap_SetSpritePosition, 1);
    stub_trap (PSSO_BoardInfo_SetSpriteImage,       trap_SetSpriteImage, 1);
    stub_trap (PSSO_BoardInfo_SetSpriteColor,       trap_SetSpriteColor, 1);

    stub_trap (PSSO_BoardInfo_SetInterrupt,         trap_SetInterrupt, 2);

    stub_pass (PSSO_BoardInfo_ScrollPlanar, PSSO_BoardInfo_ScrollPlanarDefault);
    stub_pass (PSSO_BoardInfo_UpdatePlanar, PSSO_BoardInfo_UpdatePlanarDefault);
    stub_pass (PSSO_BoardInfo_DrawLine,     PSSO_BoardInfo_DrawLineDefault);

    /* Library functions */
    openfunc     = here (); calltrap (deftrap (trap_gfx_open));    dw (RTS);
    closefunc    = here (); calltrap (deftrap (trap_gfx_close));   dw (RTS);
    expungefunc  = here (); calltrap (deftrap (trap_gfx_expunge)); dw (RTS);
    findcardfunc = here (); calltrap (deftrap (trap_FindCard));    dw (RTS);
    initcardfunc = here (); calltrap (deftrap (trap_InitCard));    dw (RTS);

    functable = here ();
    dl (openfunc);
    dl (closefunc);
    dl (expungefunc);
    dl (EXPANSION_nullfunc);
    dl (findcardfunc);
    dl (initcardfunc);
    dl (0xFFFFFFFF);

    /* DataTable (exec InitStruct) */
    datatable = here ();
    dw (0xE000); dw (0x0008); dw (0x0900);         /* INITBYTE LN_TYPE, NT_LIBRARY */
    dw (0xE000); dw (0x0009); dw (0xCE00);         /* INITBYTE LN_PRI, -50 */
    dw (0xC000); dw (0x000A); dl (uaegfx_resname); /* INITLONG LN_NAME */
    dw (0xE000); dw (0x000E); dw (0x0600);         /* INITBYTE LIB_FLAGS, SUMUSED|CHANGED */
    dw (0xD000); dw (0x0014); dw (UAEGFX_VERSION); /* INITWORD LIB_VERSION */
    dw (0xD000); dw (0x0016); dw (UAEGFX_REVISION);/* INITWORD LIB_REVISION */
    dw (0xC000); dw (0x0018); dl (uaegfx_resid);   /* INITLONG LIB_IDSTRING */
    dw (0x0000);

    /* Init routine: d0 = library base, a6 = SysBase
     *   move.l d0,-(sp) / trap gfx_init / move.l (sp),a1 /
     *   lea CARD_VBLANKIRQ(a1),a1 / moveq #5,d0 / jsr AddIntServer(a6) /
     *   move.l (sp)+,d0 / rts */
    initcode = here ();
    dw (0x2f00);
    calltrap (deftrap (trap_gfx_init));
    dw (0x2257);
    dw (0x43e9); dw (CARD_VBLANKIRQ);
    dw (0x7005);
    dl (0x4eaeff58);
    dw (0x201f);
    dw (RTS);

    /* rt_Init for RTF_AUTOINIT: dataSize, functions, data, init routine */
    align (4);
    uaegfx_initstruct = here ();
    dl (CARD_SIZEOF);
    dl (functable);
    dl (datatable);
    dl (initcode);
}

/*
 * Add the Resident of the built-in card behind the other ROMTags of the
 * filesys DiagArea (filesys_diagentry() calls InitResident() for it).
 * Only with graphics card memory.
 */
uaecptr uaegfx_add_resident (uaecptr resaddr)
{
    if (!allocated_gfxmem || !uaegfx_initstruct)
	return resaddr;

    put_word (resaddr + 0x0, 0x4AFC);                /* RTC_MATCHWORD */
    put_long (resaddr + 0x2, resaddr);               /* rt_MatchTag */
    put_long (resaddr + 0x6, resaddr + 0x1A);        /* rt_EndSkip */
    put_word (resaddr + 0xA, 0x8100 | UAEGFX_VERSION); /* RTF_AUTOINIT|RTF_COLDSTART, version */
    put_word (resaddr + 0xC, 0x0900);                /* NT_LIBRARY, pri 0 */
    put_long (resaddr + 0xE, uaegfx_resname);
    put_long (resaddr + 0x12, uaegfx_resid);
    put_long (resaddr + 0x16, uaegfx_initstruct);
    return resaddr + 0x1A;
}

int uaegfx_builtin_active (void)
{
    return uaegfx_base != 0;
}

/* Call this function first, near the beginning of code flow
 * NOTE: Don't stuff it in InitGraphics() which seems reasonable...
 * Instead, put it in customreset() for safe-keeping.  */
void InitPicasso96 (void)
{
    static int first_time = 1;

    memset (&picasso96_state, 0, sizeof (struct picasso96_state_struct));
    /* Reset: the card library and its interrupt are created again at the
     * next boot (filesys_diagentry); until then nothing may be written to
     * the old library memory. */
    uaegfx_base = 0;
    uaegfx_interrupt_on = 0;
    memset (&picasso96_sprite, 0, sizeof picasso96_sprite);

    if (first_time) {
	int i;

	for (i = 0; i < 256; i++) {
	    p2ctab[i][0] = (((i & 128) ? 0x01000000 : 0)
			  | ((i & 64)  ? 0x010000 : 0)
			  | ((i & 32)  ? 0x0100 : 0)
			  | ((i & 16)  ? 0x01 : 0));
	    p2ctab[i][1] = (((i & 8)   ? 0x01000000 : 0)
			  | ((i & 4)   ? 0x010000 : 0)
			  | ((i & 2)   ? 0x0100 : 0)
			  | ((i & 1)   ? 0x01 : 0));
	}
	mode_count = DX_FillResolutions (&picasso96_pixel_format);

	qsort (DisplayModes, mode_count, sizeof (struct PicassoResolution),
	       resolution_compare);

#if 0
	/* Work-around for possible P96 bug. A8R8G8B8 modes have
	 * palette emulation issues. Tell the world we have a
	 * a BGRA mode instead (and we'll byte-swap all pixels output).
	 */

	if (picasso_vidinfo.rgbformat == RGBFB_A8R8G8B8)
	{
	    picasso_vidinfo.rgbformat = RGBFB_B8G8R8A8;
	    picasso96_pixel_format &= RGBFF_CHUNKY;
	    picasso96_pixel_format |= 1 << picasso_vidinfo.rgbformat;
	    need_argb32_hack = 1;
	   write_log ("Enabling argb32 byte-swapping for P96.\n");
	}
#endif

	for (i = 0; i < mode_count; i++)
	{
		sprintf (DisplayModes[i].name, "%dx%d, %d-bit, %d Hz",
		     DisplayModes[i].res.width, DisplayModes[i].res.height,
		     DisplayModes[i].depth * 8, DisplayModes[i].refresh);

		switch (DisplayModes[i].depth)
		{
			case 1:
				{
					if (DisplayModes[i].res.width > chunky.width)
						chunky.width = DisplayModes[i].res.width;
					if (DisplayModes[i].res.height > chunky.height)
						chunky.height = DisplayModes[i].res.height;
				}
				break;

			case 2:
				{
					if (DisplayModes[i].res.width > hicolour.width)
						hicolour.width = DisplayModes[i].res.width;
					if (DisplayModes[i].res.height > hicolour.height)
						hicolour.height = DisplayModes[i].res.height;
				}
				break;

			case 3:
				{
					if (DisplayModes[i].res.width > truecolour.width)
						truecolour.width = DisplayModes[i].res.width;
					if (DisplayModes[i].res.height > truecolour.height)
						truecolour.height = DisplayModes[i].res.height;
				}
				break;

			case 4:
				{
					if (DisplayModes[i].res.width > alphacolour.width)
						alphacolour.width = DisplayModes[i].res.width;
					if (DisplayModes[i].res.height > alphacolour.height)
						alphacolour.height = DisplayModes[i].res.height;
				}
				break;

			default:

				printf("unexpected depth :-(\n");
		}
	}
	ShowSupportedResolutions ();

	first_time = 0;
    }
}

#endif
