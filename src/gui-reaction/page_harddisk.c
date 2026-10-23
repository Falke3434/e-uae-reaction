/*
 * E-UAE - ReAction GUI: Seite "Hard disks"
 *
 *   Hauptseite: nur eine Liste aller Laufwerke (Geraet, Typ, Groesse,
 *   Controller, Pfad) und die Buttons "Verzeichnis hinzufuegen...",
 *   "Hardfile hinzufuegen...", "Bearbeiten..." (auch Doppelklick) und
 *   "Entfernen". Alles Weitere steht in zwei eigenen Fenstern:
 *
 *   Hardfile-Fenster
 *     - Pfad, Controller (UAE / IDE A600/A1200 (Gayle) / IDE A4000) mit
 *       Master/Slave, Nur lesen; Groesse und RDB-Erkennung als Info-Zeile
 *     - "Erweitert": Geraetename, Boot-Prioritaet, RDB-Modus, Geometrie
 *     - "Neues Image": leeres Hardfile anlegen wie das Werkzeug make_hdf
 *       (make_hdf.c: Geometrie, Datei per Seek bis zur Groesse, sonst mit
 *       Nullen fuellen); Idee aus WinUAE: Dateisystem waehlen, dessen
 *       DosType in die ersten 4 Bytes kommt (OFS/FFS: 0, PFS3, PDS3, SFS;
 *       RDB: Kennung "RDSK" -> Partitionierung mit HDToolBox)
 *
 *   Verzeichnis-Fenster
 *     - Pfad, Volume-Name (Vorgabe aus dem Pfad), Nur lesen
 *     - "Erweitert": Geraetename, Boot-Prioritaet
 *
 *   CD-ROM-Fenster (v16), ein Laufwerk
 *     - Controller: uaescsi.device (Einheit 0) oder ATAPI am IDE-Controller
 *       (A600/A1200 Gayle oder A4000, Master/Slave - wie beim Hardfile)
 *     - Image (ISO oder CUE/BIN, leer = Laufwerk ohne CD); das Image laesst
 *       sich auch waehrend der Emulation auf der Seite "Media" wechseln
 *       (cdimage0=..., cdimage0_controller=uaescsi|ide0|ide1)
 *     - "Booten von CD": eingebautes CD-Dateisystem, das Laufwerk erscheint
 *       als CD0: (bootfaehig, Boot-Prioritaet) - cdimage0_boot=,
 *       cdimage0_bootpri=
 *
 *   Geraetenamen (DH0, DH1, ...) werden automatisch vergeben.
 *
 *   Die IDE-Schnittstelle (changed_prefs.cs_ide, gayle.c) ist kein eigener
 *   Schalter mehr: sie folgt dem Controller der IDE-Laufwerke und wird
 *   abgeschaltet, wenn das letzte IDE-Laufwerk entfernt wird. Beide IDE-
 *   Laufwerke haengen an derselben Schnittstelle (Master und Slave).
 *
 *   Die Konfigurationsdateien bleiben unveraendert (hardfile2=..., ide=...,
 *   ide0_hardfile=..., ide1_hardfile=...). Bedienbar nur bei gestoppter
 *   Emulation.
 */

#include "reaction_gui.h"

#ifdef FILESYS

#define BASE GID_PAGE_BASE (PAGE_ID_HARDDISK)

/* Hauptseite */
enum {
    GID_LIST = BASE + 1,
    GID_ADD_DIR, GID_ADD_HF, GID_EDIT, GID_REMOVE,
    GID_ADD_CD
};

/* Gadget-IDs der Dialogfenster (eigene Fenster, eigener Nummernkreis) */
enum {
    DID_OK = 1, DID_CANCEL,
    DID_PATH, DID_PATH_SEL, DID_CTRL, DID_UNIT, DID_RO, DID_INFO,
    DID_DEV, DID_BOOT, DID_RDB, DID_SECS, DID_SURF, DID_RES, DID_BS,
    DID_NEW_SIZE, DID_NEW_FS, DID_NEW_CREATE,
    DID_VOL,
    DID_CDCTRL, DID_CDBOOT
};

/* Groesste Hardfile-Groesse in MB: hardfile_unix.c arbeitet mit off_t; ist
   das nur 32 Bit (newlib auf AmigaOS 4), sind 2 GB das Maximum (wie make_hdf). */
#define HDF_MAX_MB ((LONG) (sizeof (off_t) < sizeof (uae_u64) ? 2047 : 2097151))

/* Dateisystem eines neuen Hardfiles: DosType fuer die ersten 4 Bytes */
static const char *const newfs_names[] = { "OFS/FFS", "PFS3", "PDS3", "SFS",
					   "RDB (partition with HDToolBox)", NULL };
static const LONG newfs_values[] = { 0, 0x50465300, 0x50445300, 0x53465300, 0x5244534b };
RA_CHOICE (c_newfs, newfs_names, newfs_values);
#define NEWFS_RDB 0x5244534b

/* Controller eines Hardfiles; die Werte 1/2 sind zugleich cs_ide */
enum { HC_UAE = 0, HC_GAYLE = 1, HC_A4000 = 2 };
static const char *const hc_names[] = { "UAE (uaehf.device)", "IDE A600/A1200 (Gayle)", "IDE A4000", NULL };
static const LONG hc_values[] = { HC_UAE, HC_GAYLE, HC_A4000 };
RA_CHOICE (c_hc, hc_names, hc_values);

static const char *const unit_names[] = { "Master", "Slave", NULL };
static const LONG unit_values[] = { 0, 1 };
RA_CHOICE (c_unit, unit_names, unit_values);

/* Controller des CD-ROM-Laufwerks; 1/2 sind zugleich cs_ide (wie HC_*),
 * CDC_AKIKO (v17) = CD-Controller des CD32 (cs_cd32cd) */
#define CDC_AKIKO 3
static const char *const cdctrl_names[] = { "uaescsi.device", "IDE A600/A1200 (Gayle)", "IDE A4000", "CD32 (Akiko)", NULL };
static const LONG cdctrl_values[] = { 0, 1, 2, CDC_AKIKO };
RA_CHOICE (c_cdctrl, cdctrl_names, cdctrl_values);

static Object *g_list, *g_add_dir, *g_add_hf, *g_add_cd, *g_edit, *g_remove;

static struct List        *unit_list;
static struct ColumnInfo  *col_info;
static int                 selected = -1;    /* Zeile in der Liste */

/* Zeilen der Liste: Wert >= 0 = Nummer der UAE-Einheit (mountinfo),
   ROW_IDE0 / ROW_IDE1 = Hardfile am IDE-Master / -Slave */
#define ROW_IDE0 (-1)
#define ROW_IDE1 (-2)
#define ROW_NEW  (-3)                         /* Dialog: neue Einheit */
/* CD-ROM-Laufwerk n (uaescsi.device-Einheit n) */
#define ROW_CD(n)     (-10 - (n))
#define IS_CD_KEY(k)  ((k) <= ROW_CD (0) && (k) > ROW_CD (MAX_CD_DRIVES))
#define CD_UNIT(k)    (-10 - (k))

/* IDE-Position des CD-ROM-Laufwerks (0 = Master, 1 = Slave) oder -1 */
/* v17: das Laufwerk gibt es auch mit cd32cd=true ohne cdimage0= */
static int cd_present (void)
{
    return changed_prefs.cddrive[0] || changed_prefs.cs_cd32cd;
}

static int cd_ide_slot (void)
{
    if (!changed_prefs.cddrive[0] || changed_prefs.cs_cd32cd)
	return -1;
    if (changed_prefs.cd_controller == CD_CTRL_IDE0)
	return 0;
    if (changed_prefs.cd_controller == CD_CTRL_IDE1)
	return 1;
    return -1;
}
static int row_map[MAX_FILESYSTEM_UNITS + 2 + MAX_CD_DRIVES];
static int nrows;

#define IDE_KEY(slot) ((slot) == 0 ? ROW_IDE0 : ROW_IDE1)
#define KEY_SLOT(key) ((key) == ROW_IDE0 ? 0 : 1)

/* ---- Hilfsfunktionen -------------------------------------------------- */

static void copy_str (char *dst, size_t size, const char *src)
{
    strncpy (dst, src ? src : "", size - 1);
    dst[size - 1] = '\0';
}

/* Daten einer UAE-Einheit (get_filesys_unit liefert malloc-Strings) */
struct unit_data {
    char devname[32], volname[64], rootdir[MAX_DPATH];
    int  is_dir, ro, secs, surf, res, bs, boot;
};

static int get_unit (int nr, struct unit_data *u)
{
    char *devname = NULL, *volname = NULL, *rootdir = NULL, *fsdir = NULL;
    int cyls = 0, flags = 0;
    uae_u64 size = 0;
    const char *err;

    memset (u, 0, sizeof *u);
    err = get_filesys_unit (changed_prefs.mountinfo, nr, &devname, &volname, &rootdir, &u->ro,
			    &u->secs, &u->surf, &u->res, &cyls, &size, &u->bs, &u->boot, &fsdir, &flags);
    if (!err) {
	copy_str (u->devname, sizeof u->devname, devname);
	copy_str (u->volname, sizeof u->volname, volname);
	copy_str (u->rootdir, sizeof u->rootdir, rootdir);
	u->is_dir = volname != NULL;
    }
    free (devname);
    free (volname);
    free (rootdir);
    free (fsdir);
    return err == NULL;
}

static int file_exists (const char *path)
{
    struct stat st;

    return path && path[0] && stat (path, &st) == 0;
}

static uae_u64 file_size (const char *path)
{
    struct stat st;

    if (path && path[0] && stat (path, &st) == 0)
	return (uae_u64) st.st_size;
    return 0;
}

/* Enthaelt die Datei eine Rigid Disk Block (RDB)-Partitionstabelle? Der RDB
   steht in einem der ersten 16 Bloecke; bei Blockgroessen > 512 liegen die
   Bloecke auf Vielfachen von 512, deshalb wird in 512-Byte-Schritten gesucht. */
static int file_has_rdb (const char *path)
{
    FILE *f;
    unsigned char buf[512];
    int i, found = 0;

    f = fopen (path, "rb");
    if (!f)
	return 0;
    for (i = 0; i < 128 && !found; i++) {
	unsigned long n;

	if (fread (buf, 1, sizeof buf, f) != sizeof buf)
	    break;
	if (memcmp (buf, "RDSK", 4) != 0)
	    continue;
	n = ((unsigned long) buf[4] << 24) | ((unsigned long) buf[5] << 16)
	  | ((unsigned long) buf[6] << 8)  |  (unsigned long) buf[7];
	if (n >= 16 && n <= 128)             /* Laenge des Blocks in Langworten */
	    found = 1;
	else if (i == 0 && n == 0)           /* nur Kennung "RDSK" (neues, leeres */
	    found = 1;                       /* RDB-Hardfile, siehe unten)        */
    }
    fclose (f);
    return found;
}

/* Ersten freien Geraetenamen DH0..DH99 suchen (ausser Einheit 'except') */
static void free_devname (char *buf, size_t size, int except)
{
    int n, i, units = nr_units (changed_prefs.mountinfo);
    struct unit_data u;

    for (n = 0; n < 100; n++) {
	int used = 0;

	snprintf (buf, size, "DH%d", n);
	for (i = 0; i < units && !used; i++)
	    if (i != except && get_unit (i, &u))
		used = strcasecmp (u.devname, buf) == 0;
	if (!used)
	    return;
    }
}

/* Wird die Datei / das Verzeichnis schon von einem anderen Eintrag benutzt? */
static int path_in_use (const char *path, int except_key)
{
    int i, n = nr_units (changed_prefs.mountinfo);
    struct unit_data u;

    for (i = 0; i < 2; i++)
	if (IDE_KEY (i) != except_key && changed_prefs.ide_hardfile[i][0]
	    && strcasecmp (changed_prefs.ide_hardfile[i], path) == 0)
	    return 1;
    for (i = 0; i < n; i++)
	if (i != except_key && get_unit (i, &u) && strcasecmp (u.rootdir, path) == 0)
	    return 1;
    return 0;
}

/* Geometrie wie make_hdf: 32 Sektoren, 1 Kopf; bei mehr als 65535 Zylindern
   (Problem fuer FFS vor OS 3.9) erst Koepfe, dann Sektoren erhoehen. */
static uae_u64 hdf_geometry (uae_u64 bytes, int *bpt, int *surfaces)
{
    uae_u64 num_blocks = bytes / 512;
    uae_u32 cylinders;

    *bpt = 32;
    *surfaces = 1;
    cylinders = (uae_u32) (num_blocks / (*bpt * *surfaces));
    while (cylinders > 65535 && *surfaces < 255) {
	(*surfaces)++;
	cylinders = (uae_u32) (num_blocks / (*bpt * *surfaces));
    }
    while (cylinders > 65535 && *bpt < 255) {
	(*bpt)++;
	cylinders = (uae_u32) (num_blocks / (*bpt * *surfaces));
    }
    return (uae_u64) cylinders * *surfaces * *bpt * 512;   /* passende Dateigroesse */
}

/* Datei der Groesse size anlegen - wie create_hdf() in make_hdf.c: erst per
   Seek ans Ende und ein Byte schreiben, geht das nicht, mit Nullen fuellen.
   dostype != 0 kommt in die ersten 4 Bytes (Big Endian). */
static int create_hdf_file (const char *path, uae_u64 size, uae_u32 dostype)
{
    const size_t CHUNK_SIZE = 65536;
    FILE *f;
    void *buf;
    int ok = 0;

    f = fopen (path, "wb+");
    if (!f)
	return 0;

    if (fseeko (f, (off_t) (size - 1), SEEK_SET) == 0 && fputc (0, f) != EOF)
	ok = 1;
    else {
	uae_u64 left = size;

	fseeko (f, 0, SEEK_SET);
	buf = calloc (1, CHUNK_SIZE);
	if (buf) {
	    while (left >= CHUNK_SIZE && fwrite (buf, CHUNK_SIZE, 1, f) == 1)
		left -= CHUNK_SIZE;
	    if (left < CHUNK_SIZE && (left == 0 || fwrite (buf, (size_t) left, 1, f) == 1))
		ok = 1;
	    free (buf);
	}
    }

    if (ok && dostype) {
	unsigned char id[4];

	id[0] = dostype >> 24; id[1] = dostype >> 16; id[2] = dostype >> 8; id[3] = dostype;
	ok = fseeko (f, 0, SEEK_SET) == 0 && fwrite (id, 4, 1, f) == 1;
    }
    if (fclose (f) != 0)
	ok = 0;
    if (!ok)
	remove (path);                       /* halbes Image nicht liegen lassen */
    return ok;
}

/* v17: die IDE-Schnittstelle bleibt eingeschaltet, auch wenn das letzte
 * IDE-Laufwerk entfernt wird - wie echte Hardware (A600/A1200/A4000 haben
 * sie immer). Ein- und ausgeschaltet wird sie ueber die Modellvorgabe
 * (Hardware-Seite) oder beim Eintragen eines IDE-Laufwerks. */
static void ide_interface_check (void)
{
}

/* ---- Liste ------------------------------------------------------------ */

static void build_rows (void)
{
    int i, n = nr_units (changed_prefs.mountinfo);

    nrows = 0;
    for (i = 0; i < n; i++)
	row_map[nrows++] = i;
    if (changed_prefs.ide_hardfile[0][0])
	row_map[nrows++] = ROW_IDE0;
    if (changed_prefs.ide_hardfile[1][0])
	row_map[nrows++] = ROW_IDE1;
    for (i = 0; i < MAX_CD_DRIVES; i++)
	if (changed_prefs.cddrive[i] || (i == 0 && changed_prefs.cs_cd32cd))
	    row_map[nrows++] = ROW_CD (i);
}

/* Art des CD-Images fuer Liste und Info-Zeile */
static const char *cd_kind (const char *path)
{
    size_t l = strlen (path);
    return l > 4 && strcasecmp (path + l - 4, ".cue") == 0 ? "CUE sheet" : "ISO image";
}

static int find_row (int key)
{
    int i;

    for (i = 0; i < nrows; i++)
	if (row_map[i] == key)
	    return i;
    return -1;
}

#define NCOLS 5
static void row_text (int row, char col[NCOLS][MAX_DPATH])
{
    int key = row_map[row], i;

    for (i = 0; i < NCOLS; i++)
	col[i][0] = '\0';

    if (IS_CD_KEY (key)) {
	int n = CD_UNIT (key);
	const char *img = changed_prefs.cdimage[n];

	/* CD32: the extended ROM mounts CD0: itself */
	copy_str (col[0], MAX_DPATH, changed_prefs.cd_boot || changed_prefs.cs_cd32cd ? "CD0:" : "-");
	if (img[0]) {
	    snprintf (col[1], MAX_DPATH, "CD-ROM, %s", ra_tr (cd_kind (img)));
	    if (file_exists (img))
		snprintf (col[2], MAX_DPATH, "%lu MB", (unsigned long) (file_size (img) >> 20));
	} else
	    copy_str (col[1], MAX_DPATH, "CD-ROM");
	if (changed_prefs.cs_cd32cd)
	    copy_str (col[3], MAX_DPATH, "CD32 (Akiko)");
	else if (cd_ide_slot () >= 0)
	    snprintf (col[3], MAX_DPATH, "%s, %s",
		      changed_prefs.cs_ide == HC_A4000 ? "IDE A4000" : "IDE A600/A1200",
		      ra_tr (cd_ide_slot () == 0 ? "Master" : "Slave"));
	else
	    snprintf (col[3], MAX_DPATH, ra_tr ("uaescsi.device, unit %d"), n);
	copy_str (col[4], MAX_DPATH, img[0] ? img : ra_tr ("(no disc)"));
    } else if (key >= 0) {
	struct unit_data u;

	if (!get_unit (key, &u))
	    return;
	snprintf (col[0], MAX_DPATH, "%s:", u.devname[0] ? u.devname : "?");
	if (u.is_dir) {
	    snprintf (col[1], MAX_DPATH, "%s (%s)%s", ra_tr ("Directory"), u.volname,
		      u.ro ? ra_tr (", read only") : "");
	} else {
	    snprintf (col[1], MAX_DPATH, "%s%s%s", ra_tr ("Hardfile"),
		      is_hardfile (changed_prefs.mountinfo, key) == FILESYS_HARDFILE_RDB ? ", RDB" : "",
		      u.ro ? ra_tr (", read only") : "");
	    snprintf (col[2], MAX_DPATH, "%lu MB", (unsigned long) (file_size (u.rootdir) >> 20));
	}
	copy_str (col[3], MAX_DPATH, "UAE");
	copy_str (col[4], MAX_DPATH, u.rootdir);
    } else {
	int slot = KEY_SLOT (key);

	copy_str (col[0], MAX_DPATH, "-");
	snprintf (col[1], MAX_DPATH, "%s%s", ra_tr ("Hardfile"),
		  changed_prefs.ide_readonly[slot] ? ra_tr (", read only") : "");
	snprintf (col[2], MAX_DPATH, "%lu MB",
		  (unsigned long) (file_size (changed_prefs.ide_hardfile[slot]) >> 20));
	snprintf (col[3], MAX_DPATH, "%s, %s",
		  changed_prefs.cs_ide == HC_A4000 ? "IDE A4000" : "IDE A600/A1200",
		  ra_tr (slot == 0 ? "Master" : "Slave"));
	copy_str (col[4], MAX_DPATH, changed_prefs.ide_hardfile[slot]);
    }
}

static void update_buttons (void)
{
    BOOL stopped = ra_emu_stopped ();
    BOOL have    = selected >= 0 && selected < nrows;

    ra_set_disabled (g_add_dir, !stopped);
    ra_set_disabled (g_add_hf,  !stopped);
    ra_set_disabled (g_add_cd,  !stopped);
    ra_set_disabled (g_edit,    !stopped || !have);
    ra_set_disabled (g_remove,  !stopped || !have);
}

static void rebuild_list (void)
{
    static char col[NCOLS][MAX_DPATH];
    int row;

    build_rows ();
    if (!unit_list || !g_list)
	return;

    RA_SET (g_list, LISTBROWSER_Labels, (ULONG) ~0);          /* abkoppeln */
    FreeListBrowserList (unit_list);

    for (row = 0; row < nrows; row++) {
	struct Node *node;

	row_text (row, col);
	node = AllocListBrowserNode (NCOLS,
				     LBNA_Column, 0, LBNCA_CopyText, TRUE, LBNCA_Text, col[0],
				     LBNA_Column, 1, LBNCA_CopyText, TRUE, LBNCA_Text, col[1],
				     LBNA_Column, 2, LBNCA_CopyText, TRUE, LBNCA_Text, col[2],
				     LBNCA_Justification, LCJ_RIGHT,
				     LBNA_Column, 3, LBNCA_CopyText, TRUE, LBNCA_Text, col[3],
				     LBNA_Column, 4, LBNCA_CopyText, TRUE, LBNCA_Text, col[4],
				     TAG_DONE);
	if (node)
	    AddTail (unit_list, node);
    }

    if (selected >= nrows)
	selected = nrows - 1;

    RA_SET (g_list, LISTBROWSER_Labels,   (ULONG) unit_list,
		    LISTBROWSER_Selected, selected);
    update_buttons ();
}

static void select_key (int key)
{
    build_rows ();
    selected = find_row (key);
    rebuild_list ();
}

/* ---- Dialogfenster: gemeinsame Teile ---------------------------------- */

static struct Window *dlg_win;
static Object        *dlg_obj;

#define DLG_SET(obj, ...) \
    RefreshSetGadgetAttrs ((struct Gadget *)(obj), dlg_win, NULL, __VA_ARGS__, TAG_END)

static void dlg_disable (Object *o, BOOL d)        { if (o) DLG_SET (o, GA_Disabled, d); }
static void dlg_text (Object *o, const char *t)    { if (o) DLG_SET (o, STRINGA_TextVal, t ? t : ""); }
static void dlg_num (Object *o, LONG v)            { if (o) DLG_SET (o, INTEGER_Number, v); }
static void dlg_check (Object *o, BOOL v)          { if (o) DLG_SET (o, GA_Selected, v); }
static int  dlg_checked (Object *o)                { return ra_get_num (o, GA_Selected) ? 1 : 0; }

static void dlg_choice (Object *o, const struct ra_choice *c, LONG value)
{
    int i, idx = 0;

    for (i = 0; i < c->count; i++)
	if (c->values[i] == value)
	    idx = i;
    if (o)
	DLG_SET (o, CHOOSER_Selected, idx);
}

/* Tooltips fuer ein Dialogfenster */
#define DLG_MAX_HINTS 32
static struct HintInfo dlg_hints[DLG_MAX_HINTS + 1];

static struct HintInfo *build_dlg_hints (const struct ra_hint *h)
{
    int n = 0;

    for (; h && h->text && n < DLG_MAX_HINTS; h++, n++) {
	dlg_hints[n].hi_GadgetID = (WORD) h->gid;
	dlg_hints[n].hi_Code     = -1;
	dlg_hints[n].hi_Text     = (CONST_STRPTR) ra_tr (h->text);
	dlg_hints[n].hi_Flags    = 0;
    }
    dlg_hints[n].hi_GadgetID = -1;
    dlg_hints[n].hi_Code     = -1;
    dlg_hints[n].hi_Text     = NULL;
    dlg_hints[n].hi_Flags    = 0;
    return dlg_hints;
}

static Object *dlg_window (const char *title, const struct ra_hint *hints, Object *content)
{
    if (!content)
	return NULL;
    return WindowObject,
	WA_Title,          ra_tr (title),
	WA_DragBar,        TRUE,
	WA_CloseGadget,    TRUE,
	WA_DepthGadget,    TRUE,
	WA_SizeGadget,     TRUE,
	WA_Activate,       TRUE,
	WINDOW_Position,   WPOS_CENTERWINDOW,
	WINDOW_RefWindow,  ra_window,
	WINDOW_GadgetHelp, TRUE,
	WINDOW_HintInfo,   build_dlg_hints (hints),
	WINDOW_ParentGroup, VLayoutObject,
	    LAYOUT_SpaceOuter, TRUE,
	    LAYOUT_AddChild, content,
	    LAYOUT_AddChild, HLayoutObject,
		LAYOUT_EvenSize, TRUE,
		LAYOUT_AddChild, ra_button (DID_OK,     "OK"),
		LAYOUT_AddChild, SpaceObject, SpaceEnd,
		LAYOUT_AddChild, ra_button (DID_CANCEL, "Cancel"),
	    LayoutEnd,
	    CHILD_WeightedHeight, 0,
	LayoutEnd,
    WindowEnd;
}

/* Fenster modal ausfuehren. init() setzt die Gadgets nach dem Oeffnen,
   handle() bekommt jedes Gadget-Ereignis; Rueckgabe 1 = fertig (OK
   uebernommen), -1 = abbrechen, 0 = weiter. Liefert 1, wenn mit OK
   beendet. Das Hauptfenster ist so lange gesperrt. */
static int run_dialog (Object *obj, void (*init) (void), int (*handle) (ULONG gid))
{
    static struct Requester lock_req;
    ULONG sigmask = 0, result, code;
    int rc = 0, done = 0, locked;

    if (!obj)
	return 0;
    dlg_obj = obj;
    dlg_win = RA_OpenWindow (obj);
    if (!dlg_win) {
	DisposeObject (obj);
	dlg_obj = NULL;
	return 0;
    }
    /* Hauptfenster sperren: leerer Requester blockiert die Eingaben,
       dazu der Warte-Mauszeiger (klassisches Intuition, in jedem SDK) */
    locked = 0;
    if (ra_window) {
	InitRequester (&lock_req);
	locked = Request (&lock_req, ra_window) ? 1 : 0;
	SetWindowPointer (ra_window, WA_BusyPointer, TRUE, TAG_END);
    }
    ra_req_window = dlg_win;

    if (init)
	init ();

    GetAttr (WINDOW_SigMask, obj, &sigmask);
    while (!done) {
	Wait (sigmask);
	while ((result = RA_HandleInput (obj, &code)) != WMHI_LASTMSG) {
	    switch (result & WMHI_CLASSMASK) {
		case WMHI_CLOSEWINDOW:
		    rc = -1;
		    break;
		case WMHI_GADGETUP:
		    if ((result & WMHI_GADGETMASK) == DID_CANCEL)
			rc = -1;
		    else
			rc = handle (result & WMHI_GADGETMASK);
		    break;
		default:
		    break;
	    }
	    if (rc != 0) {
		done = 1;
		break;
	    }
	}
    }

    ra_req_window = NULL;
    RA_CloseWindow (obj);
    DisposeObject (obj);
    dlg_obj = NULL;
    dlg_win = NULL;
    if (ra_window) {
	if (locked)
	    EndRequest (&lock_req, ra_window);
	SetWindowPointer (ra_window, TAG_END);
    }
    return rc > 0;
}

/* ---- Hardfile-Fenster ------------------------------------------------- */

static int     hf_key;                               /* bearbeiteter Eintrag oder ROW_NEW */
static Object *h_path, *h_ctrl, *h_unit, *h_ro, *h_info;
static Object *h_dev, *h_boot, *h_rdb, *h_secs, *h_surf, *h_res, *h_bs;
static Object *h_new_size, *h_new_fs;

/* Felder je nach Controller und RDB-Modus sperren, Info-Zeile setzen */
static void hf_update (void)
{
    static char info[160];
    int uae = ra_choice_value (h_ctrl, &c_hc) == HC_UAE;
    int rdb = dlg_checked (h_rdb);
    const char *path = ra_get_text (h_path);
    uae_u64 size = file_size (path);

    dlg_disable (h_unit, uae);
    dlg_disable (h_dev,  !uae);
    dlg_disable (h_boot, !uae);
    dlg_disable (h_rdb,  !uae);
    dlg_disable (h_secs, !uae || rdb);
    dlg_disable (h_surf, !uae || rdb);
    dlg_disable (h_res,  !uae || rdb);
    dlg_disable (h_bs,   !uae || rdb);

    if (!path[0])
	copy_str (info, sizeof info, ra_tr ("No hardfile chosen."));
    else if (!file_exists (path))
	copy_str (info, sizeof info, ra_tr ("File not found."));
    else
	snprintf (info, sizeof info, "%lu MB, %s", (unsigned long) (size >> 20),
		  ra_tr (file_has_rdb (path) ? "partition table (RDB) found" : "no partition table (RDB)"));
    DLG_SET (h_info, GA_Text, info);
}

/* Neue Datei gewaehlt: RDB erkennen (dann Geometrie 0 / aus dem RDB) */
static void hf_path_changed (void)
{
    const char *path = ra_get_text (h_path);

    if (path[0] && ra_choice_value (h_ctrl, &c_hc) == HC_UAE)
	dlg_check (h_rdb, file_has_rdb (path) ? TRUE : FALSE);
    hf_update ();
}

static void hf_init (void)
{
    char dev[32];

    /* Vorgaben fuer ein neues Laufwerk */
    free_devname (dev, sizeof dev, -1);
    dlg_text (h_dev, dev);
    dlg_num (h_secs, 32);
    dlg_num (h_surf, 1);
    dlg_num (h_res,  2);
    dlg_num (h_bs,   512);
    dlg_num (h_boot, 0);
    dlg_choice (h_ctrl, &c_hc, HC_UAE);
    dlg_choice (h_unit, &c_unit, (changed_prefs.ide_hardfile[0][0] || cd_ide_slot () == 0) ? 1 : 0);

    if (hf_key >= 0) {                               /* UAE-Einheit */
	struct unit_data u;

	if (get_unit (hf_key, &u)) {
	    dlg_text  (h_path, u.rootdir);
	    dlg_text  (h_dev,  u.devname);
	    dlg_check (h_ro,   u.ro ? TRUE : FALSE);
	    dlg_num   (h_boot, u.boot);
	    dlg_check (h_rdb,  u.secs == 0 ? TRUE : FALSE);   /* Geometrie 0 = RDB */
	    if (u.secs > 0) {
		dlg_num (h_secs, u.secs);
		dlg_num (h_surf, u.surf);
		dlg_num (h_res,  u.res);
	    }
	    if (u.bs > 0)
		dlg_num (h_bs, u.bs);
	}
    } else if (hf_key == ROW_IDE0 || hf_key == ROW_IDE1) {
	int slot = KEY_SLOT (hf_key);

	dlg_text   (h_path, changed_prefs.ide_hardfile[slot]);
	dlg_check  (h_ro,   changed_prefs.ide_readonly[slot] ? TRUE : FALSE);
	dlg_choice (h_ctrl, &c_hc, changed_prefs.cs_ide == HC_A4000 ? HC_A4000 : HC_GAYLE);
	dlg_choice (h_unit, &c_unit, slot);
    }
    hf_update ();
}

/* "Erstellen...": leeres Hardfile anlegen und ins Fenster uebernehmen */
static void hf_create (void)
{
    char path[MAX_DPATH];
    const char *file;
    LONG mb = (LONG) ra_get_num (h_new_size, INTEGER_Number);
    uae_u32 dostype = (uae_u32) ra_choice_value (h_new_fs, &c_newfs);
    uae_u64 bytes;
    int bpt, surfaces, ok;
    BPTR lock;

    if (mb < 1 || mb > HDF_MAX_MB) {
	ra_error ("Please enter a size between 1 and %ld MB.", (long) HDF_MAX_MB);
	return;
    }

    copy_str (path, sizeof path, ra_get_text (h_path));
    if (!ra_request_file_in (RA_DIR_HARDFILE, "Create hardfile", "#?.hdf", TRUE, FALSE, path, sizeof path))
	return;
    file = (const char *) FilePart (path);
    if (!file[0])
	return;                                  /* nur ein Verzeichnis gewaehlt */
    if (!strchr (file, '.') && strlen (path) + 4 < sizeof path)
	strcat (path, ".hdf");

    if (path_in_use (path, hf_key)) {
	ra_error ("The hardfile '%s' is in use. Remove it from the list first.", path);
	return;
    }
    lock = Lock (path, SHARED_LOCK);
    if (lock) {
	UnLock (lock);
	if (!ra_confirm ("The file '%s' already exists.\nReplace it?", path))
	    return;
    }

    bytes = hdf_geometry (((uae_u64) mb) << 20, &bpt, &surfaces);

    SetWindowPointer (dlg_win, WA_BusyPointer, TRUE, TAG_END);
    ok = create_hdf_file (path, bytes, dostype);
    SetWindowPointer (dlg_win, TAG_END);

    if (!ok) {
	ra_error ("Could not create the hardfile '%s'.", path);
	return;
    }

    dlg_text  (h_path, path);
    dlg_check (h_rdb,  dostype == NEWFS_RDB ? TRUE : FALSE);
    dlg_num   (h_secs, bpt);
    dlg_num   (h_surf, surfaces);
    dlg_num   (h_res,  2);
    dlg_num   (h_bs,   512);
    hf_update ();
}

/* OK: Einstellungen uebernehmen. Rueckgabe 1 = Fenster schliessen. */
static int hf_commit (void)
{
    char path[MAX_DPATH], dev[32];
    int ctrl = ra_choice_value (h_ctrl, &c_hc);
    int ro   = dlg_checked (h_ro);
    int new_key;

    copy_str (path, sizeof path, ra_get_text (h_path));
    if (!path[0]) {
	ra_error ("Please choose a hardfile first.");
	return 0;
    }
    if (!file_exists (path)) {
	ra_error ("Could not add unit:\n%s", "Hardfile not found");
	return 0;
    }
    if (path_in_use (path, hf_key)) {
	ra_error ("The hardfile '%s' is in use. Remove it from the list first.", path);
	return 0;
    }

    if (ctrl == HC_UAE) {
	int rdb  = dlg_checked (h_rdb) || file_has_rdb (path);
	int secs = rdb ? 0 : ra_get_num (h_secs, INTEGER_Number);
	int surf = rdb ? 0 : ra_get_num (h_surf, INTEGER_Number);
	int res  = rdb ? 0 : ra_get_num (h_res,  INTEGER_Number);
	int bs   = ra_get_num (h_bs,   INTEGER_Number);
	int boot = ra_get_num (h_boot, INTEGER_Number);
	const char *err;

	copy_str (dev, sizeof dev, ra_get_text (h_dev));
	if (!dev[0])
	    free_devname (dev, sizeof dev, hf_key);

	if (hf_key >= 0) {
	    err = set_filesys_unit (changed_prefs.mountinfo, hf_key, dev, NULL, path, ro,
				    secs, surf, res, bs, boot, NULL, 0);
	    new_key = hf_key;
	} else {
	    err = add_filesys_unit (changed_prefs.mountinfo, dev, NULL, path, ro,
				    secs, surf, res, bs, boot, NULL, 0);
	    new_key = nr_units (changed_prefs.mountinfo) - 1;
	}
	if (err) {
	    ra_error ("Could not add unit:\n%s", err);
	    return 0;
	}
	if (hf_key == ROW_IDE0 || hf_key == ROW_IDE1) {      /* IDE -> UAE */
	    int old = KEY_SLOT (hf_key);
	    changed_prefs.ide_hardfile[old][0] = '\0';
	    changed_prefs.ide_readonly[old] = 0;
	    ide_interface_check ();
	}
    } else {
	int slot = ra_choice_value (h_unit, &c_unit);

	if ((changed_prefs.ide_hardfile[slot][0] && IDE_KEY (slot) != hf_key) || cd_ide_slot () == slot) {
	    ra_error (slot == 0 ? "The IDE master slot is already in use."
				: "The IDE slave slot is already in use.");
	    return 0;
	}
	if (hf_key == ROW_IDE0 || hf_key == ROW_IDE1) {      /* anderer Slot */
	    int old = KEY_SLOT (hf_key);
	    changed_prefs.ide_hardfile[old][0] = '\0';
	    changed_prefs.ide_readonly[old] = 0;
	}
	copy_str (changed_prefs.ide_hardfile[slot], sizeof changed_prefs.ide_hardfile[slot], path);
	changed_prefs.ide_readonly[slot] = ro;
	changed_prefs.cs_ide = ctrl;                          /* Gayle oder A4000 */
	if (hf_key >= 0)                                      /* UAE -> IDE */
	    kill_filesys_unit (changed_prefs.mountinfo, hf_key);
	new_key = IDE_KEY (slot);
    }

    select_key (new_key);
    return 1;
}

static int hf_handle (ULONG gid)
{
    switch (gid) {
	case DID_PATH_SEL: {
	    char path[MAX_DPATH];

	    copy_str (path, sizeof path, ra_get_text (h_path));
	    if (ra_request_file_in (RA_DIR_HARDFILE, "Select hardfile", "#?.(hdf|img)", FALSE, FALSE, path, sizeof path)) {
		dlg_text (h_path, path);
		hf_path_changed ();
	    }
	    break;
	}
	case DID_PATH:       hf_path_changed (); break;
	case DID_CTRL:
	case DID_RDB:        hf_update ();       break;
	case DID_NEW_CREATE: hf_create ();       break;
	case DID_OK:         return hf_commit ();
	default:             break;
    }
    return 0;
}

static const struct ra_hint hf_hints[] = {
    { DID_PATH, 1, "Host file that holds the hard disk image." },
    { DID_PATH_SEL, 1, "Choose the hardfile with a file requester." },
    { DID_CTRL, 1, "Controller the drive is attached to. UAE: UAE's own uaehf.device, no Kickstart driver needed. IDE: the emulated IDE interface of the A600/A1200 (Gayle) or A4000, served by the Kickstart's own scsi.device. Both IDE drives share one interface." },
    { DID_UNIT, 1, "Position of the drive on the IDE interface." },
    { DID_RO, 1, "Mount read-only. The Amiga cannot change anything on this drive." },
    { DID_INFO, 1, "Size of the hardfile and whether it contains a partition table (RDB)." },
    { DID_DEV, 1, "Amiga device name of the drive, for example DH0. Filled in automatically." },
    { DID_BOOT, 1, "Boot priority. The drive with the highest value is booted first." },
    { DID_RDB, 1, "The hardfile contains a partition table (RDB). The Amiga reads the partitions from it; geometry, block size and names come from the image. Detected automatically when you choose the file." },
    { DID_SECS, 1, "Sectors per track (hardfiles without RDB only)." },
    { DID_SURF, 1, "Number of heads (hardfiles without RDB only)." },
    { DID_RES, 1, "Reserved blocks at the start of the hardfile." },
    { DID_BS, 1, "Block size in bytes (normally 512)." },
    { DID_NEW_SIZE, 1, "Size of the new hardfile in MB." },
    { DID_NEW_FS, 1, "File system the new hardfile is meant for. Its DosType is written to the first 4 bytes, the Amiga still has to format it. RDB: empty disk for partitioning with HDToolBox (also for the IDE controller)." },
    { DID_NEW_CREATE, 1, "Create an empty hardfile (like make_hdf) and use it for this drive, with a matching geometry." },
    { DID_OK, 1, "Apply the settings." },
    { DID_CANCEL, 1, "Close the window without changes." },
    { 0, 0, NULL }
};

static void edit_hardfile (int key)
{
    Object *content;

    hf_key = key;
    content = VLayoutObject,
	LAYOUT_AddChild, VLayoutObject,
	    LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("Hardfile"), LAYOUT_SpaceOuter, TRUE,
	    LAYOUT_AddChild, HLayoutObject,
		LAYOUT_AddChild, h_path = ra_string (DID_PATH, MAX_DPATH - 1),
		LAYOUT_AddChild, ra_button (DID_PATH_SEL, "..."),
		CHILD_WeightedWidth, 0,
	    LayoutEnd,
	    CHILD_Label, ra_label ("Path:"),
	    LAYOUT_AddChild, h_info = ButtonObject,
		GA_ID, DID_INFO, GA_ReadOnly, TRUE, GA_Text, "",
	    ButtonEnd,
	    CHILD_Label, ra_label ("Info:"),
	    LAYOUT_AddChild, HLayoutObject,
		LAYOUT_AddChild, h_ctrl = ra_choice_gadget (DID_CTRL, &c_hc),
		CHILD_MinWidth, ra_names_width (c_hc.names),
		LAYOUT_AddChild, h_unit = ra_choice_gadget (DID_UNIT, &c_unit),
		CHILD_MinWidth, ra_names_width (c_unit.names), CHILD_WeightedWidth, 0,
	    LayoutEnd,
	    CHILD_Label, ra_label ("Controller:"),
	    LAYOUT_AddChild, h_ro = ra_checkbox (DID_RO, "Read only"),
	LayoutEnd,
	CHILD_WeightedHeight, 0,

	LAYOUT_AddChild, VLayoutObject,
	    LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("Advanced"), LAYOUT_SpaceOuter, TRUE,
	    LAYOUT_AddChild, HLayoutObject,
		LAYOUT_AddChild, h_dev  = ra_string (DID_DEV, 30), CHILD_Label, ra_label ("Device:"),
		LAYOUT_AddChild, h_boot = ra_integer (DID_BOOT, -128, 127, 0), CHILD_Label, ra_label ("Boot priority:"),
	    LayoutEnd,
	    LAYOUT_AddChild, h_rdb = ra_checkbox (DID_RDB, "Hardfile has RDB (partition table)"),
	    LAYOUT_AddChild, HLayoutObject,
		LAYOUT_AddChild, h_secs = ra_integer (DID_SECS, 1, 1024, 32),  CHILD_Label, ra_label ("Sectors:"),
		LAYOUT_AddChild, h_surf = ra_integer (DID_SURF, 1, 1024, 1),   CHILD_Label, ra_label ("Heads:"),
	    LayoutEnd,
	    LAYOUT_AddChild, HLayoutObject,
		LAYOUT_AddChild, h_res  = ra_integer (DID_RES, 0, 1024, 2),      CHILD_Label, ra_label ("Reserved:"),
		LAYOUT_AddChild, h_bs   = ra_integer (DID_BS, 512, 4096, 512),   CHILD_Label, ra_label ("Block size:"),
	    LayoutEnd,
	LayoutEnd,
	CHILD_WeightedHeight, 0,

	LAYOUT_AddChild, HLayoutObject,
	    LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("New image"), LAYOUT_SpaceOuter, TRUE,
	    LAYOUT_AddChild, h_new_size = ra_integer (DID_NEW_SIZE, 1, HDF_MAX_MB, 256),
	    CHILD_Label, ra_label ("Size (MB):"),
	    LAYOUT_AddChild, h_new_fs = ra_choice_gadget (DID_NEW_FS, &c_newfs),
	    CHILD_MinWidth, ra_names_width (c_newfs.names), CHILD_Label, ra_label ("File system:"),
	    LAYOUT_AddChild, ra_button (DID_NEW_CREATE, "Create..."),
	    CHILD_WeightedWidth, 0,
	LayoutEnd,
	CHILD_WeightedHeight, 0,
    LayoutEnd;

    run_dialog (dlg_window (key == ROW_NEW ? "Add hardfile" : "Edit hardfile", hf_hints, content),
		hf_init, hf_handle);
    h_path = h_ctrl = h_unit = h_ro = h_info = NULL;
    h_dev = h_boot = h_rdb = h_secs = h_surf = h_res = h_bs = NULL;
    h_new_size = h_new_fs = NULL;
}

/* ---- Verzeichnis-Fenster ---------------------------------------------- */

static int     dir_key;
static Object *d_path, *d_vol, *d_ro, *d_dev, *d_boot;

/* Volume-Name aus dem Pfad: letzter Verzeichnisname bzw. Name vor ':' */
static void volname_from_path (const char *path, char *vol, size_t size)
{
    char tmp[MAX_DPATH];
    size_t len;
    char *p;

    copy_str (tmp, sizeof tmp, path);
    len = strlen (tmp);
    while (len > 0 && tmp[len - 1] == '/')
	tmp[--len] = '\0';
    if (len > 0 && tmp[len - 1] == ':') {
	tmp[--len] = '\0';
	p = tmp;
    } else {
	p = strrchr (tmp, '/');
	if (!p)
	    p = strrchr (tmp, ':');
	p = p ? p + 1 : tmp;
    }
    copy_str (vol, size, p);
}

static void dir_init (void)
{
    char dev[32];

    free_devname (dev, sizeof dev, -1);
    dlg_text (d_dev, dev);
    dlg_num (d_boot, 0);
    if (dir_key >= 0) {
	struct unit_data u;

	if (get_unit (dir_key, &u)) {
	    dlg_text  (d_path, u.rootdir);
	    dlg_text  (d_vol,  u.volname);
	    dlg_text  (d_dev,  u.devname);
	    dlg_check (d_ro,   u.ro ? TRUE : FALSE);
	    dlg_num   (d_boot, u.boot);
	}
    }
}

static int dir_commit (void)
{
    char path[MAX_DPATH], vol[64], dev[32];
    const char *err;
    int key;

    copy_str (path, sizeof path, ra_get_text (d_path));
    if (!path[0]) {
	ra_error ("Please enter a path first.");
	return 0;
    }
    if (path_in_use (path, dir_key)) {
	ra_error ("The directory '%s' is already in the list.", path);
	return 0;
    }
    copy_str (vol, sizeof vol, ra_get_text (d_vol));
    if (!vol[0])
	volname_from_path (path, vol, sizeof vol);
    if (!vol[0]) {
	ra_error ("Please enter a volume name for the drive.");
	return 0;
    }
    copy_str (dev, sizeof dev, ra_get_text (d_dev));
    if (!dev[0])
	free_devname (dev, sizeof dev, dir_key);

    if (dir_key >= 0) {
	err = set_filesys_unit (changed_prefs.mountinfo, dir_key, dev, vol, path, dlg_checked (d_ro),
				0, 0, 0, 0, ra_get_num (d_boot, INTEGER_Number), NULL, 0);
	key = dir_key;
    } else {
	err = add_filesys_unit (changed_prefs.mountinfo, dev, vol, path, dlg_checked (d_ro),
				0, 0, 0, 0, ra_get_num (d_boot, INTEGER_Number), NULL, 0);
	key = nr_units (changed_prefs.mountinfo) - 1;
    }
    if (err) {
	ra_error ("Could not add unit:\n%s", err);
	return 0;
    }
    select_key (key);
    return 1;
}

static int dir_handle (ULONG gid)
{
    switch (gid) {
	case DID_PATH_SEL: {
	    char path[MAX_DPATH], vol[64];

	    copy_str (path, sizeof path, ra_get_text (d_path));
	    if (ra_request_file_in (RA_DIR_HARDFILE, "Select directory", NULL, FALSE, TRUE, path, sizeof path)) {
		dlg_text (d_path, path);
		if (!ra_get_text (d_vol)[0]) {               /* Volume-Name vorschlagen */
		    volname_from_path (path, vol, sizeof vol);
		    dlg_text (d_vol, vol);
		}
	    }
	    break;
	}
	case DID_OK: return dir_commit ();
	default:     break;
    }
    return 0;
}

static const struct ra_hint dir_hints[] = {
    { DID_PATH, 1, "Host directory that appears as an Amiga drive." },
    { DID_PATH_SEL, 1, "Choose the directory with a file requester." },
    { DID_VOL, 1, "Volume name shown on the Amiga side. Taken from the directory name if left empty." },
    { DID_RO, 1, "Mount read-only. The Amiga cannot change anything on this drive." },
    { DID_DEV, 1, "Amiga device name of the drive, for example DH0. Filled in automatically." },
    { DID_BOOT, 1, "Boot priority. The drive with the highest value is booted first." },
    { DID_OK, 1, "Apply the settings." },
    { DID_CANCEL, 1, "Close the window without changes." },
    { 0, 0, NULL }
};

static void edit_directory (int key)
{
    Object *content;

    dir_key = key;
    content = VLayoutObject,
	LAYOUT_AddChild, VLayoutObject,
	    LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("Directory"), LAYOUT_SpaceOuter, TRUE,
	    LAYOUT_AddChild, HLayoutObject,
		LAYOUT_AddChild, d_path = ra_string (DID_PATH, MAX_DPATH - 1),
		LAYOUT_AddChild, ra_button (DID_PATH_SEL, "..."),
		CHILD_WeightedWidth, 0,
	    LayoutEnd,
	    CHILD_Label, ra_label ("Path:"),
	    LAYOUT_AddChild, d_vol = ra_string (DID_VOL, 30), CHILD_Label, ra_label ("Volume name:"),
	    LAYOUT_AddChild, d_ro  = ra_checkbox (DID_RO, "Read only"),
	LayoutEnd,
	CHILD_WeightedHeight, 0,

	LAYOUT_AddChild, HLayoutObject,
	    LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("Advanced"), LAYOUT_SpaceOuter, TRUE,
	    LAYOUT_AddChild, d_dev  = ra_string (DID_DEV, 30), CHILD_Label, ra_label ("Device:"),
	    LAYOUT_AddChild, d_boot = ra_integer (DID_BOOT, -128, 127, 0), CHILD_Label, ra_label ("Boot priority:"),
	LayoutEnd,
	CHILD_WeightedHeight, 0,
    LayoutEnd;

    run_dialog (dlg_window (key == ROW_NEW ? "Add directory" : "Edit directory", dir_hints, content),
		dir_init, dir_handle);
    d_path = d_vol = d_ro = d_dev = d_boot = NULL;
}

/* ---- CD-ROM-Fenster (v16) -------------------------------------------- */

static int     cd_key;                               /* bearbeitetes Laufwerk oder ROW_NEW */
static Object *c_ctrl_g, *c_slot_g, *c_path, *c_info, *c_boot, *c_bootpri;

static void cd_update (void)
{
    static char info[160];
    const char *path = ra_get_text (c_path);

    LONG ctrl = ra_choice_value (c_ctrl_g, &c_cdctrl);

    dlg_disable (c_slot_g, ctrl == 0 || ctrl == CDC_AKIKO);
    /* CD32: the extended ROM boots the CD itself */
    dlg_disable (c_boot, ctrl == CDC_AKIKO);
    dlg_disable (c_bootpri, ctrl == CDC_AKIKO || !dlg_checked (c_boot));

    if (!path[0])
	copy_str (info, sizeof info, ra_tr ("No disc - the drive stays empty."));
    else if (!file_exists (path))
	copy_str (info, sizeof info, ra_tr ("File not found."));
    else
	snprintf (info, sizeof info, "%lu MB, %s", (unsigned long) (file_size (path) >> 20),
		  ra_tr (cd_kind (path)));
    DLG_SET (c_info, GA_Text, info);
}

static void cd_init (void)
{
    int slot = cd_ide_slot ();

    /* Vorgabe fuer IDE: freie Position */
    dlg_choice (c_slot_g, &c_unit, changed_prefs.ide_hardfile[1][0] ? 0 : 1);
    dlg_choice (c_ctrl_g, &c_cdctrl, 0);
    dlg_check (c_boot, FALSE);
    dlg_num (c_bootpri, -127);
    if (IS_CD_KEY (cd_key)) {
	dlg_check (c_boot, changed_prefs.cd_boot ? TRUE : FALSE);
	dlg_num (c_bootpri, changed_prefs.cd_bootpri);
	dlg_text (c_path, changed_prefs.cdimage[0]);
	if (slot >= 0) {
	    dlg_choice (c_ctrl_g, &c_cdctrl, changed_prefs.cs_ide == HC_A4000 ? HC_A4000 : HC_GAYLE);
	    dlg_choice (c_slot_g, &c_unit, slot);
	}
    }
    if (changed_prefs.cs_cd32cd)
	dlg_choice (c_ctrl_g, &c_cdctrl, CDC_AKIKO);
    cd_update ();
}

static int cd_commit (void)
{
    char path[MAX_DPATH];
    int ctrl = ra_choice_value (c_ctrl_g, &c_cdctrl);
    int slot = ra_choice_value (c_slot_g, &c_unit);

    copy_str (path, sizeof path, ra_get_text (c_path));
    if (path[0] && !file_exists (path)) {
	ra_error ("The CD image '%s' was not found.", path);
	return 0;
    }
    /* v17: the Akiko chip as a whole, like the switch on the Hardware page */
    changed_prefs.cs_cd32cd = changed_prefs.cs_cd32c2p = changed_prefs.cs_cd32nvram = ctrl == CDC_AKIKO;
    if (ctrl == CDC_AKIKO) {
	changed_prefs.cd_controller = CD_CTRL_UAESCSI;      /* unused with Akiko */
    } else if (ctrl != 0) {
	if (changed_prefs.ide_hardfile[slot][0]) {
	    ra_error (slot == 0 ? "The IDE master slot is already in use."
				: "The IDE slave slot is already in use.");
	    return 0;
	}
	changed_prefs.cs_ide = ctrl;                         /* Gayle oder A4000 */
	changed_prefs.cd_controller = slot == 0 ? CD_CTRL_IDE0 : CD_CTRL_IDE1;
    } else
	changed_prefs.cd_controller = CD_CTRL_UAESCSI;
    changed_prefs.cddrive[0] = 1;
    copy_str (changed_prefs.cdimage[0], sizeof changed_prefs.cdimage[0], path);
    if (ctrl != CDC_AKIKO) {
	changed_prefs.cd_boot = dlg_checked (c_boot);
	changed_prefs.cd_bootpri = ra_get_num (c_bootpri, INTEGER_Number);
    }
    ide_interface_check ();
    select_key (ROW_CD (0));
    ra_write_page (PAGE_ID_HARDWARE);                /* Akiko-Schalter dort */
    ra_write_page (PAGE_ID_MEDIA);                   /* Image-Zeile dort freigeben */
    return 1;
}

static int cd_handle (ULONG gid)
{
    switch (gid) {
	case DID_PATH_SEL: {
	    char path[MAX_DPATH];

	    copy_str (path, sizeof path, ra_get_text (c_path));
	    if (ra_request_file_in (RA_DIR_CDIMAGE, "Select CD image", "#?.(iso|cue)", FALSE, FALSE, path, sizeof path)) {
		dlg_text (c_path, path);
		cd_update ();
	    }
	    break;
	}
	case DID_PATH:
	case DID_CDBOOT:
	case DID_CDCTRL: cd_update ();     break;
	case DID_OK:   return cd_commit ();
	default:       break;
    }
    return 0;
}

static const struct ra_hint cd_hints[] = {
    { DID_CDCTRL, 1, "Where the CD-ROM drive is attached. uaescsi.device: UAE's own device, unit 0, works with every Kickstart; the CD file system needs a mount entry with Device = uaescsi.device, Unit = 0. IDE: an ATAPI drive at the emulated IDE interface of the A600/A1200 (Gayle) or A4000, like real hardware; needs a scsi.device with ATAPI support (Kickstart 3.1.4 / 3.2 or a patched driver). Both IDE drives share one interface. CD32 (Akiko): the drive of the Amiga CD32; needs the CD32 Kickstart and extended ROM, which boot the CD themselves (CD32 model preset on the Hardware page)." },
    { DID_UNIT, 1, "Position of the drive on the IDE interface." },
    { DID_PATH, 1, "CD image in the drive: an ISO file or a CUE sheet with its BIN files (also with audio tracks). Leave empty for a drive without a disc. The disc can be changed on the Media page, also while the emulation is running." },
    { DID_PATH_SEL, 1, "Choose the CD image with a file requester." },
    { DID_CDBOOT, 1, "E-UAE reads the ISO 9660 file system of the disc itself and mounts the drive as CD0: - it is in the boot menu and can be booted from with any Kickstart, no CD file system or mount entry needed on the Amiga side. A disc change while the emulation runs is seen by new file accesses, the volume name stays until the next reset." },
    { DID_BOOT, 1, "Boot priority of CD0:. The drive with the highest value is booted first; with -127 the CD is only booted when chosen in the boot menu (or when nothing else is bootable)." },
    { DID_INFO, 1, "Size and type of the CD image." },
    { DID_OK, 1, "Apply the settings." },
    { DID_CANCEL, 1, "Close the window without changes." },
    { 0, 0, NULL }
};

static void edit_cdrom (int key)
{
    Object *content;

    cd_key = key;
    content = VLayoutObject,
	LAYOUT_AddChild, VLayoutObject,
	    LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("CD-ROM drive"), LAYOUT_SpaceOuter, TRUE,
	    LAYOUT_AddChild, HLayoutObject,
		LAYOUT_AddChild, c_ctrl_g = ra_choice_gadget (DID_CDCTRL, &c_cdctrl),
		CHILD_MinWidth, ra_names_width (c_cdctrl.names),
		LAYOUT_AddChild, c_slot_g = ra_choice_gadget (DID_UNIT, &c_unit),
		CHILD_MinWidth, ra_names_width (c_unit.names), CHILD_WeightedWidth, 0,
	    LayoutEnd,
	    CHILD_Label, ra_label ("Controller:"),
	    LAYOUT_AddChild, HLayoutObject,
		LAYOUT_AddChild, c_path = ra_string (DID_PATH, MAX_DPATH - 1),
		LAYOUT_AddChild, ra_button (DID_PATH_SEL, "..."),
		CHILD_WeightedWidth, 0,
	    LayoutEnd,
	    CHILD_Label, ra_label ("Image:"),
	    LAYOUT_AddChild, c_info = ButtonObject,
		GA_ID, DID_INFO, GA_ReadOnly, TRUE, GA_Text, "",
	    ButtonEnd,
	    CHILD_Label, ra_label ("Info:"),
	LayoutEnd,
	CHILD_WeightedHeight, 0,

	LAYOUT_AddChild, HLayoutObject,
	    LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("Boot"), LAYOUT_SpaceOuter, TRUE,
	    LAYOUT_AddChild, c_boot = ra_checkbox (DID_CDBOOT, "Boot from CD (mount as CD0:)"),
	    LAYOUT_AddChild, c_bootpri = ra_integer (DID_BOOT, -128, 127, -127),
	    CHILD_Label, ra_label ("Boot priority:"),
	LayoutEnd,
	CHILD_WeightedHeight, 0,
    LayoutEnd;

    run_dialog (dlg_window (key == ROW_NEW ? "Add CD-ROM drive" : "Edit CD-ROM drive", cd_hints, content),
		cd_init, cd_handle);
    c_ctrl_g = c_slot_g = c_path = c_info = c_boot = c_bootpri = NULL;
}

/* ---- Hauptseite ------------------------------------------------------- */

static void edit_selected (void)
{
    struct unit_data u;
    int key;

    if (selected < 0 || selected >= nrows || !ra_emu_stopped ())
	return;
    key = row_map[selected];
    if (IS_CD_KEY (key))
	edit_cdrom (key);
    else if (key >= 0 && get_unit (key, &u) && u.is_dir)
	edit_directory (key);
    else
	edit_hardfile (key);
}

static void do_remove (void)
{
    int key;

    if (selected < 0 || selected >= nrows)
	return;
    key = row_map[selected];
    if (IS_CD_KEY (key)) {
	changed_prefs.cddrive[0] = 0;
	changed_prefs.cdimage[0][0] = '\0';
	changed_prefs.cd_controller = CD_CTRL_UAESCSI;
	/* v17: no drive, no Akiko (switch on the Hardware page) */
	changed_prefs.cs_cd32cd = changed_prefs.cs_cd32c2p = changed_prefs.cs_cd32nvram = 0;
	ide_interface_check ();
	ra_write_page (PAGE_ID_MEDIA);
	ra_write_page (PAGE_ID_HARDWARE);
    } else if (key >= 0)
	kill_filesys_unit (changed_prefs.mountinfo, key);
    else {
	int slot = KEY_SLOT (key);
	changed_prefs.ide_hardfile[slot][0] = '\0';
	changed_prefs.ide_readonly[slot] = 0;
	ide_interface_check ();
    }
    build_rows ();
    if (selected >= nrows)
	selected = nrows - 1;
    rebuild_list ();
}

static Object *create (void)
{
    unit_list = AllocSysObjectTags (ASOT_LIST, TAG_END);
    col_info  = AllocLBColumnInfo (NCOLS,
				   LBCIA_Column, 0, LBCIA_Title, ra_tr ("Device"),     LBCIA_Weight, 10,
				   LBCIA_Column, 1, LBCIA_Title, ra_tr ("Type"),       LBCIA_Weight, 24,
				   LBCIA_Column, 2, LBCIA_Title, ra_tr ("Size"),       LBCIA_Weight, 10,
				   LBCIA_Column, 3, LBCIA_Title, ra_tr ("Controller"), LBCIA_Weight, 22,
				   LBCIA_Column, 4, LBCIA_Title, ra_tr ("Path"),       LBCIA_Weight, 34,
				   TAG_DONE);
    if (!unit_list || !col_info)
	return NULL;

    return VLayoutObject,
	LAYOUT_SpaceOuter, TRUE,

	LAYOUT_AddChild, g_list = ListBrowserObject,
	    GA_ID,                    GID_LIST,
	    GA_RelVerify,             TRUE,
	    LISTBROWSER_ColumnInfo,   col_info,
	    LISTBROWSER_ColumnTitles, TRUE,
	    LISTBROWSER_Labels,       unit_list,
	    LISTBROWSER_ShowSelected, TRUE,
	    LISTBROWSER_Selected,     -1,
	ListBrowserEnd,

	LAYOUT_AddChild, HLayoutObject,
	    LAYOUT_EvenSize, TRUE,
	    LAYOUT_AddChild, g_add_dir = ra_button (GID_ADD_DIR, "Add directory..."),
	    LAYOUT_AddChild, g_add_hf  = ra_button (GID_ADD_HF,  "Add hardfile..."),
	    LAYOUT_AddChild, g_add_cd  = ra_button (GID_ADD_CD,  "Add CD-ROM drive..."),
	    LAYOUT_AddChild, g_edit    = ra_button (GID_EDIT,    "Edit..."),
	    LAYOUT_AddChild, g_remove  = ra_button (GID_REMOVE,  "Remove"),
	LayoutEnd,
	CHILD_WeightedHeight, 0,
    LayoutEnd;
}

static void read_prefs (void)
{
    /* Die Laufwerke werden direkt ueber die Buttons / Fenster veraendert. */
}

static void write_prefs (void)
{
    if (!g_list)
	return;
    rebuild_list ();
}

static int handle (ULONG gid, ULONG code)
{
    switch (gid) {
	case GID_LIST:
	    selected = (int) ra_get_num (g_list, LISTBROWSER_Selected);
	    if (selected >= nrows)
		selected = -1;
	    update_buttons ();
	    if (selected >= 0 && (ra_get_num (g_list, LISTBROWSER_RelEvent) & LBRE_DOUBLECLICK))
		edit_selected ();
	    break;
	case GID_ADD_DIR: edit_directory (ROW_NEW); break;
	case GID_ADD_HF:  edit_hardfile (ROW_NEW);  break;
	case GID_ADD_CD:	/* nur ein CD-ROM-Laufwerk: vorhandenes bearbeiten */
	    edit_cdrom (cd_present () ? ROW_CD (0) : ROW_NEW);
	    break;
	case GID_EDIT:    edit_selected ();         break;
	case GID_REMOVE:  do_remove ();             break;
	default:          return 0;
    }
    return 1;
}

static void state (int st)
{
    update_buttons ();
}

static void cleanup (void)
{
    if (unit_list) {
	FreeListBrowserList (unit_list);
	FreeSysObject (ASOT_LIST, unit_list);
	unit_list = NULL;
    }
    if (col_info) {
	FreeLBColumnInfo (col_info);
	col_info = NULL;
    }
    g_list = g_add_dir = g_add_hf = g_add_cd = g_edit = g_remove = NULL;
    ra_choice_free (&c_hc);
    ra_choice_free (&c_unit);
    ra_choice_free (&c_newfs);
    ra_choice_free (&c_cdctrl);
    selected = -1;
    nrows = 0;
}

#else /* !FILESYS */

static Object *create (void) { return VLayoutObject, LayoutEnd; }
static void   nop (void) { }
static int    handle (ULONG gid, ULONG code) { return 0; }
#define read_prefs  nop
#define write_prefs nop
#define state       NULL
#define cleanup     nop

#endif

/* Tooltips (mouse-over help). English text = key in Catalogs/E-UAE.cd. */
static const struct ra_hint hints[] = {
    { GID_LIST, 1, "Drives of the emulated Amiga. Double-click an entry to edit it." },
    { GID_ADD_DIR, 1, "Add a host directory as an Amiga drive." },
    { GID_ADD_HF, 1, "Add a hardfile (hard disk image), or create a new one." },
    { GID_ADD_CD, 1, "Add the CD-ROM drive for ISO and CUE/BIN images, at uaescsi.device or as ATAPI drive at the IDE interface. If there is one already, it is edited." },
    { GID_EDIT, 1, "Change the settings of the selected drive." },
    { GID_REMOVE, 1, "Remove the selected drive from the list. The file itself is not deleted." },
    { 0, 0, NULL }
};

const struct ra_page ra_page_harddisk = {
    "Hard disks", PAGE_ID_HARDDISK, create, read_prefs, write_prefs, handle, state, cleanup, hints
};
