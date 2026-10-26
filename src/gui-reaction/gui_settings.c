/*
 * E-UAE - ReAction GUI for AmigaOS 4.1 (PPC)
 *
 * v16: Programm-Einstellungen, Menue (rechte Maustaste) und Info-Fenster
 *
 *   Menue "Projekt": Ueber..., Einstellungen..., Beenden
 *
 *   Einstellungen (PROGDIR:E-UAE.prefs, gelten fuer alle Konfigurationen,
 *   werden bei jedem Programmstart geladen):
 *     - Ordner fuer Kickstart-ROMs, Disketten, Hardfiles, CD-Images,
 *       Konfigurationen, Savestates (v18: und Screenshots): Startordner der Dateirequester, wenn
 *       das zugehoerige Feld noch leer ist. Leer = Vorgabe des E-UAE-Kerns
 *       (rom_path, floppy_path, hardfile_path, savestate_path aus
 *       targets/t-amiga.h, Konfigurationen PROGDIR:configs wie in
 *       od-amiga/main.c), CD-Images PROGDIR:Isos/ (v18, vorher keine
 *       Vorgabe), Screenshots PROGDIR:Screenshots (v18).
 *     - Log-Ausgabe: aus (stdout/stderr nach NIL:), Fenster (wie bisher)
 *       oder Datei PROGDIR:E-UAE.log. "Aus" und "Datei" wirken sofort,
 *       "Fenster" beim naechsten Programmstart (ein umgeleiteter Ausgabekanal
 *       laesst sich nicht wieder auf das Ausgabefenster zuruecksetzen).
 */

#include "reaction_gui.h"
#include "writelog.h"

#include <intuition/menuclass.h>
#include <stdio.h>

#define SETTINGS_FILE "PROGDIR:E-UAE.prefs"
#define LOG_FILE      "PROGDIR:E-UAE.log"

struct ra_settings ra_settings;

static const char *const dir_keys[RA_DIR_COUNT] = {
    "rom_dir", "floppy_dir", "hardfile_dir", "cdimage_dir", "config_dir", "savestate_dir", "screenshot_dir"
};
static const char *const log_keys[] = { "off", "window", "file", NULL };

static int log_applied = RA_LOG_WINDOW;     /* Zustand der Ausgabekanaele */

/* ---------------------------------------------------------------------- */
/* Ordner                                                                 */
/* ---------------------------------------------------------------------- */

/* Vorgabe des Kerns fuer eine Ordnerart (kann NULL / leer sein) */
static const char *core_dir (int kind)
{
    switch (kind) {
	case RA_DIR_ROM:       return prefs_get_attr ("rom_path");
	case RA_DIR_FLOPPY:    return prefs_get_attr ("floppy_path");
	case RA_DIR_HARDFILE:  return prefs_get_attr ("hardfile_path");
	case RA_DIR_SAVESTATE: return prefs_get_attr ("savestate_path");
	case RA_DIR_CDIMAGE:   return "PROGDIR:Isos/";	/* v18 (before: none) */
	case RA_DIR_CONFIG:    return "PROGDIR:configs";
	case RA_DIR_SCREENSHOT: return "PROGDIR:Screenshots";
	default:               return NULL;
    }
}

const char *ra_settings_dir (int kind)
{
    const char *d;

    if (kind < 0 || kind >= RA_DIR_COUNT)
	return "";
    if (ra_settings.dir[kind][0])
	return ra_settings.dir[kind];
    d = core_dir (kind);
    return d ? d : "";
}

/* Dateirequester: ist das Feld leer, beginnt er im eingestellten Ordner */
BOOL ra_request_file_in (int kind, const char *title, const char *pattern, BOOL save,
			 BOOL drawers_only, char *path, ULONG pathlen)
{
    if (path && !path[0]) {
	const char *d = ra_settings_dir (kind);
	if (d[0]) {
	    strncpy (path, d, pathlen - 1);
	    path[pathlen - 1] = '\0';
	    /* ra_request_file() nimmt den Ordner aus PathPart(): ein
	     * Trennzeichen anhaengen, damit der letzte Ordner nicht als
	     * Dateiname gilt */
	    if (!drawers_only && path[strlen (path) - 1] != '/' && path[strlen (path) - 1] != ':'
		&& strlen (path) + 1 < pathlen)
		strcat (path, "/");
	}
    }
    return ra_request_file (title, pattern, save, drawers_only, path, pathlen);
}

/* v18: Screenshot-Ordner fuer screenshot() in gfx-amigaos/ami-win.c */
const char *gui_screenshot_dir (void)
{
    return ra_settings_dir (RA_DIR_SCREENSHOT);
}

/* ---------------------------------------------------------------------- */
/* Log-Ausgabe                                                            */
/* ---------------------------------------------------------------------- */

static void apply_log (int mode)
{
    if (mode == log_applied)
	return;
    switch (mode) {
	case RA_LOG_OFF:
	    fflush (stdout);
	    fflush (stderr);
	    freopen ("NIL:", "w", stdout);
	    freopen ("NIL:", "w", stderr);
	    write_log_output = 2;            /* none */
	    break;
	case RA_LOG_FILE:
	    fflush (stdout);
	    fflush (stderr);
	    if (freopen (LOG_FILE, "w", stdout)) {
		freopen (LOG_FILE, "a", stderr);
		setvbuf (stdout, NULL, _IOLBF, 0);
		setvbuf (stderr, NULL, _IOLBF, 0);
	    }
	    write_log_output = 0;            /* console = stderr = Datei */
	    break;
	default:
	    /* Fenster: nur beim Programmstart moeglich (Ausgangszustand) */
	    write_log_output = 0;
	    return;
    }
    log_applied = mode;
}

/* ---------------------------------------------------------------------- */
/* Laden / Speichern                                                      */
/* ---------------------------------------------------------------------- */

static void settings_defaults (void)
{
    memset (&ra_settings, 0, sizeof ra_settings);
    ra_settings.log_mode = RA_LOG_WINDOW;
    ra_settings.iconify_on_start = 1;        /* v17 */
}

static void settings_load (void)
{
    FILE *f;
    char line[MAX_DPATH + 64];

    settings_defaults ();
    f = fopen (SETTINGS_FILE, "r");
    if (!f)
	return;
    while (fgets (line, sizeof line, f)) {
	char *v = strchr (line, '=');
	int i, l = strlen (line);

	while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r'))
	    line[--l] = '\0';
	if (!v || line[0] == ';' || line[0] == '#')
	    continue;
	*v++ = '\0';
	for (i = 0; i < RA_DIR_COUNT; i++)
	    if (!strcmp (line, dir_keys[i])) {
		strncpy (ra_settings.dir[i], v, MAX_DPATH - 1);
		ra_settings.dir[i][MAX_DPATH - 1] = '\0';
	    }
	if (!strcmp (line, "iconify_on_start"))
	    ra_settings.iconify_on_start = !strcmp (v, "yes");
	if (!strcmp (line, "log"))
	    for (i = 0; log_keys[i]; i++)
		if (!strcmp (v, log_keys[i]))
		    ra_settings.log_mode = i;
    }
    fclose (f);
}

static int settings_save (void)
{
    FILE *f = fopen (SETTINGS_FILE, "w");
    int i;

    if (!f)
	return 0;
    fprintf (f, "; E-UAE ReAction GUI settings (Settings window)\n");
    for (i = 0; i < RA_DIR_COUNT; i++)
	fprintf (f, "%s=%s\n", dir_keys[i], ra_settings.dir[i]);
    fprintf (f, "log=%s\n", log_keys[ra_settings.log_mode]);
    fprintf (f, "iconify_on_start=%s\n", ra_settings.iconify_on_start ? "yes" : "no");
    return fclose (f) == 0;
}

/* beim Programmstart (gui_init): Einstellungen laden, Log umleiten */
void ra_settings_init (void)
{
    settings_load ();
    if (ra_settings.log_mode == RA_LOG_WINDOW)
	write_log_output = 0;
    else
	apply_log (ra_settings.log_mode);
}

/* ---------------------------------------------------------------------- */
/* Einstellungsfenster                                                    */
/* ---------------------------------------------------------------------- */

enum { SID_OK = 1, SID_CANCEL, SID_LOG, SID_ICONIFY, SID_DIR = 10, SID_SEL = 20 };

static const char *const dir_labels[RA_DIR_COUNT] = {
    "Kickstart ROMs:", "Floppy images:", "Hardfiles:", "CD images:", "Configurations:", "Savestates:", "Screenshots:"
};
static const char *const log_names[] = { "Off", "Window", "File (PROGDIR:E-UAE.log)", NULL };

static Object *s_dir[RA_DIR_COUNT], *s_log, *s_iconify;
static struct Window *s_win;

#define SET_SW(obj, ...) RefreshSetGadgetAttrs ((struct Gadget *)(obj), s_win, NULL, __VA_ARGS__, TAG_END)

static const struct ra_hint settings_hints[] = {
    { SID_DIR + RA_DIR_ROM, 1, "Folder for Kickstart ROM images. The file requesters of the GUI start here when no file is chosen yet." },
    { SID_DIR + RA_DIR_FLOPPY, 1, "Folder for floppy disk images (ADF, DMS, ...)." },
    { SID_DIR + RA_DIR_HARDFILE, 1, "Folder for hardfiles and directories used as drives." },
    { SID_DIR + RA_DIR_CDIMAGE, 1, "Folder for CD images (ISO, CUE/BIN)." },
    { SID_DIR + RA_DIR_CONFIG, 1, "Folder for configuration files (Load / Save as)." },
    { SID_DIR + RA_DIR_SAVESTATE, 1, "Folder for savestates." },
    { SID_DIR + RA_DIR_SCREENSHOT, 1, "Folder for screenshots (hotkey Ctrl+Alt+P). Created when it does not exist yet." },
    { SID_SEL, RA_DIR_COUNT, "Choose the folder with a requester." },
    { SID_LOG, 1, "Where the E-UAE log goes. Off: no output window. Window: the output window as before (takes effect at the next start). File: PROGDIR:E-UAE.log, new at every start - useful for bug reports." },
    { SID_ICONIFY, 1, "When the emulation is started, the GUI window becomes an icon on the Workbench (double-click it to get the window back). The window can also be iconified at any time with the iconify gadget in its title bar." },
    { SID_OK, 1, "Save the settings and close the window." },
    { SID_CANCEL, 1, "Close the window without changes." },
    { 0, 0, NULL }
};

#define S_MAX_HINTS 24
static struct HintInfo s_hints[S_MAX_HINTS + 1];

static struct HintInfo *build_settings_hints (void)
{
    const struct ra_hint *h;
    int n = 0;
    ULONG k;

    for (h = settings_hints; h->text; h++)
	for (k = 0; k < (h->count ? h->count : 1) && n < S_MAX_HINTS; k++, n++) {
	    s_hints[n].hi_GadgetID = (WORD) (h->gid + k);
	    s_hints[n].hi_Code     = -1;
	    s_hints[n].hi_Text     = (CONST_STRPTR) ra_tr (h->text);
	    s_hints[n].hi_Flags    = 0;
	}
    s_hints[n].hi_GadgetID = -1;
    s_hints[n].hi_Code     = -1;
    s_hints[n].hi_Text     = NULL;
    s_hints[n].hi_Flags    = 0;
    return s_hints;
}

static Object *dir_row (int i)
{
    return HLayoutObject,
		LAYOUT_AddChild, s_dir[i] = ra_string (SID_DIR + i, MAX_DPATH - 1),
		LAYOUT_AddChild, ra_button (SID_SEL + i, "..."),
		CHILD_WeightedWidth, 0,
	   LayoutEnd;
}

static int settings_commit (void)
{
    int i, mode = ra_get_num (s_log, CHOOSER_Selected);

    for (i = 0; i < RA_DIR_COUNT; i++) {
	const char *t = ra_get_text (s_dir[i]);
	const char *core = core_dir (i);
	/* unveraenderte Kern-Vorgabe nicht festschreiben */
	if (core && !strcmp (t, core))
	    t = "";
	strncpy (ra_settings.dir[i], t, MAX_DPATH - 1);
	ra_settings.dir[i][MAX_DPATH - 1] = '\0';
    }
    if (mode < 0 || mode > RA_LOG_FILE)
	mode = RA_LOG_WINDOW;
    ra_settings.log_mode = mode;
    ra_settings.iconify_on_start = ra_get_num (s_iconify, GA_Selected) ? 1 : 0;
    if (!settings_save ())
	ra_error ("Could not save the settings to '%s'.", SETTINGS_FILE);
    apply_log (mode);
    return 1;
}

void ra_settings_window (void)
{
    static struct Requester lock_req;
    Object *obj, *content;
    ULONG sigmask = 0, result, code;
    int done = 0, i, locked = 0;

    content = VLayoutObject,
	LAYOUT_AddChild, VLayoutObject,
	    LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("Folders"), LAYOUT_SpaceOuter, TRUE,
	    LAYOUT_AddChild, dir_row (0), CHILD_Label, ra_label (dir_labels[0]),
	    LAYOUT_AddChild, dir_row (1), CHILD_Label, ra_label (dir_labels[1]),
	    LAYOUT_AddChild, dir_row (2), CHILD_Label, ra_label (dir_labels[2]),
	    LAYOUT_AddChild, dir_row (3), CHILD_Label, ra_label (dir_labels[3]),
	    LAYOUT_AddChild, dir_row (4), CHILD_Label, ra_label (dir_labels[4]),
	    LAYOUT_AddChild, dir_row (5), CHILD_Label, ra_label (dir_labels[5]),
	    LAYOUT_AddChild, dir_row (6), CHILD_Label, ra_label (dir_labels[6]),
	LayoutEnd,
	CHILD_WeightedHeight, 0,
	LAYOUT_AddChild, VLayoutObject,
	    LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("Log"), LAYOUT_SpaceOuter, TRUE,
	    LAYOUT_AddChild, s_log = ra_chooser (SID_LOG, log_names),
	    CHILD_MinWidth, ra_names_width (log_names), CHILD_WeightedWidth, 0,
	    CHILD_Label, ra_label ("Log output:"),
	LayoutEnd,
	CHILD_WeightedHeight, 0,
	LAYOUT_AddChild, VLayoutObject,
	    LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("Window"), LAYOUT_SpaceOuter, TRUE,
	    LAYOUT_AddChild, s_iconify = ra_checkbox (SID_ICONIFY, "Iconify the GUI after Start"),
	LayoutEnd,
	CHILD_WeightedHeight, 0,
    LayoutEnd;
    if (!content)
	return;

    obj = WindowObject,
	WA_Title,          ra_tr ("E-UAE settings"),
	WA_DragBar,        TRUE,
	WA_CloseGadget,    TRUE,
	WA_DepthGadget,    TRUE,
	WA_SizeGadget,     TRUE,
	WA_Activate,       TRUE,
	WA_InnerWidth,     420,
	WINDOW_Position,   WPOS_CENTERWINDOW,
	WINDOW_RefWindow,  ra_window,
	WINDOW_GadgetHelp, TRUE,
	WINDOW_HintInfo,   build_settings_hints (),
	WINDOW_ParentGroup, VLayoutObject,
	    LAYOUT_SpaceOuter, TRUE,
	    LAYOUT_AddChild, content,
	    LAYOUT_AddChild, HLayoutObject,
		LAYOUT_EvenSize, TRUE,
		LAYOUT_AddChild, ra_button (SID_OK, "Save"),
		LAYOUT_AddChild, SpaceObject, SpaceEnd,
		LAYOUT_AddChild, ra_button (SID_CANCEL, "Cancel"),
	    LayoutEnd,
	    CHILD_WeightedHeight, 0,
	LayoutEnd,
    WindowEnd;
    if (!obj)
	return;

    s_win = RA_OpenWindow (obj);
    if (!s_win) {
	DisposeObject (obj);
	return;
    }
    if (ra_window) {                     /* Hauptfenster sperren */
	InitRequester (&lock_req);
	locked = Request (&lock_req, ra_window) ? 1 : 0;
	SetWindowPointer (ra_window, WA_BusyPointer, TRUE, TAG_END);
    }
    ra_req_window = s_win;

    for (i = 0; i < RA_DIR_COUNT; i++)
	SET_SW (s_dir[i], STRINGA_TextVal, ra_settings_dir (i));
    SET_SW (s_log, CHOOSER_Selected, ra_settings.log_mode);
    SET_SW (s_iconify, GA_Selected, ra_settings.iconify_on_start ? TRUE : FALSE);

    GetAttr (WINDOW_SigMask, obj, &sigmask);
    while (!done) {
	Wait (sigmask);
	while ((result = RA_HandleInput (obj, &code)) != WMHI_LASTMSG) {
	    ULONG gid = result & WMHI_GADGETMASK;

	    switch (result & WMHI_CLASSMASK) {
		case WMHI_CLOSEWINDOW:
		    done = 1;
		    break;
		case WMHI_GADGETUP:
		    if (gid == SID_CANCEL)
			done = 1;
		    else if (gid == SID_OK)
			done = settings_commit ();
		    else if (gid >= SID_SEL && gid < SID_SEL + RA_DIR_COUNT) {
			char path[MAX_DPATH];
			int k = gid - SID_SEL;

			strncpy (path, ra_get_text (s_dir[k]), sizeof path - 1);
			path[sizeof path - 1] = '\0';
			if (ra_request_file (dir_labels[k], NULL, FALSE, TRUE, path, sizeof path))
			    SET_SW (s_dir[k], STRINGA_TextVal, path);
		    }
		    break;
		default:
		    break;
	    }
	    if (done)
		break;
	}
    }

    ra_req_window = NULL;
    RA_CloseWindow (obj);
    DisposeObject (obj);
    s_win = NULL;
    for (i = 0; i < RA_DIR_COUNT; i++)
	s_dir[i] = NULL;
    s_log = NULL;
    s_iconify = NULL;
    if (ra_window) {
	if (locked)
	    EndRequest (&lock_req, ra_window);
	SetWindowPointer (ra_window, TAG_END);
    }
}

/* ---------------------------------------------------------------------- */
/* Info-Fenster                                                           */
/* ---------------------------------------------------------------------- */

void ra_about (void)
{
    char msg[1024];

    snprintf (msg, sizeof msg, "%s\n%s %s\n\n%s\n%s\n%s\n%s\n\n%s",
	      PACKAGE_STRING " - ReAction GUI " RA_GUI_VERSION,
	      ra_tr ("AmigaOS 4.1 PPC with JIT, built"), __DATE__,
	      ra_tr ("E-UAE by Richard Drummond, based on UAE by Bernd Schmidt and WinUAE by Toni Wilen."),
	      ra_tr ("PowerPC JIT compiler by \xC1lmos Rajnai."),
	      ra_tr ("AmigaOS 4 updates by Kjetil Hvalstrand, Fredrik Wikstrom and Josef Wegner."),
	      ra_tr ("ReAction GUI and extensions (v1 - v17) by Falke_34."),
	      ra_tr ("Free software under the GNU General Public License."));
    ra_message (ra_tr ("About E-UAE"), msg);
}

/* ---------------------------------------------------------------------- */
/* Menue (rechte Maustaste, menuclass von AmigaOS 4.1)                    */
/* ---------------------------------------------------------------------- */

enum { MID_ABOUT = 1, MID_SETTINGS, MID_QUIT };

static Object *menu_strip;

Object *ra_menu_create (void)
{
    if (menu_strip)
	return menu_strip;
    menu_strip = MStrip,
	MA_AddChild, MTitle (ra_tr ("Project")),
	    MA_AddChild, MItem (ra_tr ("About...")), MA_ID, MID_ABOUT, MA_Key, "?", MItemEnd,
	    MA_AddChild, MItem (ra_tr ("Settings...")), MA_ID, MID_SETTINGS, MA_Key, "E", MItemEnd,
	    MA_AddChild, MSeparator, MSeparatorEnd,
	    MA_AddChild, MItem (ra_tr ("Quit")), MA_ID, MID_QUIT, MA_Key, "Q", MItemEnd,
	MTitleEnd,
    MStripEnd;
    return menu_strip;
}

void ra_menu_dispose (void)
{
    if (menu_strip) {
	DisposeObject (menu_strip);
	menu_strip = NULL;
    }
}

/* WMHI_MENUPICK: alle gewaehlten Punkte abarbeiten. Liefert 1, wenn
 * "Beenden" gewaehlt wurde (der Aufrufer beendet dann E-UAE). */
int ra_menu_pick (void)
{
    uint32 id = NO_MENU_ID;
    int quit = 0;

    if (!menu_strip)
	return 0;
    while ((id = IDoMethod (menu_strip, MM_NEXTSELECT, 0, id)) != NO_MENU_ID) {
	switch (id) {
	    case MID_ABOUT:    ra_about ();           break;
	    case MID_SETTINGS: ra_settings_window (); break;
	    case MID_QUIT:     quit = 1;              break;
	}
    }
    return quit;
}
