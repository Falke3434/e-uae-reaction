
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <intuition/intuition.h>
#include <proto/graphics.h>
#include <proto/layers.h>

#ifdef PICASSO96_SUPPORTED
#include "sysconfig.h"
#include "sysdeps.h"
#include "picasso96.h"
extern int screen_is_picasso;
#endif

#define ASM_SYM_FOR_FUNC(x) 

#define SIZEOF_SHORT 2
#define SIZEOF_INT 4
#define SIZEOF_VOID_P 4

#include "uae_types.h"
#include "xwin.h"

static struct Rectangle rect;
static struct Rectangle fullscreen_rect;
struct Rectangle *rect_ptr = &rect;


struct BackFillArgs
{
	int __place_holder__;
};

typedef struct CompositeHookData_s {
	struct BitMap *srcBitMap; // The source bitmap
	int32 srcWidth, srcHeight; // The source dimensions
	int32 offsetX, offsetY; // The offsets to the destination area relative to the window's origin
	int32 scaleX, scaleY; // The scale factors
	uint32 retCode; // The return code from CompositeTags()
} CompositeHookData;

;
static struct Hook hook;
static CompositeHookData hookData;

void BackFill_Func(struct RastPort *ArgRP, struct BackFillArgs *MyArgs);
void set_target_hookData( void );

extern struct Window    *W;

extern struct RastPort comp_aga_RP;
extern struct RastPort comp_p96_RP;

struct Hook BackFill_Hook =
{
	{NULL, NULL},
	(HOOKFUNC) &BackFill_Func,
	NULL,
	NULL
};

static ULONG compositeHookFunc(
			struct Hook *hook, 
			struct RastPort *rastPort, 
			struct BackFillMessage *msg)
 {

	CompositeHookData *hookData = (CompositeHookData*)hook->h_Data;

	hookData->retCode = CompositeTags(
		COMPOSITE_Src, 
			hookData->srcBitMap, 
			rastPort->BitMap,
		COMPTAG_SrcWidth,   hookData->srcWidth,
		COMPTAG_SrcHeight,  hookData->srcHeight,
		COMPTAG_ScaleX, 	hookData->scaleX,
		COMPTAG_ScaleY, 	hookData->scaleY,
		COMPTAG_OffsetX,    msg->Bounds.MinX - (msg->OffsetX - hookData->offsetX),
		COMPTAG_OffsetY,    msg->Bounds.MinY - (msg->OffsetY - hookData->offsetY),
		COMPTAG_DestX,      msg->Bounds.MinX,
		COMPTAG_DestY,      msg->Bounds.MinY,
		COMPTAG_DestWidth,  msg->Bounds.MaxX - msg->Bounds.MinX + 1,
		COMPTAG_DestHeight, msg->Bounds.MaxY - msg->Bounds.MinY + 1,
//		COMPTAG_Flags,      COMPFLAG_SrcFilter | COMPFLAG_IgnoreDestAlpha | COMPFLAG_HardwareOnly,

		COMPTAG_SrcAlpha, COMP_FLOAT_TO_FIX(1.0f),
		/* Kein COMPFLAG_HardwareOnly: ohne Hardware-Compositing (z. B. unter
		 * QEMU/Pegasos2) schlug CompositeTags() sonst still fehl und das
		 * Emulatorfenster blieb leer. So faellt es auf Software zurueck. */
		COMPTAG_Flags,      COMPFLAG_SrcAlphaOverride | COMPFLAG_IgnoreDestAlpha,

		TAG_END);

	{
		static int comp_err_reported = 0;
		if (hookData->retCode != COMPERR_Success && comp_err_reported < 5)
		{
			comp_err_reported++;
			printf("AMIGFX: CompositeTags() fehlgeschlagen, Fehlercode %ld\n", (long) hookData->retCode);
		}
	}

	return 0;
}

void BackFill_Func(struct RastPort *ArgRP, struct BackFillArgs *MyArgs)
{
	struct Rectangle rect;
	if (W)
	{
		set_target_hookData();

		register struct RastPort *RPort = W->RPort;

		LockLayer(0, RPort->Layer);
		DoHookClipRects(&hook, RPort, rect_ptr);
		UnlockLayer( RPort->Layer);
	}
}

void update_fullscreen_rect( int aspect )
{
	int SHeight = W ?  W -> WScreen -> Height : 0 ; 
	int SWidth = W ?  W -> WScreen -> Width : 0 ; 
	int Width;

	if (screen_is_picasso)
	{
		Width = aspect ? picasso_vidinfo.width * SHeight / picasso_vidinfo.height  :  SWidth;
	}
	else
	{
		Width = aspect ? gfxvidinfo.width * SHeight / gfxvidinfo.height : SWidth;
	}

	if (Width > SWidth ) Width = SWidth;	// clamp it max width

	fullscreen_rect.MinY = 0;
	fullscreen_rect.MinX = (SWidth - Width) / 2;	// calulate edge, two edges.
//	fullscreen_rect.MaxX = fullscreen_rect.MinX + Width - 1;

	fullscreen_rect.MaxX = SWidth - 1;
	fullscreen_rect.MaxY = SHeight - 1;

#if 0
	Printf("** aspect: %ld, min x: %ld, y: %ld - max x: %ld, y: %ld **\n",
		aspect,
		fullscreen_rect.MinX,
		fullscreen_rect.MinY,
		fullscreen_rect.MaxX,
		fullscreen_rect.MaxY );
#endif
}

 	float lscaleX,lscaleY;

void set_target_hookData( void )
{
 	float scaleX,scaleY;
/*
	if (W->BorderTop)	// this window, is a wb window.
	{
*/
	 	rect.MinX = W->BorderLeft;
 		rect.MinY = W->BorderTop;
 		rect.MaxX = W->Width - W->BorderRight - 1;
 		rect.MaxY = W->Height - W->BorderBottom - 1;
		rect_ptr = &rect;
/*
	}
	else
	{
		update_fullscreen_rect( 0 );
		rect_ptr = &fullscreen_rect;
	}
*/
 	float destWidth = rect_ptr -> MaxX - rect_ptr -> MinX + 1;
	float destHeight = rect_ptr -> MaxY - rect_ptr -> MinY + 1;

	if (screen_is_picasso)
	{
		hookData.srcWidth = picasso_vidinfo.width;
		hookData.srcHeight = picasso_vidinfo.height;
		hookData.srcBitMap = comp_p96_RP.BitMap;
	}
	else
	{
		hookData.srcWidth = gfxvidinfo.width;
		hookData.srcHeight = gfxvidinfo.height;
		hookData.srcBitMap = comp_aga_RP.BitMap;
	}

 	scaleX = (destWidth + 0.5f) / hookData.srcWidth;
 	scaleY = (destHeight + 0.5f) / hookData.srcHeight;

	uint32 depth = GetBitMapAttr( hookData.srcBitMap ,BMA_DEPTH );

/*
	if ((lscaleX != scaleX) && (lscaleY != scaleY))
	{
		printf("src: d: %d w: %d, h: %d\n", depth, hookData.srcWidth, hookData.srcHeight );
		printf("dst: %0.2f, %0.2f\n", destWidth, destHeight );
		printf("Scale %0.2f  - %0.2f\n", scaleX, scaleY);
		lscaleX = scaleX;
		lscaleY = scaleY;
	}
*/

	hookData.offsetX = rect_ptr -> MinX ;
	hookData.offsetY = rect_ptr -> MinY;
	hookData.scaleX = COMP_FLOAT_TO_FIX(scaleX);
	hookData.scaleY = COMP_FLOAT_TO_FIX(scaleY);
	hookData.retCode = COMPERR_Success;

	hook.h_Entry = (HOOKFUNC) compositeHookFunc;
	hook.h_Data = &hookData;
}




#ifdef PICASSO96_SUPPORTED
/*
 * v16: hardware sprite (mouse pointer) of the uaegfx card. After
 * BackFill_Func() has composited the RTG picture into the window, the
 * pointer image (32 bit ARGB, alpha 0 = transparent) is composited on top
 * with the same scale factors. The RTG picture itself (comp_p96_RP) is never
 * changed, so nothing has to be restored when the pointer moves.
 */
typedef struct SpriteHookData_s {
	struct BitMap *bm;
	int32 w, h;          /* used part of the sprite bitmap */
	int32 x, y;          /* position in RTG pixels */
} SpriteHookData;

void BackFill_Sprite(struct BitMap *bm, int w, int h, int x, int y);
static struct Hook spriteHook;
static SpriteHookData spriteData;

static ULONG spriteHookFunc(struct Hook *hook, struct RastPort *rastPort, struct BackFillMessage *msg)
{
	SpriteHookData *sd = (SpriteHookData *) hook->h_Data;
	float sx = (float) hookData.scaleX / (float) COMP_FIX_ONE;
	float sy = (float) hookData.scaleY / (float) COMP_FIX_ONE;

	/* bitmap coordinates of the window's layer origin */
	int32 lx = msg->Bounds.MinX - msg->OffsetX;
	int32 ly = msg->Bounds.MinY - msg->OffsetY;

	/* RTG picture area in the bitmap */
	int32 ax0 = lx + hookData.offsetX;
	int32 ay0 = ly + hookData.offsetY;
	int32 ax1 = ax0 + (int32) (hookData.srcWidth * sx);
	int32 ay1 = ay0 + (int32) (hookData.srcHeight * sy);

	/* sprite rectangle in the bitmap */
	int32 ox = ax0 + (int32) (sd->x * sx);
	int32 oy = ay0 + (int32) (sd->y * sy);
	int32 x0 = ox, y0 = oy;
	int32 x1 = ox + (int32) (sd->w * sx + 0.5f);
	int32 y1 = oy + (int32) (sd->h * sy + 0.5f);

	if (x0 < ax0) x0 = ax0;
	if (y0 < ay0) y0 = ay0;
	if (x1 > ax1) x1 = ax1;
	if (y1 > ay1) y1 = ay1;
	if (x0 < msg->Bounds.MinX) x0 = msg->Bounds.MinX;
	if (y0 < msg->Bounds.MinY) y0 = msg->Bounds.MinY;
	if (x1 > msg->Bounds.MaxX + 1) x1 = msg->Bounds.MaxX + 1;
	if (y1 > msg->Bounds.MaxY + 1) y1 = msg->Bounds.MaxY + 1;
	if (x0 >= x1 || y0 >= y1)
		return 0;

	CompositeTags(
		COMPOSITE_Src_Over_Dest,
			sd->bm,
			rastPort->BitMap,
		COMPTAG_SrcWidth,   sd->w,
		COMPTAG_SrcHeight,  sd->h,
		COMPTAG_ScaleX,     hookData.scaleX,
		COMPTAG_ScaleY,     hookData.scaleY,
		COMPTAG_OffsetX,    ox,
		COMPTAG_OffsetY,    oy,
		COMPTAG_DestX,      x0,
		COMPTAG_DestY,      y0,
		COMPTAG_DestWidth,  x1 - x0,
		COMPTAG_DestHeight, y1 - y0,
		COMPTAG_Flags,      COMPFLAG_IgnoreDestAlpha,
		TAG_END);
	return 0;
}

void BackFill_Sprite(struct BitMap *bm, int w, int h, int x, int y)
{
	if (!W || !bm || !hookData.srcBitMap || w <= 0 || h <= 0)
		return;

	spriteData.bm = bm;
	spriteData.w = w;
	spriteData.h = h;
	spriteData.x = x;
	spriteData.y = y;
	spriteHook.h_Entry = (HOOKFUNC) spriteHookFunc;
	spriteHook.h_Data = &spriteData;

	{
		struct RastPort *RPort = W->RPort;
		LockLayer(0, RPort->Layer);
		DoHookClipRects(&spriteHook, RPort, rect_ptr);
		UnlockLayer(RPort->Layer);
	}
}
#endif
