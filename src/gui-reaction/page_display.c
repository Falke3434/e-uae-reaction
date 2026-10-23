/*
 * E-UAE - ReAction GUI: Seite "Display"
 *
 *   Screen-Art (eigener Screen / Public Screen), Aufloesung, Vollbild,
 *   Zeilenmodus, Zentrierung, Farbtiefe, Framerate, Darstellungsoptionen.
 */

#include "reaction_gui.h"

#define BASE GID_PAGE_BASE (PAGE_ID_DISPLAY)

enum {
    GID_SCREENTYPE = BASE + 1, GID_PUBSCREEN,
    GID_WIN_W, GID_WIN_H, GID_FS_W, GID_FS_H,
    GID_AFULL, GID_PFULL,
    GID_LINEMODE, GID_XCENTER, GID_YCENTER, GID_COLOR,
    GID_FRAMERATE, GID_REFRESH,
    GID_LORES, GID_ASPECT, GID_VSYNC, GID_DITHER, GID_GREY, GID_LEDS,
    GID_RTGSPRITE, GID_RTGVBLANK
};

static const char *const screen_names[] = { "Own screen", "Public screen", "Ask at startup", NULL };
static const LONG screen_values[] = { 0, 1, 2 };
RA_CHOICE (c_screen, screen_names, screen_values);

static const char *const line_names[] = { "Normal", "Double lines", "Scanlines", NULL };
static const LONG line_values[] = { 0, 1, 2 };
RA_CHOICE (c_line, line_names, line_values);

static const char *const center_names[] = { "No", "Simple", "Smart", NULL };
static const LONG center_values[] = { 0, 1, 2 };
RA_CHOICE (c_xcenter, center_names, center_values);
RA_CHOICE (c_ycenter, center_names, center_values);


static struct ra_choice *const all_choices[] = { &c_screen, &c_line, &c_xcenter, &c_ycenter };
#define NUM_CHOICES (int)(sizeof all_choices / sizeof all_choices[0])

static Object *g_screen, *g_pub;
static Object *g_ww, *g_wh, *g_fw, *g_fh, *g_afull, *g_pfull;
static Object *g_line, *g_xc, *g_yc, *g_frame;
static Object *g_lores, *g_aspect, *g_dither, *g_grey, *g_leds;
static Object *g_rtgspr, *g_rtgvbl;

static Object *create (void)
{
    return HLayoutObject,
	LAYOUT_SpaceOuter, TRUE,

	LAYOUT_AddChild, VLayoutObject,
	    LAYOUT_AddChild, VLayoutObject,
		LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("Screen"), LAYOUT_SpaceOuter, TRUE,
		LAYOUT_AddChild, g_screen = ra_choice_gadget (GID_SCREENTYPE, &c_screen), CHILD_MinWidth, ra_names_width (c_screen.names), CHILD_Label, ra_label ("Screen type:"),
		LAYOUT_AddChild, g_pub    = ra_string (GID_PUBSCREEN, 79),                CHILD_Label, ra_label ("Public screen:"),
		LAYOUT_AddChild, g_afull  = ra_checkbox (GID_AFULL, "Fullscreen (Amiga modes)"),
		LAYOUT_AddChild, g_pfull  = ra_checkbox (GID_PFULL, "Fullscreen (Picasso96 modes)"),
	    LayoutEnd,
	    CHILD_WeightedHeight, 0,

	    LAYOUT_AddChild, VLayoutObject,
		LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("Resolution"), LAYOUT_SpaceOuter, TRUE,
		LAYOUT_AddChild, g_ww = ra_integer (GID_WIN_W, 320, 2048, 720), CHILD_Label, ra_label ("Window width:"),
		LAYOUT_AddChild, g_wh = ra_integer (GID_WIN_H, 200, 1536, 568), CHILD_Label, ra_label ("Window height:"),
		LAYOUT_AddChild, g_fw = ra_integer (GID_FS_W, 320, 2048, 800),  CHILD_Label, ra_label ("Fullscreen width:"),
		LAYOUT_AddChild, g_fh = ra_integer (GID_FS_H, 200, 1536, 600),  CHILD_Label, ra_label ("Fullscreen height:"),
	    LayoutEnd,
	    CHILD_WeightedHeight, 0,

	    LAYOUT_AddChild, VLayoutObject,
		LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("Graphics card (RTG)"), LAYOUT_SpaceOuter, TRUE,
		LAYOUT_AddChild, g_rtgspr = ra_checkbox (GID_RTGSPRITE, "Hardware mouse pointer"),
		LAYOUT_AddChild, g_rtgvbl = ra_checkbox (GID_RTGVBLANK, "Vertical blank interrupt"),
	    LayoutEnd,
	    CHILD_WeightedHeight, 0,
	    LAYOUT_AddChild, SpaceObject, SpaceEnd,
	LayoutEnd,

	LAYOUT_AddChild, VLayoutObject,
	    LAYOUT_AddChild, VLayoutObject,
		LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("Appearance"), LAYOUT_SpaceOuter, TRUE,
		LAYOUT_AddChild, g_line  = ra_choice_gadget (GID_LINEMODE, &c_line), CHILD_MinWidth, ra_names_width (c_line.names),   CHILD_Label, ra_label ("Line mode:"),
		LAYOUT_AddChild, g_xc    = ra_choice_gadget (GID_XCENTER, &c_xcenter), CHILD_MinWidth, ra_names_width (c_xcenter.names), CHILD_Label, ra_label ("Center horizontally:"),
		LAYOUT_AddChild, g_yc    = ra_choice_gadget (GID_YCENTER, &c_ycenter), CHILD_MinWidth, ra_names_width (c_ycenter.names), CHILD_Label, ra_label ("Center vertically:"),
		LAYOUT_AddChild, g_frame   = ra_integer (GID_FRAMERATE, 1, 20, 1),     CHILD_Label, ra_label ("Every nth frame:"),
	    LayoutEnd,
	    CHILD_WeightedHeight, 0,

	    LAYOUT_AddChild, VLayoutObject,
		LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("Options"), LAYOUT_SpaceOuter, TRUE,
		LAYOUT_AddChild, g_lores  = ra_checkbox (GID_LORES,  "Lores (half resolution)"),
		LAYOUT_AddChild, g_aspect = ra_checkbox (GID_ASPECT, "Correct aspect ratio"),
		LAYOUT_AddChild, g_dither = ra_checkbox (GID_DITHER, "Dithering (few colors)"),
		LAYOUT_AddChild, g_grey   = ra_checkbox (GID_GREY,   "Grayscale"),
		LAYOUT_AddChild, g_leds   = ra_checkbox (GID_LEDS,   "Show LEDs in picture"),
	    LayoutEnd,
	    CHILD_WeightedHeight, 0,
	    LAYOUT_AddChild, SpaceObject, SpaceEnd,
	LayoutEnd,
    LayoutEnd;
}

static int checked (Object *o) { return ra_get_num (o, GA_Selected) ? 1 : 0; }

static void read_prefs (void)
{
#ifdef USE_AMIGA_GFX
    const char *pub = ra_get_text (g_pub);

    changed_prefs.amiga_screen_type = ra_choice_value (g_screen, &c_screen);
    if (strcmp (changed_prefs.amiga_publicscreen, pub) != 0) {
	strncpy (changed_prefs.amiga_publicscreen, pub, sizeof changed_prefs.amiga_publicscreen - 1);
	changed_prefs.amiga_publicscreen[sizeof changed_prefs.amiga_publicscreen - 1] = '\0';
    }
    changed_prefs.amiga_use_dither = checked (g_dither);
    changed_prefs.amiga_use_grey   = checked (g_grey);
#endif
    changed_prefs.gfx_width_win  = ra_get_num (g_ww, INTEGER_Number);
    changed_prefs.gfx_height_win = ra_get_num (g_wh, INTEGER_Number);
    changed_prefs.gfx_width_fs   = ra_get_num (g_fw, INTEGER_Number);
    changed_prefs.gfx_height_fs  = ra_get_num (g_fh, INTEGER_Number);
    changed_prefs.gfx_afullscreen = checked (g_afull);
    changed_prefs.gfx_pfullscreen = checked (g_pfull);

    changed_prefs.gfx_linedbl = ra_choice_value (g_line, &c_line);
    changed_prefs.gfx_xcenter = ra_choice_value (g_xc, &c_xcenter);
    changed_prefs.gfx_ycenter = ra_choice_value (g_yc, &c_ycenter);
    changed_prefs.gfx_framerate   = ra_get_num (g_frame, INTEGER_Number);

    changed_prefs.gfx_lores          = checked (g_lores);
    changed_prefs.gfx_correct_aspect = checked (g_aspect);
    changed_prefs.leds_on_screen     = checked (g_leds);
    changed_prefs.rtg_hardwaresprite    = checked (g_rtgspr);
    changed_prefs.rtg_hardwareinterrupt = checked (g_rtgvbl);

    /* Fenstergroesse beim Start: E-UAE nimmt je nach Modus _win oder _fs. */
    changed_prefs.gfx_width  = changed_prefs.gfx_afullscreen ? changed_prefs.gfx_width_fs  : changed_prefs.gfx_width_win;
    changed_prefs.gfx_height = changed_prefs.gfx_afullscreen ? changed_prefs.gfx_height_fs : changed_prefs.gfx_height_win;
}

static void write_prefs (void)
{
#ifdef USE_AMIGA_GFX
    ra_choice_set (g_screen, &c_screen, changed_prefs.amiga_screen_type, 1);
    ra_set_text (g_pub, changed_prefs.amiga_publicscreen);
    RA_SET (g_dither, GA_Selected, changed_prefs.amiga_use_dither ? TRUE : FALSE);
    RA_SET (g_grey,   GA_Selected, changed_prefs.amiga_use_grey   ? TRUE : FALSE);
#endif
    RA_SET (g_ww, INTEGER_Number, changed_prefs.gfx_width_win);
    RA_SET (g_wh, INTEGER_Number, changed_prefs.gfx_height_win);
    RA_SET (g_fw, INTEGER_Number, changed_prefs.gfx_width_fs);
    RA_SET (g_fh, INTEGER_Number, changed_prefs.gfx_height_fs);
    RA_SET (g_afull, GA_Selected, changed_prefs.gfx_afullscreen ? TRUE : FALSE);
    RA_SET (g_pfull, GA_Selected, changed_prefs.gfx_pfullscreen ? TRUE : FALSE);

    ra_choice_set (g_line,  &c_line,    changed_prefs.gfx_linedbl, 0);
    ra_choice_set (g_xc,    &c_xcenter, changed_prefs.gfx_xcenter, 0);
    ra_choice_set (g_yc,    &c_ycenter, changed_prefs.gfx_ycenter, 0);
    RA_SET (g_frame,   INTEGER_Number, changed_prefs.gfx_framerate);

    RA_SET (g_lores,  GA_Selected, changed_prefs.gfx_lores          ? TRUE : FALSE);
    RA_SET (g_aspect, GA_Selected, changed_prefs.gfx_correct_aspect ? TRUE : FALSE);
    RA_SET (g_leds,   GA_Selected, changed_prefs.leds_on_screen     ? TRUE : FALSE);
    RA_SET (g_rtgspr, GA_Selected, changed_prefs.rtg_hardwaresprite    ? TRUE : FALSE);
    RA_SET (g_rtgvbl, GA_Selected, changed_prefs.rtg_hardwareinterrupt ? TRUE : FALSE);
}

static void state (int st)
{
    /* v17: Farbmodus, Bildwiederholrate und "Mit Bildschirm synchronisieren"
     * entfernt - das OS4-Grafik-Backend las sie nie (Werte bleiben in der
     * Konfiguration erhalten).
     * Screen-Art, Vollbild und die RTG-Optionen wirken erst beim
     * naechsten Start (currprefs wird nur dann uebernommen). */
    BOOL lock = st != UAE_STATE_STOPPED;

    ra_set_disabled (g_screen, lock);
    ra_set_disabled (g_pub,    lock);
    ra_set_disabled (g_afull,  lock);
    ra_set_disabled (g_pfull,  lock);
    ra_set_disabled (g_rtgspr, lock);
    ra_set_disabled (g_rtgvbl, lock);
}

static int handle (ULONG gid, ULONG code)
{
    return 1;
}

static void cleanup (void)
{
    int i;

    for (i = 0; i < NUM_CHOICES; i++)
	ra_choice_free (all_choices[i]);
}

/* Tooltips (mouse-over help). English text = key in Catalogs/E-UAE.cd. */
static const struct ra_hint hints[] = {
    { GID_SCREENTYPE, 1, "Where the emulation opens: on its own screen, on a public screen such as Workbench, or ask at startup." },
    { GID_PUBSCREEN, 1, "Name of the public screen the emulator window opens on." },
    { GID_AFULL, 1, "Open an own screen for the Amiga display instead of a window, with the fullscreen width and height." },
    { GID_PFULL, 1, "Open an own screen while the Amiga shows a Picasso96 (RTG) screen. When it switches back, the setting for Amiga modes applies again." },
    { GID_WIN_W, 1, "Width of the emulator window in pixels." },
    { GID_WIN_H, 1, "Height of the emulator window in pixels." },
    { GID_FS_W, 1, "Width of the fullscreen mode in pixels." },
    { GID_FS_H, 1, "Height of the fullscreen mode in pixels." },
    { GID_LINEMODE, 1, "How lines are drawn. Double lines fill the full window height; Scanlines leave gaps like a CRT." },
    { GID_XCENTER, 1, "Center the picture horizontally in the window." },
    { GID_YCENTER, 1, "Center the picture vertically in the window." },
    { GID_FRAMERATE, 1, "Draw only every nth frame. Higher values save CPU time." },
    { GID_LORES, 1, "Render at half the horizontal resolution. Faster, but less sharp." },
    { GID_ASPECT, 1, "Correct the pixel aspect ratio of the emulated display." },
    { GID_DITHER, 1, "Reduce banding when the display has only a few colors." },
    { GID_GREY, 1, "Show the picture in grayscale." },
    { GID_LEDS, 1, "Show the power and drive LEDs inside the picture." },
    { GID_RTGSPRITE, 1, "The graphics card shows the mouse pointer itself (hardware sprite): no flicker and less work for Picasso96. Needs a 16 or 32 bit host screen." },
    { GID_RTGVBLANK, 1, "The graphics card raises an interrupt at every vertical blank, like real cards. Some programs need it for smooth scrolling." },
    { 0, 0, NULL }
};

const struct ra_page ra_page_display = {
    "Display", PAGE_ID_DISPLAY, create, read_prefs, write_prefs, handle, state, cleanup, hints
};
