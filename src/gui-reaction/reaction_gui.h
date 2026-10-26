/*
 * E-UAE - ReAction GUI for AmigaOS 4.1 (PPC)
 *
 * Gemeinsame Deklarationen fuer Kern (gui_core.c), Hilfsfunktionen
 * (gui_util.c) und die einzelnen Seiten (page_*.c).
 *
 * Alle Einstellungen werden in 'changed_prefs' geschrieben - das ist die
 * gleiche Konvention wie bei der GTK-GUI. E-UAE uebernimmt sie beim Start
 * (currprefs = changed_prefs) bzw. waehrend der Emulation ueber die
 * check_prefs_changed_*()-Funktionen.
 */

#ifndef EUAE_REACTION_GUI_H
#define EUAE_REACTION_GUI_H

/* ---- E-UAE ----------------------------------------------------------- */
#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "gui.h"
#include "uae.h"
#include "disk.h"
#include "filesys.h"
#include "savestate.h"
#include "custom.h"
#include "inputdevice.h"

/* ---- AmigaOS 4 SDK --------------------------------------------------- */
#ifndef __USE_INLINE__
#define __USE_INLINE__
#endif

#include <exec/types.h>
#include <exec/exec.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <intuition/imageclass.h>
#include <intuition/icclass.h>
#include <libraries/asl.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <proto/asl.h>

#include <reaction/reaction.h>
#include <reaction/reaction_macros.h>

#include <classes/window.h>
#include <gadgets/layout.h>
#include <gadgets/button.h>
#include <gadgets/checkbox.h>
#include <gadgets/chooser.h>
#include <gadgets/clicktab.h>
#include <gadgets/integer.h>
#include <gadgets/string.h>
#include <gadgets/listbrowser.h>
#include <gadgets/space.h>
#include <images/label.h>

#include <proto/window.h>
#include <proto/layout.h>
#include <proto/button.h>
#include <proto/checkbox.h>
#include <proto/chooser.h>
#include <proto/clicktab.h>
#include <proto/integer.h>
#include <proto/string.h>
#include <proto/listbrowser.h>
#include <proto/space.h>
#include <proto/label.h>

/* ---------------------------------------------------------------------- */
/* Gadget-IDs                                                             */
/*                                                                        */
/*   1..99     Kern (Start, Pause, Reset, ...)                            */
/*   100..199  Seite 0 (Medien)        200..299  Seite 1 (Hardware)       */
/*   300..399  Seite 2 (Display)       400..499  Seite 3 (Sound)          */
/*   500..599  Seite 4 (Festplatten)   600..699  Seite 5 (Eingabe)        */
/*   700..799  Seite 6 (Konfiguration)                                    */
/* ---------------------------------------------------------------------- */
enum {
    GID_TABS = 1,
    GID_START,
    GID_PAUSE,
    GID_RESET,
    GID_STOP,
    GID_QUIT,
    GID_STATUS
};

#define GID_PAGE_BASE(n)   (100 * ((n) + 1))

enum {
    PAGE_ID_MEDIA = 0,
    PAGE_ID_HARDWARE,
    PAGE_ID_DISPLAY,
    PAGE_ID_SOUND,
    PAGE_ID_HARDDISK,
    PAGE_ID_INPUT,
    PAGE_ID_CONFIG,
    PAGE_ID_SHORTCUTS
};

/* Tooltip (Mouse-over-Hilfe) fuer ein Gadget bzw. count aufeinanderfolgende
 * Gadget-IDs. Tabellen enden mit text == NULL. */
struct ra_hint {
    ULONG       gid;
    ULONG       count;                   /* 0 oder 1 = nur dieses Gadget    */
    const char *text;                    /* englischer Text (-> ra_tr)      */
};

/* Beschreibung einer GUI-Seite */
struct ra_page {
    const char *title;                   /* Text im Reiter                   */
    int         id;                      /* PAGE_ID_xxx -> Gadget-ID-Bereich */
    Object   *(*create)(void);           /* Layout-Objekt bauen              */
    void      (*read)(void);             /* Gadgets  -> changed_prefs        */
    void      (*write)(void);            /* changed_prefs -> Gadgets         */
    int       (*handle)(ULONG gid, ULONG code); /* Gadget-Ereignis, 1=erledigt */
    void      (*state)(int state);       /* UAE_STATE_xxx geaendert          */
    void      (*cleanup)(void);          /* nach DisposeObject: Listen frei  */
    const struct ra_hint *hints;         /* Tooltips der Seite (oder NULL)   */
};

extern const struct ra_page *const ra_pages[];   /* in gui_pages.c */
extern const int                   ra_num_pages;

/* ---------------------------------------------------------------------- */
/* Kern -> Seiten                                                         */
/* ---------------------------------------------------------------------- */
#define RA_GUI_VERSION "1.0"                  /* GUI version (internal build: v17) */

extern struct Window *ra_window;                 /* offenes Fenster oder NULL */
extern Object        *ra_window_obj;
extern struct Window *ra_req_window;            /* Requester-Elternfenster oder NULL */

void ra_read_all  (void);                        /* alle Seiten: Gadgets -> Prefs */
void ra_write_all (void);                        /* alle Seiten: Prefs -> Gadgets */
int  ra_emu_stopped (void);                      /* Emulation gestoppt?           */
void ra_set_status_text (const char *text);
void ra_write_page (int page_id);                /* nur eine Seite aktualisieren  */
void ra_show_window (void);                      /* Fenster oeffnen/nach vorn     */

/* ---------------------------------------------------------------------- */
/* Hilfsfunktionen (gui_util.c)                                           */
/* ---------------------------------------------------------------------- */
BOOL ra_open_libs  (void);
void ra_close_libs (void);

/* Gadget-Attribute setzen/lesen */
#define RA_SET(obj, ...) \
    RefreshSetGadgetAttrs ((struct Gadget *)(obj), ra_window, NULL, __VA_ARGS__, TAG_END)

ULONG       ra_get_num  (Object *obj, ULONG tag);
const char *ra_get_text (Object *obj);           /* STRINGA_TextVal */
void        ra_set_text (Object *obj, const char *text);
void        ra_set_disabled (Object *obj, BOOL disabled);

/* Listen fuer Chooser / ClickTab */
struct List *ra_chooser_labels (const char *const *names);
void         ra_free_chooser_labels (struct List *list);
struct List *ra_clicktab_labels (const char *const *names);
void         ra_free_clicktab_labels (struct List *list);

/* Auswahllisten (Chooser) mit Wertetabelle */
struct ra_choice {
    const char *const *names;       /* NULL-terminiert              */
    const LONG        *values;      /* Wert je Eintrag              */
    int                count;
    struct List       *labels;      /* wird beim ersten Gadget angelegt */
};
#define RA_CHOICE(var, names_, values_) \
    static struct ra_choice var = { names_, values_, (int)(sizeof values_ / sizeof values_[0]), NULL }

Object *ra_choice_gadget (ULONG gid, struct ra_choice *c);
int     ra_choice_index  (Object *obj, const struct ra_choice *c);        /* gewaehlter Index */
LONG    ra_choice_value  (Object *obj, const struct ra_choice *c);        /* gewaehlter Wert  */
void    ra_choice_set    (Object *obj, const struct ra_choice *c, LONG value, int fallback_index);
void    ra_choice_free   (struct ra_choice *c);

ULONG   ra_names_width   (const char *const *names);   /* Mindestbreite in Pixel */

/* Layout-Bausteine */
Object *ra_chooser  (ULONG gid, const char *const *names);
Object *ra_checkbox (ULONG gid, const char *text);
Object *ra_integer  (ULONG gid, LONG min, LONG max, LONG val);
Object *ra_string   (ULONG gid, ULONG maxchars);
Object *ra_button   (ULONG gid, const char *text);
Object *ra_label    (const char *text);

/* ASL */
BOOL ra_request_file (const char *title, const char *pattern, BOOL save,
                      BOOL drawers_only, char *path, ULONG pathlen);

/* Zahl <-> Tabellenindex */
int ra_index_of (const ULONG *table, int count, ULONG value, int fallback);

/* Konfigurationsdateien (page_config.c / gui_core.c) */
void ra_error (const char *fmt, ...);

/* Locale (locale_support.c) */
void               ra_locale_open  (void);
void               ra_locale_close (void);
const char        *ra_tr           (const char *english);
const char *const *ra_tr_array     (const char *const *names);
void ra_info  (const char *fmt, ...);
void ra_message (const char *title, const char *msg);

/* Programm-Einstellungen, Menue, Info (gui_settings.c, v16) */
enum { RA_DIR_ROM, RA_DIR_FLOPPY, RA_DIR_HARDFILE, RA_DIR_CDIMAGE, RA_DIR_CONFIG, RA_DIR_SAVESTATE, RA_DIR_SCREENSHOT, RA_DIR_COUNT };
enum { RA_LOG_OFF, RA_LOG_WINDOW, RA_LOG_FILE };
struct ra_settings {
    char dir[RA_DIR_COUNT][MAX_DPATH];   /* "" = Vorgabe des Kerns */
    int  log_mode;                       /* RA_LOG_*              */
    int  iconify_on_start;               /* v17: GUI nach Start ikonifizieren */
};
extern struct ra_settings ra_settings;
void        ra_settings_init (void);
void        ra_settings_window (void);
const char *ra_settings_dir (int kind);
const char *gui_screenshot_dir (void);   /* v18, fuer ami-win.c screenshot() */
BOOL        ra_request_file_in (int kind, const char *title, const char *pattern, BOOL save,
                                BOOL drawers_only, char *path, ULONG pathlen);
void        ra_about (void);
Object     *ra_menu_create (void);
void        ra_menu_dispose (void);
int         ra_menu_pick (void);
BOOL ra_confirm (const char *fmt, ...);           /* Ja/Nein, TRUE = Ja */

#endif /* EUAE_REACTION_GUI_H */
