/*
 * E-UAE - ReAction GUI: Seite "Sound"
 *
 *   Ausgabemodus, Kanaele, Frequenz, Interpolation, Lautstaerke, Latenz,
 *   Stereo-Trennung und Floppy-Geraeusche.
 */

#include "reaction_gui.h"

#define BASE GID_PAGE_BASE (PAGE_ID_SOUND)

enum {
    GID_MODE = BASE + 1, GID_CHANNELS, GID_FREQ, GID_INTERPOL,
    GID_VOLUME, GID_LATENCY, GID_SEPARATION, GID_MIXDELAY,
    GID_CLICK = BASE + 20,          /* +0..3 */
    GID_CLICKVOL = BASE + 30
};

static const char *const mode_names[] = { "Off", "Emulated (no output)", "Normal", "Exact", NULL };
static const LONG mode_values[] = { 0, 1, 2, 3 };
RA_CHOICE (c_mode, mode_names, mode_values);

static const char *const chan_names[] = { "Mono", "Stereo", NULL };
static const LONG chan_values[] = { 0, 1 };
RA_CHOICE (c_chan, chan_names, chan_values);

static const char *const freq_names[] = { "11025 Hz", "22050 Hz", "44100 Hz", "48000 Hz", NULL };
static const LONG freq_values[] = { 11025, 22050, 44100, 48000 };
RA_CHOICE (c_freq, freq_names, freq_values);

static const char *const interp_names[] = { "None", "RH", "Crux", "Sinc", NULL };
static const LONG interp_values[] = { 0, 1, 2, 3 };
RA_CHOICE (c_interp, interp_names, interp_values);

static struct ra_choice *const all_choices[] = { &c_mode, &c_chan, &c_freq, &c_interp };
#define NUM_CHOICES (int)(sizeof all_choices / sizeof all_choices[0])

static Object *g_mode, *g_chan, *g_freq, *g_interp;
static Object *g_vol, *g_lat, *g_sep, *g_mix;
#ifdef DRIVESOUND
static Object *g_click[4], *g_clickvol;
#endif

static Object *create (void)
{
    return VLayoutObject,
	LAYOUT_SpaceOuter, TRUE,

	LAYOUT_AddChild, VLayoutObject,
	    LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("Output"), LAYOUT_SpaceOuter, TRUE,
	    LAYOUT_AddChild, g_mode   = ra_choice_gadget (GID_MODE, &c_mode), CHILD_MinWidth, ra_names_width (c_mode.names),         CHILD_Label, ra_label ("Mode:"),
	    LAYOUT_AddChild, g_chan   = ra_choice_gadget (GID_CHANNELS, &c_chan), CHILD_MinWidth, ra_names_width (c_chan.names),     CHILD_Label, ra_label ("Channels:"),
	    LAYOUT_AddChild, g_freq   = ra_choice_gadget (GID_FREQ, &c_freq), CHILD_MinWidth, ra_names_width (c_freq.names),         CHILD_Label, ra_label ("Frequency:"),
	    LAYOUT_AddChild, g_interp = ra_choice_gadget (GID_INTERPOL, &c_interp), CHILD_MinWidth, ra_names_width (c_interp.names),   CHILD_Label, ra_label ("Interpolation:"),
	    LAYOUT_AddChild, g_vol = ra_integer (GID_VOLUME, 0, 100, 0),              CHILD_Label, ra_label ("Attenuation % (0=loud):"),
	    LAYOUT_AddChild, g_lat = ra_integer (GID_LATENCY, 0, 1000, 100),          CHILD_Label, ra_label ("Latency (ms):"),
	    LAYOUT_AddChild, g_sep = ra_integer (GID_SEPARATION, 0, 10, 7),           CHILD_Label, ra_label ("Stereo separation (0-10):"),
	    LAYOUT_AddChild, g_mix = ra_integer (GID_MIXDELAY, 0, 20, 0),             CHILD_Label, ra_label ("Stereo mix delay:"),
	LayoutEnd,
	CHILD_WeightedHeight, 0,
#ifdef DRIVESOUND
	LAYOUT_AddChild, VLayoutObject,
	    LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, ra_tr ("Floppy sounds"), LAYOUT_SpaceOuter, TRUE,
	    LAYOUT_AddChild, g_click[0] = ra_checkbox (GID_CLICK + 0, "DF0 drive sounds"),
	    LAYOUT_AddChild, g_click[1] = ra_checkbox (GID_CLICK + 1, "DF1 drive sounds"),
	    LAYOUT_AddChild, g_click[2] = ra_checkbox (GID_CLICK + 2, "DF2 drive sounds"),
	    LAYOUT_AddChild, g_click[3] = ra_checkbox (GID_CLICK + 3, "DF3 drive sounds"),
	    LAYOUT_AddChild, g_clickvol = ra_integer (GID_CLICKVOL, 0, 100, 0), CHILD_Label, ra_label ("Attenuation %:"),
	LayoutEnd,
	CHILD_WeightedHeight, 0,
#endif
	LAYOUT_AddChild, SpaceObject, SpaceEnd,
    LayoutEnd;
}

static void read_prefs (void)
{
    changed_prefs.produce_sound = ra_choice_value (g_mode,   &c_mode);
    changed_prefs.sound_stereo  = ra_choice_value (g_chan,   &c_chan);
    changed_prefs.sound_freq    = ra_choice_value (g_freq,   &c_freq);
    changed_prefs.sound_interpol = ra_choice_value (g_interp, &c_interp);
    changed_prefs.sound_volume  = ra_get_num (g_vol, INTEGER_Number);
    changed_prefs.sound_latency = ra_get_num (g_lat, INTEGER_Number);
    changed_prefs.sound_stereo_separation = ra_get_num (g_sep, INTEGER_Number);
    changed_prefs.sound_mixed_stereo      = ra_get_num (g_mix, INTEGER_Number);
#ifdef DRIVESOUND
    {
	int i;
	for (i = 0; i < 4; i++)
	    changed_prefs.dfxclick[i] = ra_get_num (g_click[i], GA_Selected) ? 1 : 0;
	changed_prefs.dfxclickvolume = ra_get_num (g_clickvol, INTEGER_Number);
    }
#endif
}

static void write_prefs (void)
{
    ra_choice_set (g_mode,   &c_mode,   changed_prefs.produce_sound,  2);
    ra_choice_set (g_chan,   &c_chan,   changed_prefs.sound_stereo ? 1 : 0, 1);
    ra_choice_set (g_freq,   &c_freq,   changed_prefs.sound_freq,     2);
    ra_choice_set (g_interp, &c_interp, changed_prefs.sound_interpol, 0);
    RA_SET (g_vol, INTEGER_Number, changed_prefs.sound_volume);
    RA_SET (g_lat, INTEGER_Number, changed_prefs.sound_latency);
    RA_SET (g_sep, INTEGER_Number, changed_prefs.sound_stereo_separation);
    RA_SET (g_mix, INTEGER_Number, changed_prefs.sound_mixed_stereo < 0 ? 0 : changed_prefs.sound_mixed_stereo);
#ifdef DRIVESOUND
    {
	int i;
	for (i = 0; i < 4; i++)
	    RA_SET (g_click[i], GA_Selected, changed_prefs.dfxclick[i] ? TRUE : FALSE);
	RA_SET (g_clickvol, INTEGER_Number, changed_prefs.dfxclickvolume);
    }
#endif
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
    { GID_MODE, 1, "Off: no sound. Emulated: the sound chip runs but nothing is played. Normal and Exact play sound; Exact is more precise but slower." },
    { GID_CHANNELS, 1, "Mono or stereo sound output." },
    { GID_FREQ, 1, "Sample rate of the sound output. Higher means better quality but more CPU load." },
    { GID_INTERPOL, 1, "Smooths the sound. Sinc has the highest quality but needs the most CPU time." },
    { GID_VOLUME, 1, "Lowers the output volume in percent. 0 means full volume." },
    { GID_LATENCY, 1, "Size of the sound buffer in milliseconds. Small values react faster but may crackle." },
    { GID_SEPARATION, 1, "Stereo separation from 0 to 10." },
    { GID_MIXDELAY, 1, "Stereo mix delay (0 = off)." },
    { GID_CLICK, 4, "Play floppy drive sounds (clicks) for this drive." },
    { GID_CLICKVOL, 1, "Volume reduction for the drive sounds in percent." },
    { 0, 0, NULL }
};

const struct ra_page ra_page_sound = {
    "Sound", PAGE_ID_SOUND, create, read_prefs, write_prefs, handle, NULL, cleanup, hints
};
