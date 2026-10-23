/*
 * E-UAE - ReAction GUI for AmigaOS 4.1 (PPC)
 *
 * Kern: implementiert die gui_*()-Schnittstelle von E-UAE (siehe gui.h),
 * baut das Hauptfenster (ClickTab + Seiten + Steuerleiste) und verteilt
 * Gadget-Ereignisse an die Seiten.
 *
 * Ablauf (siehe main.c / real_main()):
 *   - gui_open()           Fenster oeffnen, Prefs in Gadgets schreiben
 *   - gui_handle_events()  wird alle 10 ms (Start) bzw. pro Frame (laufende
 *                          Emulation) aufgerufen -> RA_HandleInput()
 *   - "Start"              uae_start(): die Warteschleife in real_main()
 *                          endet, currprefs = changed_prefs
 *   - gui_exit()           Fenster und Klassen freigeben
 */

#include "reaction_gui.h"

#include <workbench/icon.h>
#include <proto/icon.h>

#define MAX_PAGES 8

extern struct Library *WindowBase;   /* gui_util.c */

struct Window *ra_window;
Object        *ra_window_obj;

static Object *tabs_obj, *page_group;
static Object *btn_start, *btn_pause, *btn_reset, *btn_stop, *btn_quit, *status_obj;
static struct List *tab_labels;

static int      emu_state = UAE_STATE_STOPPED;

/* v17: iconify (window.class AppIcon on the Workbench) */
static int              ra_iconified;
static struct MsgPort  *app_port;
static struct DiskObject *app_icon;
static void ra_iconify (void);
static unsigned led_state;               /* Bit0 PWR, 1..4 DF0..3, 5 HD, 6 CD */
static int      cur_fps, cur_idle;
static int      status_dirty = 1;
static int      media_dirty;
static char     status_buf[2][160];
static int      status_flip;

/* ---------------------------------------------------------------------- */
/* Zugriff auf alle Seiten                                                */
/* ---------------------------------------------------------------------- */

void ra_read_all (void)
{
    int i;

    for (i = 0; i < ra_num_pages; i++)
	if (ra_pages[i]->read)
	    ra_pages[i]->read ();
}

void ra_write_all (void)
{
    int i;

    for (i = 0; i < ra_num_pages; i++)
	if (ra_pages[i]->write)
	    ra_pages[i]->write ();
}

void ra_write_page (int page_id)
{
    int i;

    for (i = 0; i < ra_num_pages; i++)
	if (ra_pages[i]->id == page_id && ra_pages[i]->write)
	    ra_pages[i]->write ();
}

int ra_emu_stopped (void)
{
    return emu_state == UAE_STATE_STOPPED;
}

/* ---------------------------------------------------------------------- */
/* Steuerleiste + Statuszeile                                             */
/* ---------------------------------------------------------------------- */

static void update_controls (void)
{
    int stopped = emu_state == UAE_STATE_STOPPED;
    int paused  = emu_state == UAE_STATE_PAUSED;
    int running = emu_state == UAE_STATE_RUNNING;
    int i;

    if (!ra_window_obj)
	return;

    ra_set_disabled (btn_start, !stopped);
    ra_set_disabled (btn_pause, !(running || paused));
    ra_set_disabled (btn_reset, !(running || paused));
    ra_set_disabled (btn_stop,  stopped);

    if (ra_window)
	RA_SET (btn_pause, GA_Text, paused ? ra_tr ("Resume") : ra_tr ("Pause"));

    for (i = 0; i < ra_num_pages; i++)
	if (ra_pages[i]->state)
	    ra_pages[i]->state (emu_state);

    status_dirty = 1;
}

void ra_set_status_text (const char *text)
{
    if (!status_obj)
	return;
    status_flip ^= 1;
    strncpy (status_buf[status_flip], text, sizeof status_buf[0] - 1);
    status_buf[status_flip][sizeof status_buf[0] - 1] = '\0';
    RA_SET (status_obj, GA_Text, status_buf[status_flip]);
}

static void update_status (void)
{
    static const char *const led_names[] = { "PWR", "DF0", "DF1", "DF2", "DF3", "HD", "CD" };
    const char *st;
    char line[160], leds[64];
    int i;

    switch (emu_state) {
	case UAE_STATE_RUNNING: st = ra_tr ("Running");  break;
	case UAE_STATE_PAUSED:  st = ra_tr ("Paused"); break;
	case UAE_STATE_STOPPED: st = ra_tr ("Stopped"); break;
	default:                st = ra_tr ("Starting..."); break;
    }

    leds[0] = '\0';
    for (i = 0; i < 7; i++)
	if (led_state & (1u << i)) {
	    if (leds[0])
		strcat (leds, " ");
	    strcat (leds, led_names[i]);
	}

    if (emu_state == UAE_STATE_RUNNING || emu_state == UAE_STATE_PAUSED) {
	/* custom.c liefert die Auslastung in Promille (1000 = voll), die
	 * Variable heisst dort nur "idle". Frueher stand hier 100 - x, das
	 * zeigte bei voller Last "CPU 0%". */
	int load = cur_idle / 10;
	if (load < 0)   load = 0;
	if (load > 100) load = 100;
	sprintf (line, "%s | %d.%d fps | CPU %d%% | %s", st, cur_fps / 10, cur_fps % 10, load, leds);
    } else
	sprintf (line, "%s", st);

    ra_set_status_text (line);
    status_dirty = 0;
}

/* ---------------------------------------------------------------------- */
/* Fenster                                                                */
/* ---------------------------------------------------------------------- */

static void dispose_window (void)
{
    int i;

    if (ra_window_obj) {
	if (ra_window)
	    RA_CloseWindow (ra_window_obj);
	DisposeObject (ra_window_obj);    /* also removes the AppIcon */
    }
    ra_iconified = 0;
    if (app_icon && IIcon)
	FreeDiskObject (app_icon);
    app_icon = NULL;
    if (app_port)
	FreeSysObject (ASOT_PORT, app_port);
    app_port = NULL;
    ra_menu_dispose ();
    ra_window     = NULL;
    ra_window_obj = NULL;
    tabs_obj = page_group = NULL;
    btn_start = btn_pause = btn_reset = btn_stop = btn_quit = status_obj = NULL;

    for (i = 0; i < ra_num_pages; i++)
	if (ra_pages[i]->cleanup)
	    ra_pages[i]->cleanup ();

    ra_free_clicktab_labels (tab_labels);
    tab_labels = NULL;
}

/* Tooltips (mouse-over help). English text = key in Catalogs/E-UAE.cd. */
static const struct ra_hint core_hints[] = {
    { GID_START, 1, "Start the emulation with the current settings." },
    { GID_PAUSE, 1, "Pause the running emulation, or continue it." },
    { GID_RESET, 1, "Reset the emulated Amiga, like pressing its reset button." },
    { GID_STOP, 1, "Stop the emulation and return to the settings." },
    { GID_QUIT, 1, "Quit E-UAE." },
    { 0, 0, NULL }
};

#define MAX_HINTS 256
static struct HintInfo hint_info[MAX_HINTS + 1];

static void add_hints (int *n, const struct ra_hint *h)
{
    for (; h && h->text; h++) {
	ULONG cnt = h->count ? h->count : 1, k;
	const char *txt = ra_tr (h->text);

	for (k = 0; k < cnt && *n < MAX_HINTS; k++) {
	    hint_info[*n].hi_GadgetID = (WORD) (h->gid + k);
	    hint_info[*n].hi_Code     = -1;
	    hint_info[*n].hi_Text     = (CONST_STRPTR) txt;
	    hint_info[*n].hi_Flags    = 0;
	    (*n)++;
	}
    }
}

/* Mouse-over-Hilfe: Array fuer WINDOW_HintInfo (wie im QuakeSpasmGUI).
 * Die Texte stammen aus dem Katalog und bleiben bis ra_locale_close gueltig. */
static void build_hints (void)
{
    int i, n = 0;

    add_hints (&n, core_hints);
    for (i = 0; i < ra_num_pages; i++)
	add_hints (&n, ra_pages[i]->hints);
    hint_info[n].hi_GadgetID = -1;          /* Ende-Marke */
    hint_info[n].hi_Code     = -1;
    hint_info[n].hi_Text     = NULL;
    hint_info[n].hi_Flags    = 0;
}

static BOOL build_window (void)
{
    const char *names[MAX_PAGES + 1];
    struct TagItem page_tags[MAX_PAGES + 1];
    int i;

    if (ra_num_pages > MAX_PAGES)
	return FALSE;

    for (i = 0; i < ra_num_pages; i++) {
	Object *pg = ra_pages[i]->create ();

	if (!pg) {
	    write_log ("ReAction GUI: Seite '%s' konnte nicht erzeugt werden.\n", ra_pages[i]->title);
	    return FALSE;
	}
	names[i]               = ra_pages[i]->title;
	page_tags[i].ti_Tag    = PAGE_Add;
	page_tags[i].ti_Data   = (ULONG) pg;
    }
    names[i]             = NULL;
    page_tags[i].ti_Tag  = TAG_END;
    page_tags[i].ti_Data = 0;

    tab_labels = ra_clicktab_labels (names);
    page_group = NewObjectA (PAGE_GetClass (), NULL, page_tags);
    if (!tab_labels || !page_group)
	return FALSE;

    build_hints ();

    /* v17: AppIcon for iconify - the program's icon, else a default tool icon */
    if (!app_port)
	app_port = AllocSysObjectTags (ASOT_PORT, TAG_END);
    if (!app_icon && IIcon) {
	app_icon = GetDiskObject ("PROGDIR:E-UAE");
	if (!app_icon)
	    app_icon = GetDiskObject ("PROGDIR:uae");
	if (!app_icon)
	    app_icon = GetDefDiskObject (WBTOOL);
    }

    ra_window_obj = WindowObject,
	WA_Title,         "E-UAE",
	WINDOW_IconifyGadget, app_port ? TRUE : FALSE,
	WINDOW_AppPort,       app_port,
	WINDOW_Icon,          app_icon,
	WINDOW_IconTitle,     "E-UAE",
	WINDOW_IconNoDispose, TRUE,
	WINDOW_MenuStrip, ra_menu_create (),     /* v16: rechte Maustaste */
	WINDOW_GadgetHelp, TRUE,
	WINDOW_HintInfo,   hint_info,
	WA_DragBar,       TRUE,
	WA_CloseGadget,   TRUE,
	WA_DepthGadget,   TRUE,
	WA_SizeGadget,    TRUE,
	WA_Activate,      TRUE,
	WINDOW_Position,  WPOS_CENTERSCREEN,
	WINDOW_ParentGroup, VLayoutObject,
	    LAYOUT_SpaceOuter, TRUE,

	    LAYOUT_AddChild, tabs_obj = ClickTabObject,
		GA_ID,              GID_TABS,
		GA_RelVerify,       TRUE,
		CLICKTAB_Labels,    tab_labels,
		CLICKTAB_PageGroup, page_group,
	    ClickTabEnd,

	    LAYOUT_AddChild, HLayoutObject,
		LAYOUT_EvenSize, TRUE,
		LAYOUT_AddChild, btn_start = ra_button (GID_START, "Start"),
		LAYOUT_AddChild, btn_pause = ra_button (GID_PAUSE, "Pause"),
		LAYOUT_AddChild, btn_reset = ra_button (GID_RESET, "Reset"),
		LAYOUT_AddChild, btn_stop  = ra_button (GID_STOP,  "Stop"),
		LAYOUT_AddChild, btn_quit  = ra_button (GID_QUIT,  "Quit"),
	    LayoutEnd,
	    CHILD_WeightedHeight, 0,

	    LAYOUT_AddChild, status_obj = ButtonObject,
		GA_ID,       GID_STATUS,
		GA_ReadOnly, TRUE,
		GA_Text,     "",
	    ButtonEnd,
	    CHILD_WeightedHeight, 0,
	LayoutEnd,
    WindowEnd;

    if (!ra_window_obj)
	return FALSE;
    return TRUE;
}

void ra_show_window (void)
{
    if (!ra_open_libs ())
	return;

    if (!ra_window_obj && !build_window ()) {
	write_log ("ReAction GUI: Fenster konnte nicht erzeugt werden.\n");
	dispose_window ();
	return;
    }

    if (ra_window) {
	WindowToFront (ra_window);
	ActivateWindow (ra_window);
	return;
    }

    ra_window = RA_OpenWindow (ra_window_obj);   /* also uniconifies */
    if (!ra_window) {
	write_log ("ReAction GUI: Fenster konnte nicht geoeffnet werden.\n");
	return;
    }
    ra_iconified = 0;
    ra_write_all ();
    update_controls ();
    update_status ();
}

static void hide_window (void)
{
    if (ra_window_obj && ra_window) {
	RA_CloseWindow (ra_window_obj);
	ra_window = NULL;
    }
}

/* ---------------------------------------------------------------------- */
/* Ereignisse                                                             */
/* ---------------------------------------------------------------------- */

static void on_control (ULONG gid)
{
    switch (gid) {
	case GID_START:
	    if (emu_state == UAE_STATE_STOPPED) {
		ra_read_all ();
		/* v17: no Kickstart -> say so instead of booting UAE's
		 * replacement ROM (the default "kick.rom" is gone) */
		if (!changed_prefs.romfile[0]) {
		    ra_error ("Please choose a Kickstart ROM on the Media page.");
		    break;
		}
		uae_start ();
		/* v17: GUI out of the way while the emulation runs */
		if (ra_settings.iconify_on_start)
		    ra_iconify ();
	    }
	    break;

	case GID_PAUSE:
	    ra_read_all ();
	    if (emu_state == UAE_STATE_RUNNING)
		uae_pause ();
	    else if (emu_state == UAE_STATE_PAUSED)
		uae_resume ();
	    break;

	case GID_RESET:
	    ra_read_all ();
	    uae_reset (1);
	    break;

	case GID_STOP:
	    /* Emulation beenden und zurueck zur Konfiguration */
	    uae_restart (-1, NULL);
	    break;

	case GID_QUIT:
	    uae_quit ();
	    break;
    }
}

static void on_gadget (ULONG gid, ULONG code)
{
    int i;

    if (gid < 100) {
	if (gid != GID_TABS)
	    on_control (gid);
	return;
    }

    for (i = 0; i < ra_num_pages; i++) {
	const struct ra_page *pg = ra_pages[i];
	ULONG base = GID_PAGE_BASE (pg->id);

	if (gid >= base && gid < base + 100) {
	    if (pg->read)
		pg->read ();
	    if (pg->handle)
		pg->handle (gid, code);
	    return;
	}
    }
}

static void on_close_window (void)
{
    if (emu_state == UAE_STATE_STOPPED)
	uae_quit ();          /* Wartebildschirm: Schliessen = Beenden */
    else
	hide_window ();       /* sonst nur ausblenden (Hotkey/gui_display() holt es zurueck) */
}

/* v17: window -> AppIcon on the Workbench */
static void ra_iconify (void)
{
    if (!ra_window_obj || !ra_window || !app_port)
	return;
    if (RA_Iconify (ra_window_obj)) {
	ra_window = NULL;
	ra_iconified = 1;
    }
}

static void process_events (void)
{
    ULONG result, code;

    while (ra_window_obj && (ra_window || ra_iconified) &&
	   (result = RA_HandleInput (ra_window_obj, &code)) != WMHI_LASTMSG)
    {
	switch (result & WMHI_CLASSMASK) {
	    case WMHI_CLOSEWINDOW:
		on_close_window ();
		break;
	    case WMHI_GADGETUP:
		on_gadget (result & WMHI_GADGETMASK, code);
		break;
	    case WMHI_ICONIFY:           /* v17 */
		ra_iconify ();
		break;
	    case WMHI_UNICONIFY:
		ra_show_window ();
		break;
	    case WMHI_MENUPICK:          /* v16: Menue */
		if (ra_menu_pick ())
		    uae_quit ();
		break;
	    default:
		break;
	}
    }
}

/* ---------------------------------------------------------------------- */
/* gui_*() - Schnittstelle zu E-UAE                                       */
/* ---------------------------------------------------------------------- */

void gui_init (int argc, char **argv)
{
    /* v16: Programm-Einstellungen laden (Ordner, Log-Ausgabe) - vor der
     * ersten Ausgabe, damit "Log aus" kein Ausgabefenster oeffnet */
    ra_settings_init ();
}

int gui_open (void)
{
    ra_show_window ();
    if (!ra_window) {
	write_log ("ReAction GUI nicht verfuegbar - starte ohne GUI.\n");
	return -1;
    }
    /* Nach einem Neustart der Emulation (Stop, Konfiguration laden) stehen
       in changed_prefs neue Werte - in die Gadgets uebernehmen. */
    ra_write_all ();
    update_controls ();
    write_log ("ReAction GUI: linemode=%d color_mode=%d win=%dx%d lores=%d\n",
	       (int) changed_prefs.gfx_linedbl, (int) changed_prefs.color_mode,
	       (int) changed_prefs.gfx_width_win, (int) changed_prefs.gfx_height_win,
	       (int) changed_prefs.gfx_lores);
    return 0;
}

void gui_exit (void)
{
    if (!ra_window_obj && !WindowBase)
	return;                  /* schon aufgeraeumt */
    dispose_window ();
    ra_locale_close ();
    ra_close_libs ();
}

int gui_update (void)
{
    status_dirty = 1;
    return 0;
}

void gui_notify_state (int state)
{
    emu_state = state;
    if (ra_window_obj)
	update_controls ();
}

void gui_led (int led, int on)
{
    if (led < 0 || led > 6)
	return;
    if (on)
	led_state |= 1u << led;
    else
	led_state &= ~(1u << led);
    status_dirty = 1;
}

void gui_fps (int fps, int idle)
{
    gui_data.fps  = fps;
    gui_data.idle = idle;
    cur_fps  = fps;
    cur_idle = idle;
    status_dirty = 1;
}

void gui_hd_led (int led)
{
    static int resetcounter;
    int old = gui_data.hd;

    if (led == 0) {
	resetcounter--;
	if (resetcounter > 0)
	    return;
    }
    gui_data.hd  = led;
    resetcounter = 6;
    if (old != gui_data.hd)
	gui_led (5, gui_data.hd);
}

void gui_cd_led (int led)
{
    static int resetcounter;
    int old = gui_data.cd;

    if (led == 0) {
	resetcounter--;
	if (resetcounter > 0)
	    return;
    }
    gui_data.cd  = led;
    resetcounter = 6;
    if (old != gui_data.cd)
	gui_led (6, gui_data.cd);
}

void gui_filename (int num, const char *name)
{
    media_dirty = 1;           /* Disk wurde von der Emulation gewechselt */
}

void gui_handle_events (void)
{
    static unsigned tick;

    if (!ra_window_obj)
	return;

    if (ra_window || ra_iconified)
	process_events ();

    if (ra_window && (++tick % 8) == 0) {
	if (media_dirty) {
	    media_dirty = 0;
	    ra_write_page (PAGE_ID_MEDIA);
	}
	if (status_dirty)
	    update_status ();
    }
}

/* ---------------------------------------------------------------------- */
/* Hotkeys: Floppy einlegen, Savestates                                   */
/* ---------------------------------------------------------------------- */

static char last_floppy[MAX_DPATH];

static void quick_insert (int drive)
{
    char path[MAX_DPATH];
    char title[128];

    strcpy (path, last_floppy[0] ? last_floppy : changed_prefs.df[drive]);
    /* v17: snprintf - a long translation overflowed title[48] */
    snprintf (title, sizeof title, ra_tr ("Select image for DF%d"), drive);

    if (ra_request_file_in (RA_DIR_FLOPPY, title, "#?.(adf|adz|dms|ipf|zip|gz|lha|lzh|lzx|dsk)", FALSE, FALSE, path, sizeof path)) {
	strncpy (last_floppy, path, sizeof last_floppy - 1);
	strncpy (changed_prefs.df[drive], path, sizeof changed_prefs.df[drive] - 1);
	changed_prefs.df[drive][sizeof changed_prefs.df[drive] - 1] = '\0';
	ra_write_page (PAGE_ID_MEDIA);
    }
}

#ifdef SAVESTATE
static char last_state[MAX_DPATH];

static void quick_state (int save)
{
    char path[MAX_DPATH];

    strcpy (path, last_state);
    if (!ra_request_file_in (RA_DIR_SAVESTATE, save ? "Save savestate" : "Load savestate",
			  "#?.uss", save, FALSE, path, sizeof path))
	return;

    strncpy (last_state, path, sizeof last_state - 1);
    if (save) {
	savestate_initsave (path, 1);
	save_state (path, "E-UAE ReAction");
    } else {
	savestate_initsave (path, 1);
	savestate_state = STATE_DORESTORE;
	write_log ("Restoring state from '%s'...\n", path);
    }
}
#endif

void gui_display (int shortcut)
{
    if (!ra_window_obj || !ra_window)
	ra_show_window ();

    switch (shortcut) {
	case 0: case 1: case 2: case 3:
	    quick_insert (shortcut);
	    break;
#ifdef SAVESTATE
	case 4: quick_state (0); break;
	case 5: quick_state (1); break;
#endif
	default:                 /* -1: nur Fenster zeigen */
	    break;
    }
}

void gui_message (const char *format, ...)
{
    char msg[2048];
    va_list parms;

    va_start (parms, format);
    vsnprintf (msg, sizeof msg, format, parms);
    va_end (parms);

    write_log ("%s", msg);
    if (IIntuition)
	ra_info ("%s", msg);
}
