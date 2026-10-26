/*
 * E-UAE - ReAction GUI: Seite "Input"
 *
 *   Joystick-Port 0/1 (Maus, Joystick, Tastatur-Emulation), Tastaturlayout,
 *   Mausgeschwindigkeit, Mauszeiger verbergen.
 */

#include "reaction_gui.h"

#define BASE GID_PAGE_BASE (PAGE_ID_INPUT)

enum {
    GID_PORT0 = BASE + 1, GID_PORT1, GID_KBDLANG, GID_MOUSESPEED, GID_HIDECURSOR, GID_MIDDLE, GID_MAGIC
};

static const char *const port_names[] = {
    "Keyboard: numeric keypad", "Keyboard: cursor + space", "Keyboard: T/F/H/B + Alt",
    "Keyboard: X-Arcade 1", "Keyboard: X-Arcade 2",
    "Joystick 0", "Joystick 1", "Joystick 2", "Joystick 3",
    "Mouse", "Mouse 2", "Not assigned", NULL };
static const LONG port_values[] = {
    JSEM_KBDLAYOUT + 0, JSEM_KBDLAYOUT + 1, JSEM_KBDLAYOUT + 2,
    JSEM_KBDLAYOUT + 3, JSEM_KBDLAYOUT + 4,
    JSEM_JOYS + 0, JSEM_JOYS + 1, JSEM_JOYS + 2, JSEM_JOYS + 3,
    JSEM_MICE + 0, JSEM_MICE + 1, JSEM_NONE };
RA_CHOICE (c_port0, port_names, port_values);
RA_CHOICE (c_port1, port_names, port_values);

static const char *const lang_names[] = { "US", "Danish", "German", "Swedish",
					  "French", "Italian", "Spanish", NULL };
static const LONG lang_values[] = { KBD_LANG_US, KBD_LANG_DK, KBD_LANG_DE, KBD_LANG_SE,
				    KBD_LANG_FR, KBD_LANG_IT, KBD_LANG_ES };
RA_CHOICE (c_lang, lang_names, lang_values);

static struct ra_choice *const all_choices[] = { &c_port0, &c_port1, &c_lang };
#define NUM_CHOICES (int)(sizeof all_choices / sizeof all_choices[0])

static Object *g_p0, *g_p1, *g_lang, *g_speed, *g_middle, *g_magic;

static Object *create (void)
{
    return VLayoutObject,
	LAYOUT_SpaceOuter, TRUE,
	LAYOUT_AddChild, VLayoutObject,
	    LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("Controller ports"), LAYOUT_SpaceOuter, TRUE,
	    LAYOUT_AddChild, g_p0 = ra_choice_gadget (GID_PORT0, &c_port0), CHILD_MinWidth, ra_names_width (c_port0.names), CHILD_Label, ra_label ("Port 0 (mouse):"),
	    LAYOUT_AddChild, g_p1 = ra_choice_gadget (GID_PORT1, &c_port1), CHILD_MinWidth, ra_names_width (c_port1.names), CHILD_Label, ra_label ("Port 1 (joystick):"),
	LayoutEnd,
	CHILD_WeightedHeight, 0,
	LAYOUT_AddChild, VLayoutObject,
	    LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("Keyboard and mouse"), LAYOUT_SpaceOuter, TRUE,
	    LAYOUT_AddChild, g_lang  = ra_choice_gadget (GID_KBDLANG, &c_lang), CHILD_MinWidth, ra_names_width (c_lang.names),   CHILD_Label, ra_label ("Keyboard layout:"),
	    LAYOUT_AddChild, g_speed = ra_integer (GID_MOUSESPEED, 1, 1000, 100), CHILD_Label, ra_label ("Mouse speed (%):"),
	    LAYOUT_AddChild, g_middle = ra_checkbox (GID_MIDDLE, "Middle mouse button releases the pointer"),
	    LAYOUT_AddChild, g_magic = ra_checkbox (GID_MAGIC, "Magic Mouse: Amiga pointer follows the host pointer"),
	LayoutEnd,
	CHILD_WeightedHeight, 0,
	LAYOUT_AddChild, SpaceObject, SpaceEnd,
    LayoutEnd;
}

static void read_prefs (void)
{
    changed_prefs.jport0 = ra_choice_value (g_p0, &c_port0);
    changed_prefs.jport1 = ra_choice_value (g_p1, &c_port1);
    changed_prefs.keyboard_lang = (KbdLang) ra_choice_value (g_lang, &c_lang);
    changed_prefs.input_mouse_speed = ra_get_num (g_speed, INTEGER_Number);
    changed_prefs.amiga_middle_release = ra_get_num (g_middle, GA_Selected) ? 1 : 0;
    changed_prefs.amiga_magic_mouse = ra_get_num (g_magic, GA_Selected) ? 1 : 0;	/* v18 */
}

static void write_prefs (void)
{
    ra_choice_set (g_p0, &c_port0, changed_prefs.jport0, 9);
    ra_choice_set (g_p1, &c_port1, changed_prefs.jport1, 0);
    ra_choice_set (g_lang, &c_lang, changed_prefs.keyboard_lang, 0);
    RA_SET (g_speed, INTEGER_Number, changed_prefs.input_mouse_speed);
    RA_SET (g_middle, GA_Selected, changed_prefs.amiga_middle_release ? TRUE : FALSE);
    RA_SET (g_magic, GA_Selected, changed_prefs.amiga_magic_mouse ? TRUE : FALSE);
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
    { GID_PORT0, 1, "Device that controls Amiga port 0 (normally the mouse)." },
    { GID_PORT1, 1, "Device that controls Amiga port 1 (normally the joystick). With the CD32 Akiko chip it is a CD32 pad: joystick buttons 1-7 are red, blue, yellow, green, forward, reverse and play." },
    { GID_KBDLANG, 1, "Keyboard layout of the emulated Amiga keyboard." },
    { GID_MOUSESPEED, 1, "Mouse sensitivity in percent." },
    { GID_MIDDLE, 1, "In a window on the Workbench, the middle mouse button gives the pointer back to the Workbench and makes the E-UAE window inactive. Click into the window to continue. The Amiga then gets no middle button; in full screen it always does." },
    { GID_MAGIC, 1, "The Amiga mouse pointer follows the Workbench pointer, without capturing the mouse: the pointer can leave the window at any time. Works in a window with a graphics card (RTG) screen and the hardware mouse pointer; on chipset screens and in full screen the mouse works as before." },
    { 0, 0, NULL }
};

const struct ra_page ra_page_input = {
    "Input", PAGE_ID_INPUT, create, read_prefs, write_prefs, handle, NULL, cleanup, hints
};
