/*
 * E-UAE - ReAction GUI: Seite "Configuration"
 *
 *   - Konfigurationsdatei laden / speichern / speichern unter
 *   - Beschreibung der Konfiguration
 *   - Savestates laden / speichern (waehrend die Emulation laeuft)
 *   - Allgemeine Optionen (GUI beim Start, bsdsocket.library, Uhr synchronisieren)
 */

#include "reaction_gui.h"

#define BASE GID_PAGE_BASE (PAGE_ID_CONFIG)

enum {
    GID_DESC = BASE + 1,
    GID_LOAD, GID_SAVE, GID_SAVEAS,
    GID_STATE_LOAD, GID_STATE_SAVE,
    GID_STARTGUI, GID_BSDSOCKET, GID_TODHACK
};

static Object *g_desc, *g_load, *g_save, *g_saveas;
static Object *g_sload, *g_ssave;
static Object *g_startgui, *g_bsd, *g_tod;

static char last_config[256];

static Object *create (void)
{
    return VLayoutObject,
	LAYOUT_SpaceOuter, TRUE,

	LAYOUT_AddChild, VLayoutObject,
	    LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("Configuration"), LAYOUT_SpaceOuter, TRUE,
	    LAYOUT_AddChild, g_desc = ra_string (GID_DESC, 255), CHILD_Label, ra_label ("Description:"),
	    LAYOUT_AddChild, HLayoutObject,
		LAYOUT_EvenSize, TRUE,
		LAYOUT_AddChild, g_load   = ra_button (GID_LOAD,   "Load..."),
		LAYOUT_AddChild, g_save   = ra_button (GID_SAVE,   "Save"),
		LAYOUT_AddChild, g_saveas = ra_button (GID_SAVEAS, "Save as..."),
	    LayoutEnd,
	LayoutEnd,
	CHILD_WeightedHeight, 0,

	LAYOUT_AddChild, VLayoutObject,
	    LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("Savestates"), LAYOUT_SpaceOuter, TRUE,
	    LAYOUT_AddChild, HLayoutObject,
		LAYOUT_EvenSize, TRUE,
		LAYOUT_AddChild, g_sload = ra_button (GID_STATE_LOAD, "Load savestate..."),
		LAYOUT_AddChild, g_ssave = ra_button (GID_STATE_SAVE, "Save savestate..."),
	    LayoutEnd,
	LayoutEnd,
	CHILD_WeightedHeight, 0,

	LAYOUT_AddChild, VLayoutObject,
	    LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("General"), LAYOUT_SpaceOuter, TRUE,
	    LAYOUT_AddChild, g_startgui = ra_checkbox (GID_STARTGUI,   "Open GUI at program start"),
	    LAYOUT_AddChild, g_bsd      = ra_checkbox (GID_BSDSOCKET,  "Emulate bsdsocket.library (network)"),
	    LAYOUT_AddChild, g_tod      = ra_checkbox (GID_TODHACK,    "Synchronize Amiga clock with system time"),
	LayoutEnd,
	CHILD_WeightedHeight, 0,

	LAYOUT_AddChild, SpaceObject, SpaceEnd,
    LayoutEnd;
}

static void read_prefs (void)
{
    const char *d = ra_get_text (g_desc);

    if (strcmp (changed_prefs.description, d) != 0) {
	strncpy (changed_prefs.description, d, sizeof changed_prefs.description - 1);
	changed_prefs.description[sizeof changed_prefs.description - 1] = '\0';
    }
    changed_prefs.start_gui   = ra_get_num (g_startgui, GA_Selected) ? 1 : 0;
    changed_prefs.socket_emu  = ra_get_num (g_bsd, GA_Selected) ? 1 : 0;
    changed_prefs.tod_hack    = ra_get_num (g_tod, GA_Selected) ? 1 : 0;
}

static void write_prefs (void)
{
    ra_set_text (g_desc, changed_prefs.description);
    RA_SET (g_startgui, GA_Selected, changed_prefs.start_gui ? TRUE : FALSE);
    RA_SET (g_bsd,      GA_Selected, changed_prefs.socket_emu ? TRUE : FALSE);
    RA_SET (g_tod,      GA_Selected, changed_prefs.tod_hack ? TRUE : FALSE);
}

static void do_load (void)
{
    char path[256];

    strcpy (path, last_config);
    if (!ra_request_file_in (RA_DIR_CONFIG, "Load configuration", "#?.(uae|cfg|conf)", FALSE, FALSE, path, sizeof path))
	return;
    strcpy (last_config, path);

    /*
     * Laeuft die Emulation, startet uae_restart() sie mit genau dieser Datei
     * neu und oeffnet die GUI wieder. Ist sie gestoppt, laedt main.c die
     * Datei direkt in der Warteschleife. In beiden Faellen schreibt
     * gui_open() die neuen Werte in alle Gadgets. "Save" schreibt weiterhin
     * in die Standarddatei, "Save as..." in eine beliebige.
     */
    uae_restart (-1, path);
}

static void do_save_as (void)
{
    char path[256];

    ra_read_all ();
    strcpy (path, last_config);
    if (!ra_request_file_in (RA_DIR_CONFIG, "Save configuration", "#?.(uae|cfg|conf)", TRUE, FALSE, path, sizeof path))
	return;

    /* Ohne Endung ".cfg" anhaengen ("Amiga1200" -> "Amiga1200.cfg").
     * Eine eigene Endung (.uae, .conf, ...) bleibt. Gibt es die Datei mit
     * angehaengter Endung schon, vorher fragen - im Requester stand ja
     * ein anderer Name. */
    if (!strchr ((const char *) FilePart (path), '.') && strlen (path) + 4 < sizeof path) {
	BPTR lock;

	strcat (path, ".cfg");
	lock = Lock (path, SHARED_LOCK);
	if (lock) {
	    UnLock (lock);
	    if (!ra_confirm ("The file '%s' already exists.\nReplace it?", path))
		return;
	}
    }

    if (cfgfile_save (&changed_prefs, path, 0))
	strcpy (last_config, path);
    else
	ra_error ("Could not write configuration to '%s'.", path);
}

static int handle (ULONG gid, ULONG code)
{
    switch (gid) {
	case GID_LOAD:    do_load ();               break;
	case GID_SAVEAS:  do_save_as ();            break;
	case GID_SAVE:
	    ra_read_all ();
	    uae_save_config ();                     /* Standarddatei (siehe main.c) */
	    break;
#ifdef SAVESTATE
	case GID_STATE_LOAD: gui_display (4);       break;
	case GID_STATE_SAVE: gui_display (5);       break;
#endif
	default:          return 0;
    }
    return 1;
}

static void state (int st)
{
    BOOL running = st == UAE_STATE_RUNNING || st == UAE_STATE_PAUSED;

#ifdef SAVESTATE
    ra_set_disabled (g_sload, !running);
    ra_set_disabled (g_ssave, !running);
#else
    ra_set_disabled (g_sload, TRUE);
    ra_set_disabled (g_ssave, TRUE);
#endif
    ra_set_disabled (g_bsd, st != UAE_STATE_STOPPED);
}

/* Tooltips (mouse-over help). English text = key in Catalogs/E-UAE.cd. */
static const struct ra_hint hints[] = {
    { GID_DESC, 1, "Free-text description saved with the configuration." },
    { GID_LOAD, 1, "Load a configuration file and apply it." },
    { GID_SAVE, 1, "Save the current settings to the current configuration file." },
    { GID_SAVEAS, 1, "Save the current settings under a new file name." },
    { GID_STATE_LOAD, 1, "Load a saved emulation state (savestate)." },
    { GID_STATE_SAVE, 1, "Save the state of the running emulation to a file." },
    { GID_STARTGUI, 1, "Open this settings window every time E-UAE starts." },
    { GID_BSDSOCKET, 1, "Provide bsdsocket.library so emulated Amiga programs can use the host network." },
    { GID_TODHACK, 1, "Keep the Amiga clock in sync with the system time." },
    { 0, 0, NULL }
};

const struct ra_page ra_page_config = {
    "Configuration", PAGE_ID_CONFIG, create, read_prefs, write_prefs, handle, state, NULL, hints
};
