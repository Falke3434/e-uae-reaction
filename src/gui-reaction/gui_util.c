/*
 * E-UAE - ReAction GUI for AmigaOS 4.1 (PPC)
 *
 * Hilfsfunktionen: Klassen-Bibliotheken, Gadget-Zugriff, Labels,
 * ASL-Dateirequester, Requester-Meldungen.
 */

#include "reaction_gui.h"
#include <proto/graphics.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* Bibliotheken / Interfaces der ReAction-Klassen                         */
/* ---------------------------------------------------------------------- */

struct Library *WindowBase, *LayoutBase, *ButtonBase, *CheckBoxBase, *ChooserBase,
               *ClickTabBase, *IntegerBase, *StringBase, *ListBrowserBase,
               *SpaceBase, *LabelBase;

struct WindowIFace      *IWindow;
struct LayoutIFace      *ILayout;
struct ButtonIFace      *IButton;
struct CheckBoxIFace    *ICheckBox;
struct ChooserIFace     *IChooser;
struct ClickTabIFace    *IClickTab;
struct IntegerIFace     *IInteger;
struct StringIFace      *IString;
struct ListBrowserIFace *IListBrowser;
struct SpaceIFace       *ISpace;
struct LabelIFace       *ILabel;

struct ra_lib {
    const char        *name;
    struct Library   **base;
    struct Interface **iface;
};

static const struct ra_lib ra_libs[] = {
    { "window.class",              &WindowBase,      (struct Interface **) &IWindow      },
    { "gadgets/layout.gadget",     &LayoutBase,      (struct Interface **) &ILayout      },
    { "gadgets/button.gadget",     &ButtonBase,      (struct Interface **) &IButton      },
    { "gadgets/checkbox.gadget",   &CheckBoxBase,    (struct Interface **) &ICheckBox    },
    { "gadgets/chooser.gadget",    &ChooserBase,     (struct Interface **) &IChooser     },
    { "gadgets/clicktab.gadget",   &ClickTabBase,    (struct Interface **) &IClickTab    },
    { "gadgets/integer.gadget",    &IntegerBase,     (struct Interface **) &IInteger     },
    { "gadgets/string.gadget",     &StringBase,      (struct Interface **) &IString      },
    { "gadgets/listbrowser.gadget",&ListBrowserBase, (struct Interface **) &IListBrowser },
    { "gadgets/space.gadget",      &SpaceBase,       (struct Interface **) &ISpace       },
    { "images/label.image",        &LabelBase,       (struct Interface **) &ILabel       },
    { NULL, NULL, NULL }
};

void ra_close_libs (void)
{
    const struct ra_lib *l;

    for (l = ra_libs; l->name; l++) {
	if (*l->iface) {
	    DropInterface (*l->iface);
	    *l->iface = NULL;
	}
	if (*l->base) {
	    CloseLibrary (*l->base);
	    *l->base = NULL;
	}
    }
}

BOOL ra_open_libs (void)
{
    const struct ra_lib *l;

    if (WindowBase)
	return TRUE;                          /* bereits geoeffnet */

    for (l = ra_libs; l->name; l++) {
	*l->base = OpenLibrary (l->name, 0);
	if (!*l->base) {
	    write_log ("ReAction GUI: Kann '%s' nicht oeffnen.\n", l->name);
	    ra_close_libs ();
	    return FALSE;
	}
	*l->iface = GetInterface (*l->base, "main", 1, NULL);
	if (!*l->iface) {
	    write_log ("ReAction GUI: Kein 'main'-Interface fuer '%s'.\n", l->name);
	    ra_close_libs ();
	    return FALSE;
	}
    }
    ra_locale_open ();
    return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Gadget-Zugriff                                                         */
/* ---------------------------------------------------------------------- */

ULONG ra_get_num (Object *obj, ULONG tag)
{
    ULONG val = 0;

    if (obj)
	GetAttr (tag, obj, &val);
    return val;
}

const char *ra_get_text (Object *obj)
{
    STRPTR str = NULL;

    if (obj)
	GetAttr (STRINGA_TextVal, obj, (ULONG *) &str);
    return str ? (const char *) str : "";
}

void ra_set_text (Object *obj, const char *text)
{
    if (obj)
	RA_SET (obj, STRINGA_TextVal, text ? text : "");
}

void ra_set_disabled (Object *obj, BOOL disabled)
{
    if (obj)
	RA_SET (obj, GA_Disabled, disabled);
}

int ra_index_of (const ULONG *table, int count, ULONG value, int fallback)
{
    int i;

    for (i = 0; i < count; i++)
	if (table[i] == value)
	    return i;
    return fallback;
}

/* ---------------------------------------------------------------------- */
/* Labels fuer Chooser / ClickTab                                         */
/* ---------------------------------------------------------------------- */

struct List *ra_chooser_labels (const char *const *names)
{
    struct List *list = AllocSysObjectTags (ASOT_LIST, TAG_END);

    if (!list)
	return NULL;

    for (; *names; names++) {
	struct Node *node = AllocChooserNode (CNA_Text, *names, TAG_END);
	if (node)
	    AddTail (list, node);
    }
    return list;
}

void ra_free_chooser_labels (struct List *list)
{
    struct Node *node;

    if (!list)
	return;
    while ((node = RemHead (list)))
	FreeChooserNode (node);
    FreeSysObject (ASOT_LIST, list);
}

struct List *ra_clicktab_labels (const char *const *names)
{
    struct List *list = AllocSysObjectTags (ASOT_LIST, TAG_END);
    int i;

    if (!list)
	return NULL;

    for (i = 0; names[i]; i++) {
	struct Node *node = AllocClickTabNode (TNA_Text, ra_tr (names[i]), TNA_Number, i, TAG_END);
	if (node)
	    AddTail (list, node);
    }
    return list;
}

void ra_free_clicktab_labels (struct List *list)
{
    if (!list)
	return;
    FreeClickTabList (list);
    FreeSysObject (ASOT_LIST, list);
}

/* ---------------------------------------------------------------------- */
/* Gadget-Fabriken                                                        */
/* ---------------------------------------------------------------------- */

/* Mindestbreite fuer Chooser: Breite des laengsten Eintrags in der Schrift
 * des Workbench-Bildschirms + Platz fuer Pfeil und Rand. Der Chooser meldet
 * dem Layout sonst nur die Breite des Pfeils (Text unsichtbar). */
static ULONG ra_text_px (const char *s)
{
    ULONG w = 0;
    struct Screen *scr = LockPubScreen (NULL);

    if (scr) {
	w = TextLength (&scr->RastPort, s, strlen (s));
	UnlockPubScreen (NULL, scr);
    } else
	w = strlen (s) * 8;
    return w;
}

ULONG ra_names_width (const char *const *names)
{
    ULONG max = 0;

    for (; *names; names++) {
	ULONG w = ra_text_px (ra_tr (*names));
	if (w > max)
	    max = w;
    }
    return max + 44;
}

/* Wie im Clavicembalo (dort funktionierend): PopUp-Chooser mit
 * CHOOSER_LabelArray (NULL-terminiertes Textarray) statt einer Liste aus
 * Chooser-Knoten. Die Knoten-Variante (DropDown + CHOOSER_Labels) lieferte
 * unter OS4.1 FE nur einen leeren Pfeil ohne Text. */
Object *ra_chooser (ULONG gid, const char *const *names)
{
    int n = 0;

    while (names[n])
	n++;

    names = ra_tr_array (names);

    return ChooserObject,
		GA_ID,              gid,
		GA_RelVerify,       TRUE,
		CHOOSER_PopUp,      TRUE,
		CHOOSER_LabelArray, names,
		CHOOSER_Selected,   (WORD) 0,
		CHOOSER_MaxLabels,  n,
	   ChooserEnd;
}

Object *ra_checkbox (ULONG gid, const char *text)
{
    return CheckBoxObject,
		GA_ID,        gid,
		GA_RelVerify, TRUE,
		GA_Text,      ra_tr (text),
		CHECKBOX_TextPlace, PLACETEXT_RIGHT,
	   CheckBoxEnd;
}

Object *ra_integer (ULONG gid, LONG min, LONG max, LONG val)
{
    return IntegerObject,
		GA_ID,           gid,
		GA_RelVerify,    TRUE,
		GA_TabCycle,     TRUE,
		INTEGER_Minimum, min,
		INTEGER_Maximum, max,
		INTEGER_Number,  val,
		INTEGER_MaxChars, 8,
		INTEGER_Arrows,  TRUE,
	   IntegerEnd;
}

Object *ra_string (ULONG gid, ULONG maxchars)
{
    return StringObject,
		GA_ID,            gid,
		GA_RelVerify,     TRUE,
		GA_TabCycle,      TRUE,
		STRINGA_MaxChars, maxchars,
	   StringEnd;
}

Object *ra_button (ULONG gid, const char *text)
{
    return ButtonObject,
		GA_ID,        gid,
		GA_Text,      ra_tr (text),
		GA_RelVerify, TRUE,
	   ButtonEnd;
}

Object *ra_label (const char *text)
{
    return LabelObject,
		LABEL_Text, ra_tr (text),
	   LabelEnd;
}

Object *ra_choice_gadget (ULONG gid, struct ra_choice *c)
{
    return ra_chooser (gid, c->names);
}

int ra_choice_index (Object *obj, const struct ra_choice *c)
{
    int sel = (int) ra_get_num (obj, CHOOSER_Selected);

    return (sel < 0 || sel >= c->count) ? 0 : sel;
}

LONG ra_choice_value (Object *obj, const struct ra_choice *c)
{
    return c->values[ra_choice_index (obj, c)];
}

void ra_choice_set (Object *obj, const struct ra_choice *c, LONG value, int fallback_index)
{
    int i, idx = fallback_index;

    for (i = 0; i < c->count; i++)
	if (c->values[i] == value) {
	    idx = i;
	    break;
	}
    if (obj)
	RA_SET (obj, CHOOSER_Selected, idx);
}

void ra_choice_free (struct ra_choice *c)
{
    ra_free_chooser_labels (c->labels);
    c->labels = NULL;
}

/* ---------------------------------------------------------------------- */
/* ASL-Dateirequester                                                     */
/* ---------------------------------------------------------------------- */

/* Fenster, ueber dem Requester erscheinen: ein offenes Dialogfenster
   (z. B. Hardfile bearbeiten), sonst das Hauptfenster */
struct Window *ra_req_window;
#define REQ_WINDOW (ra_req_window ? ra_req_window : ra_window)

BOOL ra_request_file (const char *title, const char *pattern, BOOL save,
                      BOOL drawers_only, char *path, ULONG pathlen)
{
    struct FileRequester *fr;
    char   drawer[MAX_DPATH];
    char   file[MAX_DPATH];
    BOOL   ok = FALSE;

    drawer[0] = file[0] = '\0';

    /* Startverzeichnis/-datei aus dem bisherigen Pfad ableiten */
    if (path && path[0]) {
	strncpy (drawer, path, sizeof drawer - 1);
	drawer[sizeof drawer - 1] = '\0';
	strncpy (file, FilePart (path), sizeof file - 1);
	file[sizeof file - 1] = '\0';
	*PathPart (drawer) = '\0';
	if (drawers_only) {
	    strncpy (drawer, path, sizeof drawer - 1);
	    file[0] = '\0';
	}
    }

    fr = AllocAslRequest (ASL_FileRequest, NULL);
    if (!fr) {
	write_log ("ReAction GUI: Kein ASL-Requester.\n");
	return FALSE;
    }

    if (AslRequestTags (fr,
			      ASLFR_TitleText,      ra_tr (title),
			      ASLFR_Window,         REQ_WINDOW,
			      ASLFR_InitialDrawer,  drawer,
			      ASLFR_InitialFile,    file,
			      ASLFR_InitialPattern, pattern ? pattern : "#?",
			      ASLFR_DoPatterns,     pattern != NULL,
			      ASLFR_DoSaveMode,     save,
			      ASLFR_DrawersOnly,    drawers_only,
			      ASLFR_RejectIcons,    TRUE,
			      TAG_DONE))
    {
	strncpy (path, fr->fr_Drawer, pathlen - 1);
	path[pathlen - 1] = '\0';
	if (!drawers_only && fr->fr_File && fr->fr_File[0])
	    ok = AddPart (path, fr->fr_File, pathlen) ? TRUE : FALSE;
	else
	    ok = path[0] != '\0';
    }

    FreeAslRequest (fr);
    return ok;
}

/* ---------------------------------------------------------------------- */
/* Meldungen                                                              */
/* ---------------------------------------------------------------------- */

void ra_message (const char *title, const char *msg)
{
    struct EasyStruct es;

    es.es_StructSize   = sizeof es;
    es.es_Flags        = 0;
    es.es_Title        = (STRPTR) title;
    es.es_TextFormat   = (STRPTR) "%s";
    es.es_GadgetFormat = (STRPTR) "OK";
    EasyRequest (REQ_WINDOW, &es, NULL, msg);
}

void ra_error (const char *fmt, ...)
{
    char msg[1024];
    va_list ap;

    va_start (ap, fmt);
    vsnprintf (msg, sizeof msg, ra_tr (fmt), ap);
    va_end (ap);
    write_log ("%s\n", msg);
    ra_message (ra_tr ("E-UAE Error"), msg);
}

/* Ja/Nein-Rueckfrage, liefert TRUE bei "Yes" */
BOOL ra_confirm (const char *fmt, ...)
{
    struct EasyStruct es;
    char msg[1024];
    va_list ap;

    va_start (ap, fmt);
    vsnprintf (msg, sizeof msg, ra_tr (fmt), ap);
    va_end (ap);

    es.es_StructSize   = sizeof es;
    es.es_Flags        = 0;
    es.es_Title        = (STRPTR) "E-UAE";
    es.es_TextFormat   = (STRPTR) "%s";
    es.es_GadgetFormat = (STRPTR) ra_tr ("Yes|No");
    return EasyRequest (REQ_WINDOW, &es, NULL, msg) == 1;
}

void ra_info (const char *fmt, ...)
{
    char msg[1024];
    va_list ap;

    va_start (ap, fmt);
    vsnprintf (msg, sizeof msg, ra_tr (fmt), ap);
    va_end (ap);
    ra_message ("E-UAE", msg);
}
