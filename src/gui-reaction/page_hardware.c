/*
 * E-UAE - ReAction GUI: Seite "Hardware"
 *
 *   - CPU (Typ, Geschwindigkeit, kompatibel / zyklengenau, Idle)
 *   - Chipsatz (OCS/ECS/AGA, NTSC, Kollision, Blitter, v17: CD32-Akiko)
 *   - Speicher (Chip, Slow, Fast, Zorro III, Grafikkarte)
 *   - JIT-Compiler (nur wenn E-UAE mit JIT gebaut wurde)
 *
 * Alle Werte stehen nur bei gestoppter Emulation zur Verfuegung.
 */

#include "reaction_gui.h"

#define BASE GID_PAGE_BASE (PAGE_ID_HARDWARE)

enum {
    GID_CPU = BASE + 1, GID_SPEED, GID_COMPAT, GID_CYCLEEXACT, GID_IDLE,
    GID_CHIPSET, GID_NTSC, GID_COLLISION, GID_IMMBLITS, GID_BLITCYCLE,
    GID_CHIP, GID_SLOW, GID_FAST, GID_Z3, GID_GFX,
    GID_JITCACHE, GID_JITHARD, GID_JITCONST,
    GID_MODEL, GID_AKIKO
};

/* ---- Auswahllisten ---------------------------------------------------- */

static const char *const cpu_names[] = {
    "68000", "68010", "68EC020 (24 Bit)", "68020 (32 Bit)",
    "68EC020 + 68881 FPU (24 Bit)", "68020 + 68881 FPU (32 Bit)",
    "68040", "68060", NULL };
/* Wert = cpu_level * 2 + (32-Bit ? 1 : 0) = Index in der cpu_type-Tabelle
 * (cpumode[] in cfgfile.c): 4 = 68ec020, 5 = 68020, 6 = 68ec020/68881,
 * 7 = 68020/68881 (cpu_level 3 = 68020 mit FPU). 68000/68010 gibt es dort
 * nur mit 24 Bit (Index 0, 2), 68040/68060 nur mit 32 Bit (Index 9, 13). */
static const LONG cpu_values[] = { 0, 2, 4, 5, 6, 7, 9, 13 };
RA_CHOICE (c_cpu, cpu_names, cpu_values);

/* cpu_level/address_space_24 -> Wert in cpu_values. Kombinationen, die es
 * in der Liste nicht gibt (68000 mit 32 Bit, 68040 mit 24 Bit), werden auf
 * den passenden Eintrag abgebildet, statt still auf "68020" zu fallen. */
static LONG cpu_value_from_prefs (void)
{
    int level = changed_prefs.cpu_level;

    if (level < 0)
	level = 0;
    if (level <= 1)
	return level * 2;                          /* 68000, 68010: 24 Bit */
    if (level >= 4)
	return level >= 6 ? 13 : 9;                /* 68040, 68060: 32 Bit */
    return level * 2 + (changed_prefs.address_space_24 ? 0 : 1);
}

static const char *const speed_names[] = { "Maximum", "Original (Amiga speed)", NULL };
static const LONG speed_values[] = { -1, 0 };
RA_CHOICE (c_speed, speed_names, speed_values);

static const char *const chipset_names[] = { "OCS", "ECS Agnus", "ECS Denise", "ECS complete", "AGA", NULL };
static const LONG chipset_values[] = { 0, CSMASK_ECS_AGNUS, CSMASK_ECS_DENISE,
				       CSMASK_ECS_AGNUS | CSMASK_ECS_DENISE,
				       CSMASK_AGA | CSMASK_ECS_AGNUS | CSMASK_ECS_DENISE };
RA_CHOICE (c_chipset, chipset_names, chipset_values);

static const char *const coll_names[] = { "None", "Sprites only", "Sprites + playfields", "Full", NULL };
static const LONG coll_values[] = { 0, 1, 2, 3 };
RA_CHOICE (c_coll, coll_names, coll_values);

static const char *const chip_names[] = { "256 KB", "512 KB", "1 MB", "2 MB", "4 MB", "8 MB", NULL };
static const LONG chip_values[] = { 0x40000, 0x80000, 0x100000, 0x200000, 0x400000, 0x800000 };
RA_CHOICE (c_chip, chip_names, chip_values);

/* Nur Groessen, die fix_options() in main.c annimmt (0, 512 KB, 1 MB, 1.8 MB);
 * 256 KB wurde gespeichert, beim Start aber still auf 0 gesetzt. */
static const char *const slow_names[] = { "No RAM", "512 KB", "1 MB", "1.8 MB", NULL };
static const LONG slow_values[] = { 0, 0x80000, 0x100000, 0x1C0000 };
RA_CHOICE (c_slow, slow_names, slow_values);

static const char *const fast_names[] = { "No RAM", "1 MB", "2 MB", "4 MB", "8 MB", NULL };
static const LONG fast_values[] = { 0, 0x100000, 0x200000, 0x400000, 0x800000 };
RA_CHOICE (c_fast, fast_names, fast_values);

static const char *const z3_names[] = { "No RAM", "1 MB", "2 MB", "4 MB", "8 MB", "16 MB", "32 MB",
					"64 MB", "128 MB", "256 MB", "512 MB", NULL };
static const LONG z3_values[] = { 0, 0x100000, 0x200000, 0x400000, 0x800000, 0x1000000, 0x2000000,
				  0x4000000, 0x8000000, 0x10000000, 0x20000000 };
RA_CHOICE (c_z3, z3_names, z3_values);

/* Bis 128 MB (v16; fix_options() in main.c, Anmeldung als Zorro-III-Karte
 * in expansion.c). */
static const char *const gfx_names[] = { "None", "1 MB", "2 MB", "4 MB", "8 MB", "16 MB", "32 MB",
					 "64 MB", "128 MB", NULL };
static const LONG gfx_values[] = { 0, 0x100000, 0x200000, 0x400000, 0x800000, 0x1000000, 0x2000000,
				   0x4000000, 0x8000000 };
RA_CHOICE (c_gfx, gfx_names, gfx_values);

/* JIT-Cache in KB: nur sinnvolle Groessen (0 = aus, Maximum laut main.c: 16384) */
static const char *const jit_names[] = { "Off", "1 MB", "2 MB", "4 MB", "8 MB", "16 MB", NULL };
static const LONG jit_values[] = { 0, 1024, 2048, 4096, 8192, 16384 };
RA_CHOICE (c_jit, jit_names, jit_values);

/* Amiga-Modelle: stellen CPU, Chipsatz und Speicher passend ein.
 * (Kickstart-ROM und Laufwerke bleiben unveraendert - die waehlt man auf "Media";
 * v17: das CD32-Modell schaltet zusaetzlich den Akiko-Chip mit CD-Laufwerk ein.) */
struct ra_model { LONG cpu, chipset, chip, slow, fast, akiko, ide; };

static const char *const model_names[] = {
    "Custom settings", "A500 (OCS, 1 MB)", "A500+ / A600 (ECS, 1 MB)",
    "A1200 (AGA, 2 MB)", "A1200 + turbo card", "A4000 (68040, AGA)", "CD32 (AGA, 2 MB, Akiko)", NULL };
static const LONG model_values[] = { 0, 1, 2, 3, 4, 5, 6 };
RA_CHOICE (c_model, model_names, model_values);

#define CS_ECS (CSMASK_ECS_AGNUS | CSMASK_ECS_DENISE)
#define CS_AGA (CSMASK_AGA | CSMASK_ECS_AGNUS | CSMASK_ECS_DENISE)
/* cpu: Wert wie in cpu_values; Index 0 = "Custom settings" (unbenutzt) */
static const struct ra_model models[] = {
    /* v17: ide = IDE-Schnittstelle des Modells (1 = Gayle, 2 = A4000), wie
     * echte Hardware auch ohne IDE-Laufwerk; A500+/A600 teilen sich eine
     * Vorgabe - Gayle stoert eine A500+ nicht (Kickstart 2.04 ohne
     * scsi.device) */
    { 0, 0, 0, 0, 0, 0, 0 },
    { 0,  0,       0x80000,  0x80000, 0,        0, 0 },   /* A500:   68000, OCS, 512 KB Chip + 512 KB Slow */
    { 0,  CS_ECS,  0x100000, 0,       0,        0, 1 },   /* A500+/A600: 68000, ECS, 1 MB Chip            */
    { 4,  CS_AGA,  0x200000, 0,       0,        0, 1 },   /* A1200:  68EC020 (24 Bit), AGA, 2 MB Chip     */
    { 5,  CS_AGA,  0x200000, 0,       0x800000, 0, 1 },   /* A1200 + Turbokarte: 68020 (32 Bit), 8 MB Fast */
    { 9,  CS_AGA,  0x200000, 0,       0x800000, 0, 2 },   /* A4000:  68040, AGA, 2 MB Chip + 8 MB Fast     */
    { 4,  CS_AGA,  0x200000, 0,       0,        1, 0 },   /* CD32:   68EC020, AGA, 2 MB Chip, Akiko (v17)  */
};

static struct ra_choice *const all_choices[] = {
    &c_cpu, &c_speed, &c_chipset, &c_coll, &c_chip, &c_slow, &c_fast, &c_z3, &c_gfx,
    &c_jit, &c_model
};
#define NUM_CHOICES (int)(sizeof all_choices / sizeof all_choices[0])

/* ---- Gadgets ---------------------------------------------------------- */

static Object *g_cpu, *g_speed, *g_compat, *g_cycle, *g_idle;
static Object *g_chipset, *g_ntsc, *g_coll, *g_imm, *g_blitcycle, *g_akiko;
static Object *g_chip, *g_slow, *g_fast, *g_z3, *g_gfx;
static Object *g_model;
#ifdef JIT
static Object *g_jitcache, *g_jithard, *g_jitconst;
#endif

static Object *create (void)
{
    return HLayoutObject,
	LAYOUT_SpaceOuter, TRUE,

	/* ---- linke Spalte: Modell + CPU + Chipsatz ---- */
	LAYOUT_AddChild, VLayoutObject,
	    LAYOUT_AddChild, VLayoutObject,
		LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("Amiga model"), LAYOUT_SpaceOuter, TRUE,
		LAYOUT_AddChild, g_model = ra_choice_gadget (GID_MODEL, &c_model), CHILD_MinWidth, ra_names_width (c_model.names), CHILD_Label, ra_label ("Preset:"),
	    LayoutEnd,
	    CHILD_WeightedHeight, 0,

	    LAYOUT_AddChild, VLayoutObject,
		LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("CPU"), LAYOUT_SpaceOuter, TRUE,
		LAYOUT_AddChild, g_cpu   = ra_choice_gadget (GID_CPU, &c_cpu), CHILD_MinWidth, ra_names_width (c_cpu.names),   CHILD_Label, ra_label ("Type:"),
		LAYOUT_AddChild, g_speed = ra_choice_gadget (GID_SPEED, &c_speed), CHILD_MinWidth, ra_names_width (c_speed.names), CHILD_Label, ra_label ("Speed:"),
		LAYOUT_AddChild, g_idle  = ra_integer (GID_IDLE, 0, 120, 0),   CHILD_Label, ra_label ("Idle (0-120):"),
		LAYOUT_AddChild, g_compat = ra_checkbox (GID_COMPAT, "Compatible CPU mode"),
		LAYOUT_AddChild, g_cycle  = ra_checkbox (GID_CYCLEEXACT, "Cycle-exact CPU"),
	    LayoutEnd,
	    CHILD_WeightedHeight, 0,

	    LAYOUT_AddChild, VLayoutObject,
		LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("Chipset"), LAYOUT_SpaceOuter, TRUE,
		LAYOUT_AddChild, g_chipset = ra_choice_gadget (GID_CHIPSET, &c_chipset), CHILD_MinWidth, ra_names_width (c_chipset.names), CHILD_Label, ra_label ("Chipset:"),
		LAYOUT_AddChild, g_coll    = ra_choice_gadget (GID_COLLISION, &c_coll), CHILD_MinWidth, ra_names_width (c_coll.names),  CHILD_Label, ra_label ("Collision:"),
		LAYOUT_AddChild, g_ntsc      = ra_checkbox (GID_NTSC, "NTSC"),
		LAYOUT_AddChild, g_imm       = ra_checkbox (GID_IMMBLITS, "Run blitter immediately"),
		LAYOUT_AddChild, g_blitcycle = ra_checkbox (GID_BLITCYCLE, "Cycle-exact blitter"),
		LAYOUT_AddChild, g_akiko     = ra_checkbox (GID_AKIKO, "CD32 Akiko chip (CD drive, C2P, NVRAM)"),
	    LayoutEnd,
	    CHILD_WeightedHeight, 0,
	    LAYOUT_AddChild, SpaceObject, SpaceEnd,
	LayoutEnd,

	/* ---- rechte Spalte: Speicher + JIT ---- */
	LAYOUT_AddChild, VLayoutObject,
	    LAYOUT_AddChild, VLayoutObject,
		LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("Memory"), LAYOUT_SpaceOuter, TRUE,
		LAYOUT_AddChild, g_chip = ra_choice_gadget (GID_CHIP, &c_chip), CHILD_MinWidth, ra_names_width (c_chip.names), CHILD_Label, ra_label ("Chip RAM:"),
		LAYOUT_AddChild, g_slow = ra_choice_gadget (GID_SLOW, &c_slow), CHILD_MinWidth, ra_names_width (c_slow.names), CHILD_Label, ra_label ("Slow RAM:"),
		LAYOUT_AddChild, g_fast = ra_choice_gadget (GID_FAST, &c_fast), CHILD_MinWidth, ra_names_width (c_fast.names), CHILD_Label, ra_label ("Fast RAM:"),
		LAYOUT_AddChild, g_z3   = ra_choice_gadget (GID_Z3, &c_z3), CHILD_MinWidth, ra_names_width (c_z3.names),   CHILD_Label, ra_label ("Zorro III RAM:"),
		LAYOUT_AddChild, g_gfx  = ra_choice_gadget (GID_GFX, &c_gfx), CHILD_MinWidth, ra_names_width (c_gfx.names),  CHILD_Label, ra_label ("Graphics card (P96):"),
	    LayoutEnd,
	    CHILD_WeightedHeight, 0,
#ifdef JIT
	    LAYOUT_AddChild, VLayoutObject,
		LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("JIT compiler"), LAYOUT_SpaceOuter, TRUE,
		LAYOUT_AddChild, g_jitcache = ra_choice_gadget (GID_JITCACHE, &c_jit), CHILD_MinWidth, ra_names_width (c_jit.names), CHILD_Label, ra_label ("Cache:"),
		LAYOUT_AddChild, g_jithard  = ra_checkbox (GID_JITHARD,  "Hard cache flush"),
		LAYOUT_AddChild, g_jitconst = ra_checkbox (GID_JITCONST, "Constant jumps"),
	    LayoutEnd,
	    CHILD_WeightedHeight, 0,
#endif
	    LAYOUT_AddChild, SpaceObject, SpaceEnd,
	LayoutEnd,
    LayoutEnd;
}

/* ---- Gadgets <-> Prefs ------------------------------------------------ */

static int is_checked (Object *o)
{
    return ra_get_num (o, GA_Selected) ? 1 : 0;
}

/* Laeuft der JIT mit diesen Einstellungen? Gleiche Bedingung wie die
 * Auswahl der CPU-Schleife in newcpu.c (m68k_run_2a). */
static int jit_active (void)
{
#ifdef JIT
    return changed_prefs.cpu_level >= 2 && changed_prefs.cachesize > 0;
#else
    return 0;
#endif
}

/* v17: the Akiko chip as a whole (WinUAE: cd32cd, cd32c2p, cd32nvram) */
static int akiko_on (void)
{
    return changed_prefs.cs_cd32cd && changed_prefs.cs_cd32c2p;
}

static void set_akiko (int on)
{
    if (on == akiko_on ())
	return;
    changed_prefs.cs_cd32cd = changed_prefs.cs_cd32c2p = changed_prefs.cs_cd32nvram = on;
    if (on)
	changed_prefs.cddrive[0] = 1;       /* the CD32's drive (Hard disks / Media pages) */
}

static void read_prefs (void)
{
    LONG cpu = ra_choice_value (g_cpu, &c_cpu);

    changed_prefs.cpu_level        = cpu >> 1;
    changed_prefs.address_space_24 = (cpu < 8 && !(cpu & 1)) ? 1 : 0;   /* wie cfgfile.c */
    changed_prefs.m68k_speed       = ra_choice_value (g_speed, &c_speed);
    changed_prefs.cpu_idle         = ra_get_num (g_idle, INTEGER_Number);
    changed_prefs.cpu_compatible   = is_checked (g_compat);
    changed_prefs.cpu_cycle_exact  = is_checked (g_cycle);

    changed_prefs.chipset_mask     = ra_choice_value (g_chipset, &c_chipset);
    changed_prefs.collision_level  = ra_choice_value (g_coll, &c_coll);
    changed_prefs.ntscmode         = is_checked (g_ntsc);
    changed_prefs.immediate_blits  = is_checked (g_imm);
    changed_prefs.blitter_cycle_exact = is_checked (g_blitcycle);
    set_akiko (is_checked (g_akiko));

    changed_prefs.chipmem_size     = ra_choice_value (g_chip, &c_chip);
    changed_prefs.bogomem_size     = ra_choice_value (g_slow, &c_slow);
    changed_prefs.fastmem_size     = ra_choice_value (g_fast, &c_fast);
    changed_prefs.z3fastmem_size   = ra_choice_value (g_z3, &c_z3);
    changed_prefs.gfxmem_size      = ra_choice_value (g_gfx, &c_gfx);

#ifdef JIT
    changed_prefs.cachesize        = ra_choice_value (g_jitcache, &c_jit);
    changed_prefs.comp_hardflush   = is_checked (g_jithard);
    changed_prefs.comp_constjump   = is_checked (g_jitconst);
#endif
}

/*
 * Das Modell ist nur eine Vorlage und steht nicht in der Konfiguration.
 * Nach dem Laden (oder Start/Stop) wird es aus den Werten wiedererkannt:
 * stimmen CPU, Chipsatz und Speicher genau mit einem Modell ueberein, zeigt
 * die Auswahl dieses Modell, sonst "Custom settings".
 */
static int model_from_prefs (void)
{
    LONG cpu = cpu_value_from_prefs ();
    int i;

    if (changed_prefs.z3fastmem_size || changed_prefs.gfxmem_size)
	return 0;
    for (i = 1; i < (int)(sizeof models / sizeof models[0]); i++) {
	const struct ra_model *m = &models[i];
	if (m->cpu == cpu && m->chipset == (LONG) changed_prefs.chipset_mask
	    && m->chip == (LONG) changed_prefs.chipmem_size
	    && m->slow == (LONG) changed_prefs.bogomem_size
	    && m->fast == (LONG) changed_prefs.fastmem_size
	    && m->akiko == akiko_on ())
	    return i;
    }
    return 0;
}

static void write_prefs (void)
{
    ra_choice_set (g_model, &c_model, model_from_prefs (), 0);
    ra_choice_set (g_cpu,   &c_cpu,   cpu_value_from_prefs (), 3);
    ra_choice_set (g_speed, &c_speed, changed_prefs.m68k_speed < 0 ? -1 : 0, 1);
    RA_SET (g_idle,   INTEGER_Number, changed_prefs.cpu_idle);
    RA_SET (g_compat, GA_Selected,    changed_prefs.cpu_compatible ? TRUE : FALSE);
    RA_SET (g_cycle,  GA_Selected,    changed_prefs.cpu_cycle_exact ? TRUE : FALSE);

    ra_choice_set (g_chipset, &c_chipset, changed_prefs.chipset_mask, 1);
    ra_choice_set (g_coll,    &c_coll,    changed_prefs.collision_level, 0);
    RA_SET (g_ntsc,      GA_Selected, changed_prefs.ntscmode ? TRUE : FALSE);
    RA_SET (g_imm,       GA_Selected, changed_prefs.immediate_blits ? TRUE : FALSE);
    RA_SET (g_blitcycle, GA_Selected, changed_prefs.blitter_cycle_exact ? TRUE : FALSE);
    RA_SET (g_akiko,     GA_Selected, akiko_on () ? TRUE : FALSE);

    ra_choice_set (g_chip, &c_chip, changed_prefs.chipmem_size,   1);
    ra_choice_set (g_slow, &c_slow, changed_prefs.bogomem_size,   0);
    ra_choice_set (g_fast, &c_fast, changed_prefs.fastmem_size,   0);
    ra_choice_set (g_z3,   &c_z3,   changed_prefs.z3fastmem_size, 0);
    ra_choice_set (g_gfx,  &c_gfx,  changed_prefs.gfxmem_size,    0);

#ifdef JIT
    ra_choice_set (g_jitcache, &c_jit, changed_prefs.cachesize, 0);
    RA_SET (g_jithard,  GA_Selected,    changed_prefs.comp_hardflush ? TRUE : FALSE);
    RA_SET (g_jitconst, GA_Selected,    changed_prefs.comp_constjump ? TRUE : FALSE);
#endif
}

/* Abhaengigkeiten zwischen den Einstellungen */
static void apply_rules (void)
{
    int stopped = ra_emu_stopped ();

    /* Ab 68020 mit 32 Bit sind Zorro-III-RAM und Grafikkarte sinnvoll. */
    int cpu32 = changed_prefs.cpu_level >= 2 && !changed_prefs.address_space_24;

    ra_set_disabled (g_z3,  !stopped || !cpu32);
    ra_set_disabled (g_gfx, !stopped || !cpu32);
    if (!cpu32) {
	changed_prefs.z3fastmem_size = 0;
	changed_prefs.gfxmem_size    = 0;
	ra_choice_set (g_z3,  &c_z3,  0, 0);
	ra_choice_set (g_gfx, &c_gfx, 0, 0);
    }

    /* v17: Akiko gibt es nur mit AGA (CD32) */
    if (!(changed_prefs.chipset_mask & CSMASK_AGA) && akiko_on ()) {
	set_akiko (0);
	RA_SET (g_akiko, GA_Selected, FALSE);
	ra_write_page (PAGE_ID_HARDDISK);
    }
    ra_set_disabled (g_akiko, !stopped || !(changed_prefs.chipset_mask & CSMASK_AGA));

    /* Mit mehr als 2 MB Chip RAM gibt es kein Fast RAM. */
    if (changed_prefs.chipmem_size > 0x200000 && changed_prefs.fastmem_size) {
	changed_prefs.fastmem_size = 0;
	ra_choice_set (g_fast, &c_fast, 0, 0);
    }
    ra_set_disabled (g_fast, !stopped || changed_prefs.chipmem_size > 0x200000);

#ifdef JIT
    /* Der JIT braucht 32-Bit-Adressierung (siehe Tooltip). Ohne sie wird
     * der Cache abgeschaltet, sonst liefe er unsichtbar weiter (newcpu.c
     * waehlt die JIT-Schleife schon ab cpu_level 2). */
    if (!cpu32 && changed_prefs.cachesize) {
	changed_prefs.cachesize = 0;
	ra_choice_set (g_jitcache, &c_jit, 0, 0);
    }
    ra_set_disabled (g_jitcache, !stopped || !cpu32);
    ra_set_disabled (g_jithard,  !stopped || !cpu32);
    ra_set_disabled (g_jitconst, !stopped || !cpu32);
#endif

    /* JIT und "Compatible" / "Cycle-exact" schliessen sich aus (wie bei
     * WinUAE): mit JIT ignoriert die CPU-Schleife beides, Cycle-exact
     * wuerde aber das Chip-RAM- und Interrupt-Timing bremsen. */
    if (jit_active ()) {
	if (changed_prefs.cpu_compatible || changed_prefs.cpu_cycle_exact) {
	    changed_prefs.cpu_compatible  = 0;
	    changed_prefs.cpu_cycle_exact = 0;
	    RA_SET (g_compat, GA_Selected, FALSE);
	    RA_SET (g_cycle,  GA_Selected, FALSE);
	}
    }
    ra_set_disabled (g_compat, !stopped || jit_active ());
    ra_set_disabled (g_cycle,  !stopped || jit_active ());
}

static void apply_model (int idx)
{
    const struct ra_model *m;

    if (idx <= 0 || idx >= (int)(sizeof models / sizeof models[0]))
	return;
    m = &models[idx];

    changed_prefs.cpu_level        = m->cpu >> 1;
    changed_prefs.address_space_24 = (m->cpu < 8 && !(m->cpu & 1)) ? 1 : 0;
    changed_prefs.chipset_mask     = m->chipset;
    changed_prefs.chipmem_size     = m->chip;
    changed_prefs.bogomem_size     = m->slow;
    changed_prefs.fastmem_size     = m->fast;
    changed_prefs.z3fastmem_size   = 0;
    changed_prefs.gfxmem_size      = 0;
    set_akiko (m->akiko);
    /* v17: IDE-Schnittstelle des Modells; ein eingetragenes IDE-Laufwerk
     * (Hardfile oder ATAPI-CD) behaelt seine Schnittstelle */
    if (!changed_prefs.ide_hardfile[0][0] && !changed_prefs.ide_hardfile[1][0]
	&& !(changed_prefs.cddrive[0] && (changed_prefs.cd_controller == CD_CTRL_IDE0
					  || changed_prefs.cd_controller == CD_CTRL_IDE1)))
	changed_prefs.cs_ide = m->ide;
}

static int handle (ULONG gid, ULONG code)
{
    /* read_prefs() wurde vom Kern bereits aufgerufen. */
    if (gid == GID_MODEL) {
	apply_model (ra_choice_index (g_model, &c_model));
	write_prefs ();                 /* Gadgets aus den neuen Prefs setzen */
	ra_write_page (PAGE_ID_HARDDISK);
	ra_write_page (PAGE_ID_MEDIA);
    } else if (gid == GID_AKIKO) {
	ra_choice_set (g_model, &c_model, model_from_prefs (), 0);
	ra_write_page (PAGE_ID_HARDDISK);   /* CD drive on Akiko */
	ra_write_page (PAGE_ID_MEDIA);
    } else if (gid == GID_CPU || gid == GID_CHIPSET || gid == GID_CHIP || gid == GID_SLOW
	       || gid == GID_FAST || gid == GID_Z3 || gid == GID_GFX) {
	/* Von Hand geaendert: nicht mehr exakt das gewaehlte Modell */
	ra_choice_set (g_model, &c_model, 0, 0);
    }
    apply_rules ();
    return 1;
}

static void state (int st)
{
    BOOL lock = st != UAE_STATE_STOPPED;

    ra_set_disabled (g_model,   lock);
    ra_set_disabled (g_cpu,     lock);
    ra_set_disabled (g_chipset, lock);
    ra_set_disabled (g_ntsc,    lock);
    ra_set_disabled (g_blitcycle, lock);
    ra_set_disabled (g_akiko,   lock);
    ra_set_disabled (g_chip,    lock);
    ra_set_disabled (g_slow,    lock);
    apply_rules ();             /* auch Compatible / Cycle-exact */
}

static void write_and_rules (void)
{
    write_prefs ();
    apply_rules ();
}

static void cleanup (void)
{
    int i;

    for (i = 0; i < NUM_CHOICES; i++)
	ra_choice_free (all_choices[i]);
}

/* Tooltips (mouse-over help). English text = key in Catalogs/E-UAE.cd. */
static const struct ra_hint hints[] = {
    { GID_MODEL, 1, "Presets for common Amiga models. Sets CPU, chipset and memory; ROM and drives stay unchanged. CD32 also switches on the Akiko chip with its CD drive - choose the CD32 Kickstart and extended ROM on the Media page." },
    { GID_CPU, 1, "Processor to emulate. 68020 + 68881 adds a floating point unit, 68040 and 68060 always have one. The JIT compiler needs a 68020 or higher with 32 bit addressing." },
    { GID_SPEED, 1, "Maximum runs as fast as the host allows. Original keeps the speed of a real Amiga." },
    { GID_IDLE, 1, "Gives CPU time back to the system while the Amiga is idle. 0 = off; otherwise E-UAE sleeps 1 ms after every n raster lines in which the Amiga waits, so smaller values save more (e.g. 30-60). Only works with CPU speed Maximum." },
    { GID_COMPAT, 1, "More compatible, but slower CPU emulation. Needed by some games. Not available while the JIT compiler is on." },
    { GID_CYCLEEXACT, 1, "Cycle-exact timing for demanding demos and games. Very slow. Fully cycle-exact only for the 68000; with a 68020 or higher only chip RAM and interrupt timing are exact. Not available while the JIT compiler is on." },
    { GID_CHIPSET, 1, "Chipset to emulate: OCS (A500), ECS (A500+, A600) or AGA (A1200, A4000)." },
    { GID_COLLISION, 1, "Which sprite and playfield collisions are detected. Higher levels are more accurate but slower." },
    { GID_NTSC, 1, "Use NTSC video timing (60 Hz) instead of PAL (50 Hz)." },
    { GID_IMMBLITS, 1, "Run blitter operations immediately instead of cycle by cycle. Faster, slightly less compatible." },
    { GID_BLITCYCLE, 1, "Cycle-exact blitter. Needed by some demos, but slower." },
    { GID_AKIKO, 1, "The custom chip of the Amiga CD32: CD-ROM controller (the CD drive is attached to it), chunky-to-planar converter and 1 KB NVRAM for game saves (file PROGDIR:cd32.nvr). Needs AGA and the CD32 Kickstart + extended ROM. The joystick in Amiga port 1 becomes a CD32 pad." },
    { GID_CHIP, 1, "Chip RAM size. Chip RAM is shared by the CPU and the graphics and sound hardware." },
    { GID_SLOW, 1, "Slow RAM (the trapdoor memory expansion of the A500)." },
    { GID_FAST, 1, "Fast RAM (Zorro II) for the CPU. Not available with more than 2 MB Chip RAM." },
    { GID_Z3, 1, "Zorro III Fast RAM. Needs a 68020 or higher with 32 bit addressing." },
    { GID_GFX, 1, "Memory of the emulated Picasso96 graphics card. Needs a 68020 or higher with 32 bit addressing." },
    { GID_JITCACHE, 1, "Size of the JIT translation cache. Off disables the JIT compiler. Needs a 68020 or higher with 32 bit addressing." },
    { GID_JITHARD, 1, "When the Amiga flushes its CPU cache, throw away all translated code (safe, slower). Off: keep the code and translate again only blocks whose memory has changed (checksum test)." },
    { GID_JITCONST, 1, "Optimization for jumps with fixed targets. Can be faster, but may reduce compatibility." },
    { 0, 0, NULL }
};

const struct ra_page ra_page_hardware = {
    "Hardware", PAGE_ID_HARDWARE, create, read_prefs, write_and_rules, handle, state, cleanup, hints
};
