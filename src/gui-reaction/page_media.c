/*
 * E-UAE - ReAction GUI: Seite "Media"
 *
 *   - Kickstart-ROM, Extended-ROM, Keyfile
 *   - Floppylaufwerke DF0..DF3 (Typ, Image, Neu, Auswerfen), Floppy-Geschwindigkeit
 *   - CD-ROM-Laufwerk (v16): Image wechseln / auswerfen, auch waehrend der
 *     Emulation; das Laufwerk selbst wird auf der Seite "Hard disks"
 *     angelegt (changed_prefs.cddrive[0]), blkdev-cdimage.c uebernimmt den
 *     Wechsel im naechsten Frame
 *
 * "New..." legt ein leeres Disk-Image an (disk_creatediskfile() in disk.c):
 * 3.5" DD/HD als formatierte OFS-Diskette (880 KB / 1760 KB), 5.25" als
 * unformatiertes Extended-ADF. Das Image wird danach in das Laufwerk gelegt.
 */

#include "reaction_gui.h"

#define BASE GID_PAGE_BASE (PAGE_ID_MEDIA)

enum {
    GID_ROM = BASE + 1, GID_ROM_SEL,
    GID_EXT,            GID_EXT_SEL,
    GID_KEY,            GID_KEY_SEL,
    GID_CART,           GID_CART_SEL,   /* v17: Action Replay / HRTMon */
    GID_DFTYPE  = BASE + 10,     /* +0..3 */
    GID_DFFILE  = BASE + 20,     /* +0..3 */
    GID_DFSEL   = BASE + 30,     /* +0..3 */
    GID_DFEJECT = BASE + 40,     /* +0..3 */
    GID_FLOPPYSPEED = BASE + 50,
    GID_DFNEW   = BASE + 60,     /* +0..3 */
    GID_CDFILE  = BASE + 70,     /* +0..3 */
    GID_CDSEL   = BASE + 75,     /* +0..3 */
    GID_CDEJECT = BASE + 80      /* +0..3 */
};

static const char *const type_names[] = { "Off", "3.5\" DD", "3.5\" HD", "5.25\" SD", NULL };
static const LONG        type_values[] = { -1, DRV_35_DD, DRV_35_HD, DRV_525_SD };
#define NUM_TYPES 4

static const char *const speed_names[] = { "Original (100%)", "Turbo", "200%", "400%", "800%", NULL };
static const LONG        speed_values[] = { 100, 0, 200, 400, 800 };
#define NUM_SPEEDS 5

static Object *g_rom, *g_ext, *g_key, *g_cart;
static Object *g_dftype[4], *g_dffile[4], *g_dfeject[4], *g_dfnew[4];
static Object *g_speed;
static Object *g_cdfile[MAX_CD_DRIVES], *g_cdsel[MAX_CD_DRIVES], *g_cdeject[MAX_CD_DRIVES];

/* ---------------------------------------------------------------------- */

static Object *file_row (ULONG gid_str, ULONG gid_sel, Object **str_out)
{
    return HLayoutObject,
		LAYOUT_AddChild, *str_out = ra_string (gid_str, 255),
		LAYOUT_AddChild, ra_button (gid_sel, "..."),
		CHILD_WeightedWidth, 0,
	   LayoutEnd;
}

static Object *drive_row (int i)
{
    return HLayoutObject,
		LAYOUT_AddChild, g_dftype[i] = ra_chooser (GID_DFTYPE + i, type_names), CHILD_MinWidth, ra_names_width (type_names),
		CHILD_WeightedWidth, 0,
		LAYOUT_AddChild, g_dffile[i] = ra_string (GID_DFFILE + i, 255),
		LAYOUT_AddChild, ra_button (GID_DFSEL + i, "..."),
		CHILD_WeightedWidth, 0,
		LAYOUT_AddChild, g_dfnew[i] = ra_button (GID_DFNEW + i, "New..."),
		CHILD_WeightedWidth, 0,
		LAYOUT_AddChild, g_dfeject[i] = ra_button (GID_DFEJECT + i, "Eject"),
		CHILD_WeightedWidth, 0,
	   LayoutEnd;
}

static Object *cd_row (int i)
{
    return HLayoutObject,
		LAYOUT_AddChild, g_cdfile[i] = ra_string (GID_CDFILE + i, 255),
		LAYOUT_AddChild, g_cdsel[i] = ra_button (GID_CDSEL + i, "..."),
		CHILD_WeightedWidth, 0,
		LAYOUT_AddChild, g_cdeject[i] = ra_button (GID_CDEJECT + i, "Eject"),
		CHILD_WeightedWidth, 0,
	   LayoutEnd;
}

/* v17: a CD32 (cd32cd=true) has its drive also without cdimage0= */
#define CD_PRESENT(i) (changed_prefs.cddrive[i] || ((i) == 0 && changed_prefs.cs_cd32cd))

static Object *create (void)
{
    return VLayoutObject,
	LAYOUT_AddChild, VLayoutObject,
	    LAYOUT_BevelStyle, BVS_GROUP,
	    LAYOUT_Label,      ra_tr ("Kickstart"),
	    LAYOUT_SpaceOuter, TRUE,
	    LAYOUT_AddChild, file_row (GID_ROM, GID_ROM_SEL, &g_rom),
	    CHILD_Label, ra_label ("ROM:"),
	    LAYOUT_AddChild, file_row (GID_EXT, GID_EXT_SEL, &g_ext),
	    CHILD_Label, ra_label ("Ext. ROM:"),
	    LAYOUT_AddChild, file_row (GID_KEY, GID_KEY_SEL, &g_key),
	    CHILD_Label, ra_label ("Keyfile:"),
	    LAYOUT_AddChild, file_row (GID_CART, GID_CART_SEL, &g_cart),
	    CHILD_Label, ra_label ("Cartridge:"),
	LayoutEnd,
	CHILD_WeightedHeight, 0,

	LAYOUT_AddChild, VLayoutObject,
	    LAYOUT_BevelStyle, BVS_GROUP,
	    LAYOUT_Label,      ra_tr ("Floppy drives"),
	    LAYOUT_SpaceOuter, TRUE,
	    LAYOUT_AddChild, drive_row (0), CHILD_Label, ra_label ("DF0:"),
	    LAYOUT_AddChild, drive_row (1), CHILD_Label, ra_label ("DF1:"),
	    LAYOUT_AddChild, drive_row (2), CHILD_Label, ra_label ("DF2:"),
	    LAYOUT_AddChild, drive_row (3), CHILD_Label, ra_label ("DF3:"),
	LayoutEnd,
	CHILD_WeightedHeight, 0,

	LAYOUT_AddChild, g_speed = ra_chooser (GID_FLOPPYSPEED, speed_names), CHILD_MinWidth, ra_names_width (speed_names),
	CHILD_Label, ra_label ("Floppy speed:"),
	CHILD_WeightedHeight, 0,

	LAYOUT_AddChild, VLayoutObject,
	    LAYOUT_BevelStyle, BVS_GROUP,
	    LAYOUT_Label,      ra_tr ("CD-ROM drive"),
	    LAYOUT_SpaceOuter, TRUE,
	    LAYOUT_AddChild, cd_row (0), CHILD_Label, ra_label ("CD:"),
	LayoutEnd,
	CHILD_WeightedHeight, 0,

	LAYOUT_AddChild, SpaceObject, SpaceEnd,
    LayoutEnd;
}

/* ---------------------------------------------------------------------- */

static void copy_path (char *dst, size_t size, const char *src)
{
    if (strcmp (dst, src) != 0) {
	strncpy (dst, src, size - 1);
	dst[size - 1] = '\0';
    }
}

static void read_prefs (void)
{
    int i, last = -1;

    copy_path (changed_prefs.romfile,    sizeof changed_prefs.romfile,    ra_get_text (g_rom));
    copy_path (changed_prefs.romextfile, sizeof changed_prefs.romextfile, ra_get_text (g_ext));
    copy_path (changed_prefs.keyfile,    sizeof changed_prefs.keyfile,    ra_get_text (g_key));
#ifdef ACTION_REPLAY
    copy_path (changed_prefs.cartfile,   sizeof changed_prefs.cartfile,   ra_get_text (g_cart));
#endif

    for (i = 0; i < 4; i++) {
	int sel = ra_get_num (g_dftype[i], CHOOSER_Selected);

	if (sel < 0 || sel >= NUM_TYPES)
	    sel = 0;
	changed_prefs.dfxtype[i] = type_values[sel];
	if (type_values[sel] >= 0)
	    last = i;
	copy_path (changed_prefs.df[i], sizeof changed_prefs.df[i], ra_get_text (g_dffile[i]));
    }
    changed_prefs.nr_floppies = last + 1;

    {
	int sel = ra_get_num (g_speed, CHOOSER_Selected);
	if (sel >= 0 && sel < NUM_SPEEDS)
	    changed_prefs.floppy_speed = speed_values[sel];
    }

    /* CD-ROM: nur fuer vorhandene Laufwerke (Seite "Hard disks") */
    for (i = 0; i < MAX_CD_DRIVES; i++)
	if (CD_PRESENT (i) && g_cdfile[i])
	    copy_path (changed_prefs.cdimage[i], sizeof changed_prefs.cdimage[i], ra_get_text (g_cdfile[i]));
}

static void write_prefs (void)
{
    int i;

    ra_set_text (g_rom, changed_prefs.romfile);
    ra_set_text (g_ext, changed_prefs.romextfile);
    ra_set_text (g_key, changed_prefs.keyfile);
#ifdef ACTION_REPLAY
    ra_set_text (g_cart, changed_prefs.cartfile);
#endif

    for (i = 0; i < 4; i++) {
	int idx = ra_index_of ((const ULONG *) type_values, NUM_TYPES,
			       (ULONG) changed_prefs.dfxtype[i], 0);
	RA_SET (g_dftype[i], CHOOSER_Selected, idx);
	ra_set_text (g_dffile[i], changed_prefs.df[i]);
	/* "New..." braucht einen eingeschalteten Laufwerkstyp (DD/HD/5.25") */
	ra_set_disabled (g_dfnew[i], type_values[idx] < 0);
    }

    RA_SET (g_speed, CHOOSER_Selected,
	    ra_index_of ((const ULONG *) speed_values, NUM_SPEEDS,
			 (ULONG) changed_prefs.floppy_speed, 0));

    for (i = 0; i < MAX_CD_DRIVES; i++) {
	BOOL off = !CD_PRESENT (i);
	ra_set_text (g_cdfile[i], off ? "" : changed_prefs.cdimage[i]);
	ra_set_disabled (g_cdfile[i], off);
	ra_set_disabled (g_cdsel[i], off);
	ra_set_disabled (g_cdeject[i], off);
    }
}

static void choose (int kind, Object *str, const char *title, const char *pattern, BOOL drawers)
{
    char path[MAX_DPATH];

    strncpy (path, ra_get_text (str), sizeof path - 1);
    path[sizeof path - 1] = '\0';
    if (ra_request_file_in (kind, title, pattern, FALSE, drawers, path, sizeof path))
	ra_set_text (str, path);
}

/* Leeres Disk-Image fuer Laufwerk n anlegen und einlegen */
static void create_disk (int n)
{
    char path[MAX_DPATH], volname[32];
    const char *file, *dot;
    int sel = ra_get_num (g_dftype[n], CHOOSER_Selected);
    LONG type;
    size_t len;
    BPTR lock;

    if (sel < 0 || sel >= NUM_TYPES || type_values[sel] < 0)
	return;                                 /* Laufwerk aus (Button ist gesperrt) */
    type = type_values[sel];

    /* Startverzeichnis: Pfad des eingelegten Images, ohne Dateinamen */
    strncpy (path, ra_get_text (g_dffile[n]), sizeof path - 1);
    path[sizeof path - 1] = '\0';
    path[(const char *) FilePart (path) - path] = '\0';   /* FilePart(): CONST_STRPTR */

    if (!ra_request_file_in (RA_DIR_FLOPPY, "Create blank disk image", "#?.adf", TRUE, FALSE, path, sizeof path))
	return;

    /* Ohne Endung ".adf" anhaengen */
    file = (const char *) FilePart (path);
    if (!file[0])
	return;                                 /* nur ein Verzeichnis gewaehlt */
    if (!strchr (file, '.') && strlen (path) + 4 < sizeof path)
	strcat (path, ".adf");

    /* Ein eingelegtes Image nicht ueberschreiben: die Emulation haelt die
       Datei offen (gilt auch fuer die Felder der anderen Laufwerke) */
    {
	int i;
	for (i = 0; i < 4; i++)
	    if (strcasecmp (path, currprefs.df[i]) == 0 || strcasecmp (path, changed_prefs.df[i]) == 0
		|| strcasecmp (path, ra_get_text (g_dffile[i])) == 0) {
		ra_error ("The disk image '%s' is inserted in a drive. Eject it first.", path);
		return;
	    }
    }

    /* Vorhandene Datei nur nach Rueckfrage ueberschreiben */
    lock = Lock (path, SHARED_LOCK);
    if (lock) {
	UnLock (lock);
	if (!ra_confirm ("The file '%s' already exists.\nReplace it?", path))
	    return;
    }

    /* Volume-Name = Dateiname ohne Endung (AmigaDOS: max. 30 Zeichen) */
    file = (const char *) FilePart (path);
    dot  = strrchr (file, '.');
    len  = dot ? (size_t) (dot - file) : strlen (file);
    if (len > 30)
	len = 30;
    memcpy (volname, file, len);
    volname[len] = '\0';

    if (!disk_creatediskfile (path, 0, (drive_type) type, volname)) {
	ra_error ("Could not create the disk image '%s'.", path);
	return;
    }
    ra_set_text (g_dffile[n], path);          /* einlegen: handle() -> read_prefs() */
}

static int handle (ULONG gid, ULONG code)
{
    int n;

    switch (gid) {
	case GID_ROM_SEL: choose (RA_DIR_ROM, g_rom, "Select Kickstart ROM",     NULL, FALSE); break;
	case GID_EXT_SEL: choose (RA_DIR_ROM, g_ext, "Select extended ROM",      NULL, FALSE); break;
	case GID_KEY_SEL: choose (RA_DIR_ROM, g_key, "Select ROM keyfile",       NULL, FALSE); break;
	case GID_CART_SEL: choose (RA_DIR_ROM, g_cart, "Select cartridge ROM",   NULL, FALSE); break;
	default:
	    if (gid >= GID_DFSEL && gid < GID_DFSEL + 4) {
		n = gid - GID_DFSEL;
		choose (RA_DIR_FLOPPY, g_dffile[n], "Select disk image", "#?.(adf|adz|dms|fdi|zip|gz|lha|lzh|lzx|dsk)", FALSE);
	    } else if (gid >= GID_DFEJECT && gid < GID_DFEJECT + 4) {
		n = gid - GID_DFEJECT;
		ra_set_text (g_dffile[n], "");
	    } else if (gid >= GID_CDSEL && gid < GID_CDSEL + MAX_CD_DRIVES) {
		n = gid - GID_CDSEL;
		choose (RA_DIR_CDIMAGE, g_cdfile[n], "Select CD image", "#?.(iso|cue)", FALSE);
	    } else if (gid >= GID_CDEJECT && gid < GID_CDEJECT + MAX_CD_DRIVES) {
		n = gid - GID_CDEJECT;
		ra_set_text (g_cdfile[n], "");
	    } else if (gid >= GID_CDFILE && gid < GID_CDFILE + MAX_CD_DRIVES) {
		;                                   /* Text -> read_prefs() */
	    } else if (gid >= GID_DFNEW && gid < GID_DFNEW + 4) {
		create_disk (gid - GID_DFNEW);
	    } else if (gid >= GID_DFTYPE && gid < GID_DFTYPE + 4) {
		int sel;
		n   = gid - GID_DFTYPE;
		sel = ra_get_num (g_dftype[n], CHOOSER_Selected);
		ra_set_disabled (g_dfnew[n], sel <= 0 || sel >= NUM_TYPES);
	    } else
		return 0;
	    break;
    }
    read_prefs ();          /* neue Texte in changed_prefs uebernehmen */
    return 1;
}

static void state (int st)
{
    /* ROMs und Laufwerkstypen nur bei gestoppter Emulation aenderbar. */
    BOOL lock = st != UAE_STATE_STOPPED;
    int i;

    ra_set_disabled (g_rom, lock);
    ra_set_disabled (g_ext, lock);
    ra_set_disabled (g_key, lock);
#ifdef ACTION_REPLAY
    ra_set_disabled (g_cart, lock);
#else
    ra_set_disabled (g_cart, TRUE);
#endif
    for (i = 0; i < 4; i++)
	ra_set_disabled (g_dftype[i], lock);
}

static void cleanup (void)
{
    int i;
    for (i = 0; i < MAX_CD_DRIVES; i++)
	g_cdfile[i] = g_cdsel[i] = g_cdeject[i] = NULL;
}

/* Tooltips (mouse-over help). English text = key in Catalogs/E-UAE.cd. */
static const struct ra_hint hints[] = {
    { GID_ROM, 1, "Kickstart ROM image used by the emulated Amiga." },
    { GID_ROM_SEL, 1, "Choose the Kickstart ROM file." },
    { GID_EXT, 1, "Optional extended ROM. The CD32 needs its 512 KB extended ROM here - or use the 1 MB CD32 ROM (Kickstart and extended ROM in one file) as Kickstart and leave this empty." },
    { GID_EXT_SEL, 1, "Choose the extended ROM file." },
    { GID_KEY, 1, "Keyfile for encrypted ROM images (Cloanto Amiga Forever)." },
    { GID_KEY_SEL, 1, "Choose the keyfile." },
    { GID_CART, 1, "Freezer cartridge ROM: Action Replay 1, 2 or 3 (a dump of the real cartridge) or HRTMon. Ctrl+Alt+F (Shortcuts page) freezes the running program and opens the cartridge's monitor: look at and change memory, trainers (infinite lives), rip graphics. Empty = no cartridge. Action Replay 2/3 are made for 68000 Amigas; HRTMon also works with 68020 and higher." },
    { GID_CART_SEL, 1, "Choose the cartridge ROM file." },
    { GID_DFTYPE, 4, "Type of this floppy drive. Off disables the drive." },
    { GID_DFFILE, 4, "Disk image inserted in this drive: ADF, ADZ, DMS or FDI, also packed in a ZIP, GZ, LHA or LZX archive (the first disk image in it; another one with archive.lha/name.adf)." },
    { GID_DFSEL, 4, "Choose a disk image for this drive." },
    { GID_DFNEW, 4, "Create a new blank disk image and insert it into this drive. 3.5\" DD and HD drives get a formatted (OFS) disk, 5.25\" drives an unformatted one." },
    { GID_DFEJECT, 4, "Eject the disk from this drive." },
    { GID_CDFILE, MAX_CD_DRIVES, "CD image (ISO or CUE sheet) in the CD-ROM drive. Can be changed while the emulation is running. The CD-ROM drive is added on the Hard disks page." },
    { GID_CDSEL, MAX_CD_DRIVES, "Choose a CD image for this drive." },
    { GID_CDEJECT, MAX_CD_DRIVES, "Eject the CD from this drive." },
    { GID_FLOPPYSPEED, 1, "Floppy drive speed. Turbo loads as fast as possible, but some software needs the original speed." },
    { 0, 0, NULL }
};

const struct ra_page ra_page_media = {
    "Media", PAGE_ID_MEDIA, create, read_prefs, write_prefs, handle, state, cleanup, hints
};
