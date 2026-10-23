/*
 * E-UAE - ReAction GUI: Seite "Shortcuts"
 *
 *   Tastenkuerzel (Hotkeys) des Emulatorfensters aendern: Liste aller Aktionen
 *   mit ihrer Tastenkombination, darunter Sondertasten + Taste fuer den
 *   gewaehlten Eintrag. Die Belegung lebt in ami-win.c (ami_hk_*) und wird in
 *   PROGDIR:E-UAE.hotkeys gespeichert. Wirkt sofort, auch bei laufender Emulation.
 */

#include "reaction_gui.h"

/* ami-win.c */
extern int         ami_hk_count (void);
extern const char *ami_hk_label (int i);
extern void        ami_hk_get (int i, int *mods, int *key);
extern int         ami_hk_set (int i, int mods, int key);
extern int         ami_hk_find (int mods, int key, int exclude);
extern void        ami_hk_default (int i);
extern void        ami_hk_default_all (void);
extern int         ami_hk_nkeys (void);
extern const char *ami_hk_keyname_at (int n);
extern int         ami_hk_keycode_at (int n);
extern const char *ami_hk_keyname (int code);
extern int         ami_hk_save (void);

#define BASE GID_PAGE_BASE (PAGE_ID_SHORTCUTS)

#define NUM_MODS 7
#define MAX_KEYS 128

enum {
    GID_LIST = BASE + 1,
    GID_MOD  = BASE + 10,           /* +0..6: Ctrl, LAlt, RAlt, LAmiga, RAmiga, LShift, RShift */
    GID_KEY  = BASE + 20,
    GID_APPLY, GID_DEFAULT, GID_ALLDEFAULT
};

static const char *const mod_texts[NUM_MODS] = {
    "Ctrl", "Left Alt", "Right Alt", "Left Amiga", "Right Amiga", "Left Shift", "Right Shift"
};

static const char *key_names[MAX_KEYS + 1];
static LONG        key_values[MAX_KEYS];
static struct ra_choice c_key = { key_names, key_values, 0, NULL };

static Object *g_list, *g_mod[NUM_MODS], *g_key, *g_apply, *g_default, *g_alldef;

static struct List       *item_list;
static struct ColumnInfo *col_info;
static int                selected = -1;

/* "Ctrl + Left Alt + Q" */
static void combo_text (int mods, int key, char *buf, size_t len)
{
    int j;

    buf[0] = '\0';
    for (j = 0; j < NUM_MODS; j++)
	if (mods & (1 << j)) {
	    strncat (buf, ra_tr (mod_texts[j]), len - strlen (buf) - 1);
	    strncat (buf, " + ", len - strlen (buf) - 1);
	}
    strncat (buf, ami_hk_keyname (key), len - strlen (buf) - 1);
}

static void rebuild_list (void)
{
    int i, n = ami_hk_count ();

    if (!item_list || !g_list)
	return;

    RA_SET (g_list, LISTBROWSER_Labels, (ULONG) ~0);          /* abkoppeln */
    FreeListBrowserList (item_list);

    for (i = 0; i < n; i++) {
	char combo[128];
	int mods, key;
	struct Node *node;

	ami_hk_get (i, &mods, &key);
	combo_text (mods, key, combo, sizeof combo);
	node = AllocListBrowserNode (2,
				     LBNA_Column,    0,
				     LBNCA_CopyText, TRUE,
				     LBNCA_Text,     ra_tr (ami_hk_label (i)),
				     LBNA_Column,    1,
				     LBNCA_CopyText, TRUE,
				     LBNCA_Text,     combo,
				     TAG_DONE);
	if (node)
	    AddTail (item_list, node);
    }

    if (selected >= n)
	selected = n - 1;

    RA_SET (g_list, LISTBROWSER_Labels,   (ULONG) item_list,
		    LISTBROWSER_Selected, selected);
}

static void load_form (int i)
{
    int mods, key, j;

    ami_hk_get (i, &mods, &key);
    for (j = 0; j < NUM_MODS; j++)
	RA_SET (g_mod[j], GA_Selected, (mods & (1 << j)) ? TRUE : FALSE);
    ra_choice_set (g_key, &c_key, key, 0);
}

static void update_buttons (void)
{
    int j;
    BOOL have = selected >= 0;

    for (j = 0; j < NUM_MODS; j++)
	ra_set_disabled (g_mod[j], !have);
    ra_set_disabled (g_key,     !have);
    ra_set_disabled (g_apply,   !have);
    ra_set_disabled (g_default, !have);
}

static void save_file (void)
{
    if (ami_hk_save () != 0)
	ra_error ("Could not write the hotkeys file.");
}

static void do_apply (void)
{
    int j, mods = 0, key, rc, other;

    if (selected < 0)
	return;
    for (j = 0; j < NUM_MODS; j++)
	if (ra_get_num (g_mod[j], GA_Selected))
	    mods |= 1 << j;
    key = (int) ra_choice_value (g_key, &c_key);

    other = ami_hk_find (mods, key, selected);
    if (other >= 0) {
	ra_error ("This combination is already used by: %s", ra_tr (ami_hk_label (other)));
	return;
    }
    rc = ami_hk_set (selected, mods, key);
    if (rc == -2) {
	ra_error ("Please select at least one modifier key (Ctrl, Alt, Amiga or Shift).");
	return;
    }
    if (rc != 0) {
	ra_error ("At most three modifier keys can be combined.");
	return;
    }
    save_file ();
    rebuild_list ();
}

static void do_default (void)
{
    if (selected < 0)
	return;
    ami_hk_default (selected);
    save_file ();
    rebuild_list ();
    load_form (selected);
}

static void do_default_all (void)
{
    ami_hk_default_all ();
    save_file ();
    rebuild_list ();
    if (selected >= 0)
	load_form (selected);
}

/* ---- Seite ------------------------------------------------------------ */

static Object *create (void)
{
    int i, n = ami_hk_nkeys ();

    if (n > MAX_KEYS)
	n = MAX_KEYS;
    for (i = 0; i < n; i++) {
	key_names[i]  = ami_hk_keyname_at (i);
	key_values[i] = ami_hk_keycode_at (i);
    }
    key_names[n] = NULL;
    c_key.count  = n;

    item_list = AllocSysObjectTags (ASOT_LIST, TAG_END);
    col_info  = AllocLBColumnInfo (2,
				   LBCIA_Column, 0, LBCIA_Title, ra_tr ("Action"),   LBCIA_Weight, 60,
				   LBCIA_Column, 1, LBCIA_Title, ra_tr ("Shortcut"), LBCIA_Weight, 40,
				   TAG_DONE);
    if (!item_list || !col_info)
	return NULL;

    return VLayoutObject,
	LAYOUT_SpaceOuter, TRUE,

	LAYOUT_AddChild, g_list = ListBrowserObject,
	    GA_ID,                    GID_LIST,
	    GA_RelVerify,             TRUE,
	    LISTBROWSER_ColumnInfo,   col_info,
	    LISTBROWSER_ColumnTitles, TRUE,
	    LISTBROWSER_Labels,       item_list,
	    LISTBROWSER_ShowSelected, TRUE,
	    LISTBROWSER_Selected,     -1,
	ListBrowserEnd,

	LAYOUT_AddChild, VLayoutObject,
	    LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("Selected shortcut"), LAYOUT_SpaceOuter, TRUE,
	    LAYOUT_AddChild, HLayoutObject,
		LAYOUT_AddChild, g_mod[0] = ra_checkbox (GID_MOD + 0, mod_texts[0]),
		LAYOUT_AddChild, g_mod[1] = ra_checkbox (GID_MOD + 1, mod_texts[1]),
		LAYOUT_AddChild, g_mod[2] = ra_checkbox (GID_MOD + 2, mod_texts[2]),
		LAYOUT_AddChild, g_mod[3] = ra_checkbox (GID_MOD + 3, mod_texts[3]),
	    LayoutEnd,
	    LAYOUT_AddChild, HLayoutObject,
		LAYOUT_AddChild, g_mod[4] = ra_checkbox (GID_MOD + 4, mod_texts[4]),
		LAYOUT_AddChild, g_mod[5] = ra_checkbox (GID_MOD + 5, mod_texts[5]),
		LAYOUT_AddChild, g_mod[6] = ra_checkbox (GID_MOD + 6, mod_texts[6]),
	    LayoutEnd,
	    LAYOUT_AddChild, g_key = ra_choice_gadget (GID_KEY, &c_key),
		CHILD_MinWidth, ra_names_width (c_key.names), CHILD_Label, ra_label ("Key:"),
	LayoutEnd,
	CHILD_WeightedHeight, 0,

	LAYOUT_AddChild, HLayoutObject,
	    LAYOUT_EvenSize, TRUE,
	    LAYOUT_AddChild, g_apply   = ra_button (GID_APPLY,      "Apply"),
	    LAYOUT_AddChild, g_default = ra_button (GID_DEFAULT,    "Default"),
	    LAYOUT_AddChild, g_alldef  = ra_button (GID_ALLDEFAULT, "All defaults"),
	LayoutEnd,
	CHILD_WeightedHeight, 0,
    LayoutEnd;
}

static void read_prefs (void)
{
    /* Die Belegung wird direkt ueber die Buttons geaendert. */
}

static void write_prefs (void)
{
    rebuild_list ();
    if (selected >= 0)
	load_form (selected);
    update_buttons ();
}

static int handle (ULONG gid, ULONG code)
{
    switch (gid) {
	case GID_LIST:
	    selected = (int) ra_get_num (g_list, LISTBROWSER_Selected);
	    if (selected >= ami_hk_count ())
		selected = -1;
	    if (selected >= 0)
		load_form (selected);
	    update_buttons ();
	    break;
	case GID_APPLY:      do_apply ();       break;
	case GID_DEFAULT:    do_default ();     break;
	case GID_ALLDEFAULT: do_default_all (); break;
	default:             return 0;
    }
    return 1;
}

static void cleanup (void)
{
    if (item_list) {
	FreeListBrowserList (item_list);
	FreeSysObject (ASOT_LIST, item_list);
	item_list = NULL;
    }
    if (col_info) {
	FreeLBColumnInfo (col_info);
	col_info = NULL;
    }
    ra_choice_free (&c_key);
    g_list = NULL;
    selected = -1;
}

/* Tooltips */
static const struct ra_hint hints[] = {
    { GID_LIST, 1, "Hotkeys of the emulator window. Select an entry to change its key combination." },
    { GID_MOD, 7, "Modifier keys of this shortcut. Hold them down in any order, then press the key." },
    { GID_KEY, 1, "The key that triggers the action together with the modifier keys." },
    { GID_APPLY, 1, "Use the selected modifier keys and key for this action. Takes effect at once and is saved." },
    { GID_DEFAULT, 1, "Restore the default shortcut of the selected action." },
    { GID_ALLDEFAULT, 1, "Restore the default shortcuts of all actions." },
    { 0, 0, NULL }
};

const struct ra_page ra_page_shortcuts = {
    "Shortcuts", PAGE_ID_SHORTCUTS, create, read_prefs, write_prefs, handle, NULL, cleanup, hints
};
