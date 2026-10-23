 /*
  * E-UAE - The portable Amiga Emulator
  *
  * CD-ROM image drive (v16)
  *
  * Emulates a SCSI/ATAPI CD-ROM drive (MAX_CD_DRIVES, one) whose media are
  * image files on the host:
  *   - ISO images (2048 bytes per sector, or raw 2352 bytes per sector)
  *   - CUE sheets with BIN files: MODE1/2048, MODE1/2352, MODE2/2336,
  *     MODE2/2352 and AUDIO tracks, several FILE entries, INDEX 00/01,
  *     PREGAP/POSTGAP
  *   - CD audio (CD-DA) playback, mixed into the emulator's sound output
  *
  * The drive itself is a SCSI-2 / MMC CD-ROM. The Amiga side talks to it
  * either through uaescsi.device unit 0 (scsiemul.c, HD_SCSICMD or the
  * trackdisk-style commands) or as an ATAPI drive at the emulated IDE
  * interface (gayle.c, PACKET command -> cdimage_scsi_cmd()); this file
  * implements the SCSI command set for both.
  *
  * v17: on a CD32 (cd32cd=true) the drive belongs to the Akiko chip
  * (akiko.c), which uses the high-level functions cdimage_hl_*() at the
  * end of this file instead of SCSI commands.
  *
  * Threads: SCSI commands arrive on the uaescsi.device unit threads, the
  * audio mixer and the media change run in the emulator thread. All unit
  * state is protected by one lock per drive.
  */

#ifdef __amigaos4__
#include <exec/semaphores.h>
#include <proto/exec.h>
#endif

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "memory.h"
#include "custom.h"
#include "gui.h"
#include "blkdev.h"
#include "scsidev.h"
#include "cdimage.h"

#include <fcntl.h>
#include <ctype.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

/* 64-bit file i/o on AmigaOS 4 newlib, as in hardfile_unix.c */
#if defined(__amigaos4__) && defined(__NEWLIB__)
typedef _off64_t cd_off_t;
#define cd_open  open64
#define cd_lseek lseek64
#else
typedef off_t cd_off_t;
#define cd_open  open
#define cd_lseek lseek
#endif
#ifndef O_BINARY
#define O_BINARY 0
#endif

/*#define CDIMAGE_DEBUG*/
#ifdef CDIMAGE_DEBUG
#define CDLOG(x) write_log x
#else
#define CDLOG(x)
#endif

/* ---------------------------------------------------------------------- */
/* Lock                                                                   */

#ifdef __amigaos4__
typedef struct SignalSemaphore cd_lock_t;
#define cd_lock_init(l)    InitSemaphore (l)
#define cd_lock(l)         ObtainSemaphore (l)
#define cd_unlock(l)       ReleaseSemaphore (l)
#elif defined (HAVE_PTHREAD_H) || defined (__unix__) || defined (__APPLE__)
#include <pthread.h>
typedef pthread_mutex_t cd_lock_t;
static void cd_lock_init (cd_lock_t *l)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init (&a);
    pthread_mutexattr_settype (&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init (l, &a);
    pthread_mutexattr_destroy (&a);
}
#define cd_lock(l)         pthread_mutex_lock (l)
#define cd_unlock(l)       pthread_mutex_unlock (l)
#else
#include "threaddep/thread.h"
typedef uae_sem_t cd_lock_t;
#define cd_lock_init(l)    uae_sem_init (l, 0, 1)
#define cd_lock(l)         uae_sem_wait (l)
#define cd_unlock(l)       uae_sem_post (l)
#endif

/* ---------------------------------------------------------------------- */
/* Image description                                                      */

#define CD_RAW_SECTOR     2352
#define CD_DATA_SECTOR    2048
#define CD_FRAMES_SECTOR  588          /* stereo sample frames per sector */
#define CD_MAX_TRACKS     99
#define CD_MAX_FILES      CD_MAX_TRACKS
#define CD_LEADIN         150          /* 2 seconds: MSF 00:02:00 = LBA 0 */
#define CD_BLOCK_CHUNK    16           /* blocks per host read             */

#define TRK_AUDIO      0
#define TRK_MODE1      1               /* user data at 16 in a raw sector  */
#define TRK_MODE2      2               /* XA form 1: user data at 24       */
#define TRK_MODE2_RAW  3               /* MODE2/2336 formless              */

struct cdfile {
    int fd;
    uae_u64 size;
    char name[MAX_DPATH];
};

struct cdtrack {
    int number;
    int type;            /* TRK_*                                     */
    int secsize;         /* bytes per sector in the file              */
    int file;            /* index in cdimage.files                    */
    uae_u64 fileoffset;  /* byte offset of the first sector in file   */
    int first;           /* absolute LBA of the first sector in file  */
    int start;           /* absolute LBA of INDEX 01                  */
    int end;             /* absolute LBA after the last sector        */
    int pregap;          /* PREGAP sectors (not in the file)          */
    int postgap;         /* POSTGAP sectors (not in the file)         */
};

struct cdimage {
    int ntracks;
    struct cdtrack tracks[CD_MAX_TRACKS];
    int nfiles;
    struct cdfile files[CD_MAX_FILES];
    int leadout;         /* absolute LBA of the lead-out              */
};

/* SCSI audio status (READ SUB-CHANNEL) */
#define AUDIO_PLAYING    0x11
#define AUDIO_PAUSED     0x12
#define AUDIO_COMPLETED  0x13
#define AUDIO_ERROR      0x14
#define AUDIO_NONE       0x15

struct cdunit {
    int present;               /* drive configured                     */
    int on_scsi;               /* attached to uaescsi.device            */
    int on_akiko;              /* v17: CD32 Akiko drive (akiko.c)       */
    int opencnt;
    cd_lock_t lock;

    /* medium */
    char path[MAX_DPATH];      /* image in the drive ("" = empty)      */
    int inserted;
    struct cdimage img;
    int changecnt;             /* host-side change counter (GUI)       */
    int unit_attention;        /* report 6/28 once after a change      */
    int media_event;           /* GET EVENT STATUS: 0 none, 2 new, 3 removed */
    int prevent;               /* PREVENT MEDIUM REMOVAL               */
    int eject_request;         /* eject requested by the Amiga side    */

    /* pending change from the GUI: eject first, insert later */
    int insert_delay;
    char insert_path[MAX_DPATH];

    /* sense data */
    uae_u8 sense_key, asc, ascq;

    /* CD-DA */
    int audio_status;
    uae_u64 play_frame;        /* current position in sample frames    */
    uae_u64 play_end;          /* end position in sample frames        */
    uae_u32 play_frac;         /* 16.16 resampling fraction            */
    int last_lba;              /* position reported when not playing   */
    int volume[2];             /* 0..255 (MODE page 0x0E)              */
    int cache_lba;             /* first sector in the audio cache      */
    uae_u8 cache[2 * CD_RAW_SECTOR];
    int vsyncs_since_mix;

    /* buffers */
    uae_u8 secbuf[CD_RAW_SECTOR + 296 + 96];
    uae_u8 scsibuf[DEVICE_SCSI_BUFSIZE];
    uae_u8 blockbuf[CD_BLOCK_CHUNK * 2048];   /* not on the stack: unit threads have 16 KB */
};

static struct cdunit cdunits[MAX_CD_DRIVES];
static int bus_open;

/* ---------------------------------------------------------------------- */
/* Helpers                                                                */

static void lba_to_msf (int lba, uae_u8 *p)
{
    lba += CD_LEADIN;
    if (lba < 0)
	lba = 0;
    p[0] = lba / (60 * 75);
    p[1] = (lba / 75) % 60;
    p[2] = lba % 75;
}

static int msf_to_lba (const uae_u8 *p)
{
    return (p[0] * 60 + p[1]) * 75 + p[2] - CD_LEADIN;
}

static uae_u8 tobcd (int v)
{
    return ((v / 10) << 4) | (v % 10);
}

static int ctrl_of (const struct cdtrack *t)
{
    return t->type == TRK_AUDIO ? 0x10 : 0x14;   /* ADR 1, control */
}

static uae_u32 rl (const uae_u8 *p) { return ((uae_u32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uae_u16 rw (const uae_u8 *p) { return (p[0] << 8) | p[1]; }
static void wl (uae_u8 *p, uae_u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void ww (uae_u8 *p, uae_u16 v) { p[0] = v >> 8; p[1] = (uae_u8)v; }

static void put_addr (uae_u8 *p, int lba, int msf)
{
    if (msf) {
	p[0] = 0;
	lba_to_msf (lba, p + 1);
    } else
	wl (p, (uae_u32)lba);
}

static int read_file (struct cdfile *f, uae_u64 offset, uae_u8 *buf, int len)
{
    int got = 0;

    if (f->fd < 0)
	return 0;
    if (offset >= f->size)
	return 0;
    if (offset + len > f->size)
	len = (int)(f->size - offset);
    if (cd_lseek (f->fd, (cd_off_t)offset, SEEK_SET) == (cd_off_t)-1)
	return 0;
    while (got < len) {
	int r = read (f->fd, buf + got, len - got);
	if (r <= 0)
	    break;
	got += r;
    }
    return got;
}

/* ---------------------------------------------------------------------- */
/* Image loading                                                          */

static void image_close (struct cdimage *img)
{
    int i;
    for (i = 0; i < img->nfiles; i++) {
	if (img->files[i].fd >= 0)
	    close (img->files[i].fd);
	img->files[i].fd = -1;
    }
    memset (img, 0, sizeof *img);
}

static int add_file (struct cdimage *img, const char *name)
{
    struct cdfile *f;
    cd_off_t size;

    if (img->nfiles >= CD_MAX_FILES)
	return -1;
    f = &img->files[img->nfiles];
    f->fd = cd_open (name, O_RDONLY | O_BINARY);
    if (f->fd < 0) {
	write_log ("CDIMAGE: can't open '%s'\n", name);
	return -1;
    }
    size = cd_lseek (f->fd, 0, SEEK_END);
    if (size == (cd_off_t)-1) {
	close (f->fd);
	f->fd = -1;
	return -1;
    }
    f->size = (uae_u64)size;
    strncpy (f->name, name, MAX_DPATH - 1);
    f->name[MAX_DPATH - 1] = 0;
    return img->nfiles++;
}

static int has_suffix (const char *name, const char *suffix)
{
    size_t l = strlen (name), s = strlen (suffix);
    if (l < s)
	return 0;
    return strcasecmp (name + l - s, suffix) == 0;
}

static const uae_u8 cd_sync[12] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };

/* Plain image: ISO (2048) or raw 2352 bytes per sector, one data track */
static int load_iso (struct cdimage *img, const char *name)
{
    struct cdtrack *t;
    uae_u8 head[16];
    int fi = add_file (img, name);

    if (fi < 0)
	return 0;
    t = &img->tracks[0];
    t->number = 1;
    t->file = fi;
    t->type = TRK_MODE1;
    t->secsize = CD_DATA_SECTOR;
    if (read_file (&img->files[fi], 0, head, 16) == 16 && !memcmp (head, cd_sync, 12)
	&& (img->files[fi].size % CD_RAW_SECTOR) == 0) {
	t->secsize = CD_RAW_SECTOR;
	t->type = head[15] == 2 ? TRK_MODE2 : TRK_MODE1;
    }
    t->first = t->start = 0;
    t->end = (int)(img->files[fi].size / t->secsize);
    img->ntracks = 1;
    img->leadout = t->end;
    return t->end > 0;
}

/* "MM:SS:FF" -> frames */
static int parse_msf (const char *s)
{
    int m = 0, sec = 0, f = 0;
    if (sscanf (s, "%d:%d:%d", &m, &sec, &f) != 3)
	return -1;
    return (m * 60 + sec) * 75 + f;
}

static char *next_token (char **pp, char *out, int outlen)
{
    char *p = *pp;
    int n = 0;

    while (*p == ' ' || *p == '\t')
	p++;
    if (!*p)
	return NULL;
    if (*p == '"') {
	p++;
	while (*p && *p != '"') {
	    if (n < outlen - 1)
		out[n++] = *p;
	    p++;
	}
	if (*p == '"')
	    p++;
    } else {
	while (*p && *p != ' ' && *p != '\t') {
	    if (n < outlen - 1)
		out[n++] = *p;
	    p++;
	}
    }
    out[n] = 0;
    *pp = p;
    return out;
}

/* Directory part of the cue sheet path (keeps the separator) */
static void cue_dir (const char *cue, char *dir, int len)
{
    int i, l;
    strncpy (dir, cue, len - 1);
    dir[len - 1] = 0;
    l = strlen (dir);
    for (i = l - 1; i >= 0; i--) {
	if (dir[i] == '/' || dir[i] == ':' || dir[i] == '\\')
	    break;
    }
    dir[i + 1] = 0;
}

static int path_is_absolute (const char *p)
{
    if (p[0] == '/' || p[0] == '\\')
	return 1;
#ifdef TARGET_AMIGAOS
    if (strchr (p, ':'))
	return 1;
#endif
    if (isalpha ((unsigned char)p[0]) && p[1] == ':')
	return 1;
    return 0;
}

/* A FILE entry: try the name as given, relative to the cue sheet, and the
 * bare file name next to the cue sheet (cue sheets made on other systems
 * often carry foreign absolute paths). */
static int open_cue_file (struct cdimage *img, const char *cue, const char *name)
{
    char dir[MAX_DPATH], tmp[MAX_DPATH * 2];
    const char *base;
    int fi;

    cue_dir (cue, dir, sizeof dir);
    if (path_is_absolute (name)) {
	FILE *f = fopen (name, "rb");
	if (f) {
	    fclose (f);
	    return add_file (img, name);
	}
    } else {
	sprintf (tmp, "%s%s", dir, name);
	{
	    FILE *f = fopen (tmp, "rb");
	    if (f) {
		fclose (f);
		return add_file (img, tmp);
	    }
	}
    }
    base = name + strlen (name);
    while (base > name && base[-1] != '/' && base[-1] != '\\' && base[-1] != ':')
	base--;
    sprintf (tmp, "%s%s", dir, base);
    fi = add_file (img, tmp);
    return fi;
}

static int load_cue (struct cdimage *img, const char *name)
{
    FILE *f = fopen (name, "r");
    char line[1024], tok[MAX_DPATH], arg[MAX_DPATH];
    int curfile = -1;
    struct cdtrack *t = NULL;
    int idx0[CD_MAX_TRACKS], idx1[CD_MAX_TRACKS];
    int i, running, ok = 1;

    if (!f) {
	write_log ("CDIMAGE: can't open cue sheet '%s'\n", name);
	return 0;
    }
    for (i = 0; i < CD_MAX_TRACKS; i++)
	idx0[i] = idx1[i] = -1;

    while (fgets (line, sizeof line, f)) {
	char *p = line;
	int l = strlen (line);
	while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r'))
	    line[--l] = 0;
	/* skip a UTF-8 byte order mark */
	if ((uae_u8)p[0] == 0xef && (uae_u8)p[1] == 0xbb && (uae_u8)p[2] == 0xbf)
	    p += 3;
	if (!next_token (&p, tok, sizeof tok))
	    continue;
	if (!strcasecmp (tok, "FILE")) {
	    if (!next_token (&p, arg, sizeof arg)) {
		ok = 0;
		break;
	    }
	    next_token (&p, tok, sizeof tok);   /* BINARY / WAVE / MOTOROLA ... */
	    if (strcasecmp (tok, "BINARY") && tok[0])
		write_log ("CDIMAGE: file type '%s' not supported, treated as BINARY\n", tok);
	    curfile = open_cue_file (img, name, arg);
	    if (curfile < 0) {
		ok = 0;
		break;
	    }
	} else if (!strcasecmp (tok, "TRACK")) {
	    int num;
	    if (curfile < 0 || img->ntracks >= CD_MAX_TRACKS
		|| !next_token (&p, arg, sizeof arg) || !next_token (&p, tok, sizeof tok)) {
		ok = 0;
		break;
	    }
	    num = atoi (arg);
	    t = &img->tracks[img->ntracks++];
	    memset (t, 0, sizeof *t);
	    t->number = num;
	    t->file = curfile;
	    if (!strcasecmp (tok, "AUDIO")) {
		t->type = TRK_AUDIO;
		t->secsize = CD_RAW_SECTOR;
	    } else if (!strcasecmp (tok, "MODE1/2048")) {
		t->type = TRK_MODE1;
		t->secsize = 2048;
	    } else if (!strcasecmp (tok, "MODE1/2352")) {
		t->type = TRK_MODE1;
		t->secsize = 2352;
	    } else if (!strcasecmp (tok, "MODE2/2352")) {
		t->type = TRK_MODE2;
		t->secsize = 2352;
	    } else if (!strcasecmp (tok, "MODE2/2336")) {
		t->type = TRK_MODE2_RAW;
		t->secsize = 2336;
	    } else if (!strcasecmp (tok, "MODE2/2048")) {
		t->type = TRK_MODE2;
		t->secsize = 2048;
	    } else {
		write_log ("CDIMAGE: track mode '%s' not supported\n", tok);
		ok = 0;
		break;
	    }
	} else if (!strcasecmp (tok, "INDEX")) {
	    int n, v;
	    if (!t || !next_token (&p, arg, sizeof arg) || !next_token (&p, tok, sizeof tok)) {
		ok = 0;
		break;
	    }
	    n = atoi (arg);
	    v = parse_msf (tok);
	    if (v < 0) {
		ok = 0;
		break;
	    }
	    if (n == 0)
		idx0[img->ntracks - 1] = v;
	    else if (n == 1)
		idx1[img->ntracks - 1] = v;
	} else if (!strcasecmp (tok, "PREGAP")) {
	    if (t && next_token (&p, arg, sizeof arg))
		t->pregap = parse_msf (arg) > 0 ? parse_msf (arg) : 0;
	} else if (!strcasecmp (tok, "POSTGAP")) {
	    if (t && next_token (&p, arg, sizeof arg))
		t->postgap = parse_msf (arg) > 0 ? parse_msf (arg) : 0;
	}
	/* CATALOG, PERFORMER, TITLE, REM, FLAGS, ISRC, SONGWRITER: ignored */
    }
    fclose (f);

    if (!ok || img->ntracks == 0) {
	write_log ("CDIMAGE: '%s' is not a valid cue sheet\n", name);
	return 0;
    }

    /* Lay out the tracks: absolute positions in sectors, relative to the
     * first sector of the first file */
    running = 0;
    for (i = 0; i < img->ntracks; i++) {
	struct cdtrack *tr = &img->tracks[i];
	struct cdtrack *nx = i + 1 < img->ntracks ? &img->tracks[i + 1] : NULL;
	int fstart, fend, nsec;
	uae_u64 fsize = img->files[tr->file].size;
	uae_u64 base = 0;
	int j;

	if (idx1[i] < 0)
	    idx1[i] = idx0[i] >= 0 ? idx0[i] : 0;
	fstart = idx0[i] >= 0 ? idx0[i] : idx1[i];

	/* byte offset of sector 0 of this track's file region: sum of the
	 * sizes of earlier tracks in the same file (they may use different
	 * sector sizes) */
	for (j = 0; j < i; j++) {
	    if (img->tracks[j].file == tr->file) {
		base = img->tracks[j].fileoffset
		    + (uae_u64)(img->tracks[j].end - img->tracks[j].first) * img->tracks[j].secsize;
	    }
	}
	/* first track in this file: INDEX values start at the file start */
	{
	    int firstinfile = 1;
	    for (j = 0; j < i; j++)
		if (img->tracks[j].file == tr->file)
		    firstinfile = 0;
	    if (firstinfile) {
		base = 0;
		tr->fileoffset = (uae_u64)fstart * tr->secsize;
	    } else {
		/* previous track in the same file ended where this one begins */
		tr->fileoffset = base;
	    }
	}

	if (nx && nx->file == tr->file) {
	    int nstart = idx0[i + 1] >= 0 ? idx0[i + 1] : (idx1[i + 1] >= 0 ? idx1[i + 1] : fstart);
	    fend = nstart;
	    nsec = fend - fstart;
	} else {
	    if (tr->fileoffset >= fsize)
		nsec = 0;
	    else
		nsec = (int)((fsize - tr->fileoffset) / tr->secsize);
	}
	if (nsec < 0)
	    nsec = 0;

	running += tr->pregap;
	tr->first = running;
	tr->start = running + (idx1[i] - fstart);
	tr->end = running + nsec;
	if (tr->start > tr->end)
	    tr->start = tr->end;
	running = tr->end + tr->postgap;
    }
    img->leadout = running;

    /* LBA 0 is INDEX 01 of the first track */
    {
	int shift = img->tracks[0].start;
	if (shift) {
	    for (i = 0; i < img->ntracks; i++) {
		img->tracks[i].first -= shift;
		img->tracks[i].start -= shift;
		img->tracks[i].end -= shift;
	    }
	    img->leadout -= shift;
	}
    }
#ifdef CDIMAGE_DEBUG
    for (i = 0; i < img->ntracks; i++) {
	struct cdtrack *tr = &img->tracks[i];
	CDLOG (("CDIMAGE: track %2d type %d size %4d file %d off %llu first %d start %d end %d\n",
		tr->number, tr->type, tr->secsize, tr->file, (unsigned long long)tr->fileoffset,
		tr->first, tr->start, tr->end));
    }
#endif
    return img->leadout > 0;
}

static int image_open (struct cdimage *img, const char *name)
{
    int ok;
    memset (img, 0, sizeof *img);
    {
	int i;
	for (i = 0; i < CD_MAX_FILES; i++)
	    img->files[i].fd = -1;
    }
    if (has_suffix (name, ".cue"))
	ok = load_cue (img, name);
    else
	ok = load_iso (img, name);
    if (!ok) {
	image_close (img);
	return 0;
    }
    write_log ("CDIMAGE: '%s': %d track(s), %d sectors\n", name, img->ntracks, img->leadout);
    return 1;
}

/* ---------------------------------------------------------------------- */
/* Sector access                                                          */

/* Track containing absolute LBA (including pregap areas before INDEX 01) */
static struct cdtrack *find_track (struct cdimage *img, int lba)
{
    int i;
    for (i = img->ntracks - 1; i >= 0; i--) {
	struct cdtrack *t = &img->tracks[i];
	if (lba >= t->first - t->pregap && lba < img->leadout)
	    return lba < t->end + t->postgap ? t : NULL;
    }
    return NULL;
}

static struct cdtrack *track_by_number (struct cdimage *img, int num)
{
    int i;
    for (i = 0; i < img->ntracks; i++)
	if (img->tracks[i].number == num)
	    return &img->tracks[i];
    return NULL;
}

/* Fill a complete raw 2352-byte sector. Returns the track or NULL. */
static struct cdtrack *read_raw (struct cdunit *u, int lba, uae_u8 *out)
{
    struct cdimage *img = &u->img;
    struct cdtrack *t;
    uae_u8 *data;
    int mode;

    if (lba < 0 || lba >= img->leadout)
	return NULL;
    t = find_track (img, lba);
    if (!t)
	return NULL;

    memset (out, 0, CD_RAW_SECTOR);
    if (t->type == TRK_AUDIO) {
	if (lba >= t->first && lba < t->end)
	    read_file (&img->files[t->file], t->fileoffset + (uae_u64)(lba - t->first) * t->secsize,
		       out, CD_RAW_SECTOR);
	return t;
    }

    mode = t->type == TRK_MODE1 ? 1 : 2;
    if (lba >= t->first && lba < t->end) {
	uae_u64 off = t->fileoffset + (uae_u64)(lba - t->first) * t->secsize;
	switch (t->secsize) {
	    case 2352:
		read_file (&img->files[t->file], off, out, CD_RAW_SECTOR);
		return t;
	    case 2336:
		read_file (&img->files[t->file], off, out + 16, 2336);
		break;
	    default:
		data = out + (t->type == TRK_MODE1 ? 16 : 24);
		read_file (&img->files[t->file], off, data, 2048);
		if (t->type == TRK_MODE2)
		    out[18] = out[22] = 0x08;   /* subheader: data, form 1 */
		break;
	}
    }
    memcpy (out, cd_sync, 12);
    lba_to_msf (lba, out + 12);
    out[12] = tobcd (out[12]);
    out[13] = tobcd (out[13]);
    out[14] = tobcd (out[14]);
    out[15] = mode;
    return t;
}

/* Offset of the user data in a raw sector of this track */
static int user_offset (const struct cdtrack *t, const uae_u8 *raw)
{
    if (t->type == TRK_MODE1)
	return 16;
    if (t->type == TRK_MODE2_RAW)
	return 16;
    /* MODE2 raw: check the real header if there is one */
    if (t->secsize == CD_RAW_SECTOR && raw[15] == 1)
	return 16;
    return 24;
}

/* Read 2048-byte user data blocks. Returns the number of blocks read,
 * sets the sense data on error. */
static int read_blocks (struct cdunit *u, int lba, int count, uae_u8 *out)
{
    int n = 0;

    while (n < count) {
	struct cdtrack *t;
	int cur = lba + n;

	if (cur < 0 || cur >= u->img.leadout) {
	    u->sense_key = 5; u->asc = 0x21; u->ascq = 0;   /* LBA out of range */
	    break;
	}
	t = find_track (&u->img, cur);
	if (!t || t->type == TRK_AUDIO) {
	    u->sense_key = 5; u->asc = 0x64; u->ascq = 0;   /* illegal mode for this track */
	    break;
	}
	/* fast path: cooked 2048 byte sectors, read a run in one go */
	if (t->secsize == 2048 && cur >= t->first && cur < t->end) {
	    int run = count - n;
	    if (run > t->end - cur)
		run = t->end - cur;
	    memset (out + n * 2048, 0, run * 2048);
	    read_file (&u->img.files[t->file], t->fileoffset + (uae_u64)(cur - t->first) * 2048,
		       out + n * 2048, run * 2048);
	    n += run;
	    continue;
	}
	read_raw (u, cur, u->secbuf);
	memcpy (out + n * 2048, u->secbuf + user_offset (t, u->secbuf), 2048);
	n++;
    }
    return n;
}

/* ---------------------------------------------------------------------- */
/* Medium handling                                                        */

static void stop_audio (struct cdunit *u, int status)
{
    if (u->audio_status == AUDIO_PLAYING || u->audio_status == AUDIO_PAUSED)
	u->last_lba = (int)(u->play_frame / CD_FRAMES_SECTOR);
    u->audio_status = status;
}

static void unit_eject (struct cdunit *u)
{
    stop_audio (u, AUDIO_NONE);
    image_close (&u->img);
    if (u->inserted) {
	u->inserted = 0;
	u->media_event = 3;
	u->unit_attention = 1;
    }
    u->path[0] = 0;
    u->cache_lba = -1;
    u->prevent = 0;
    u->changecnt++;
}

static int unit_insert (struct cdunit *u, const char *path)
{
    unit_eject (u);
    if (!path || !path[0])
	return 1;
    strncpy (u->path, path, MAX_DPATH - 1);
    u->path[MAX_DPATH - 1] = 0;
    if (!image_open (&u->img, path)) {
	u->path[0] = 0;
	return 0;
    }
    u->inserted = 1;
    u->media_event = 2;
    u->unit_attention = 1;
    u->last_lba = 0;
    u->changecnt++;
    return 1;
}

/* ---------------------------------------------------------------------- */
/* SCSI command set                                                       */

struct xfer {
    uae_u8 *host;        /* host buffer (exec_in) or NULL             */
    uaecptr abuf;        /* Amiga buffer (exec_direct); not "amiga": predefined macro of ppc-amigaos-gcc */
    int maxlen;          /* buffer / allocation length                */
    int len;             /* bytes transferred                         */
};

static void xfer_put (struct xfer *x, const uae_u8 *data, int len)
{
    if (len > x->maxlen - x->len)
	len = x->maxlen - x->len;
    if (len <= 0)
	return;
    if (x->host)
	memcpy (x->host + x->len, data, len);
    else {
	uaecptr a = x->abuf + x->len;
	int i;
	if (valid_address (a, len)) {
	    uae_u8 *p = get_real_address (a);
	    memcpy (p, data, len);
	} else {
	    for (i = 0; i < len; i++)
		put_byte (a + i, data[i]);
	}
    }
    x->len += len;
}

#define SENSE(k,a,q) do { u->sense_key = (k); u->asc = (a); u->ascq = (q); } while (0)
#define NOT_READY()        SENSE (2, 0x3a, 0x00)
#define ILLEGAL_OPCODE()   SENSE (5, 0x20, 0x00)
#define ILLEGAL_FIELD()    SENSE (5, 0x24, 0x00)
#define LBA_RANGE()        SENSE (5, 0x21, 0x00)

/* commands that report "not ready" without a medium */
static int needs_medium (uae_u8 op)
{
    switch (op) {
	case 0x00: case 0x08: case 0x28: case 0xa8: case 0x25: case 0x43: case 0x44:
	case 0x45: case 0xa5: case 0x47: case 0x48: case 0x4b: case 0xbe: case 0xb9:
	case 0x2b: case 0x0b: case 0x51: case 0x52:
	    return 1;
    }
    return 0;
}

static void audio_set_cache_invalid (struct cdunit *u)
{
    u->cache_lba = -1;
}

static int start_play (struct cdunit *u, int start, int end)
{
    struct cdtrack *t;

    if (end > u->img.leadout)
	end = u->img.leadout;
    if (start < 0)
	start = 0;
    if (start >= end) {
	stop_audio (u, AUDIO_COMPLETED);
	return 1;
    }
    t = find_track (&u->img, start);
    if (!t || t->type != TRK_AUDIO) {
	SENSE (5, 0x64, 0x00);
	return 0;
    }
    u->play_frame = (uae_u64)start * CD_FRAMES_SECTOR;
    u->play_end = (uae_u64)end * CD_FRAMES_SECTOR;
    u->play_frac = 0;
    u->audio_status = AUDIO_PLAYING;
    u->vsyncs_since_mix = 0;
    audio_set_cache_invalid (u);
    CDLOG (("CDIMAGE: play %d - %d\n", start, end));
    return 1;
}

static int current_lba (struct cdunit *u)
{
    if (u->audio_status == AUDIO_PLAYING || u->audio_status == AUDIO_PAUSED)
	return (int)(u->play_frame / CD_FRAMES_SECTOR);
    return u->last_lba;
}

static void do_inquiry (struct cdunit *u, const uae_u8 *cmd, struct xfer *x)
{
    uae_u8 b[36];
    int unit = u - cdunits;

    memset (b, 0, sizeof b);
    if (cmd[1] & 1) {
	/* vital product data */
	if (cmd[2] == 0x00) {
	    b[0] = INQ_ROMD; b[3] = 2; b[4] = 0x00; b[5] = 0x80;
	    xfer_put (x, b, 6);
	} else if (cmd[2] == 0x80) {
	    b[0] = INQ_ROMD; b[1] = 0x80; b[3] = 8;
	    sprintf ((char *)b + 4, "UAECD%03d", unit);
	    xfer_put (x, b, 12);
	} else
	    ILLEGAL_FIELD ();
	return;
    }
    b[0] = INQ_ROMD;
    b[1] = 0x80;              /* removable */
    b[2] = 2;                 /* SCSI-2    */
    b[3] = 2;                 /* response data format */
    b[4] = 31;
    b[7] = 0x10;              /* sync */
    memcpy (b + 8,  "UAE     ", 8);
    memcpy (b + 16, "CD-ROM image    ", 16);
    memcpy (b + 32, "0.16", 4);
    xfer_put (x, b, 36);
}

static int mode_page (struct cdunit *u, int page, int pc, uae_u8 *p)
{
    switch (page) {
	case 0x01:          /* read error recovery */
	    memset (p, 0, 8);
	    p[0] = 0x01; p[1] = 6;
	    return 8;
	case 0x0d:          /* CD device parameters */
	    memset (p, 0, 8);
	    p[0] = 0x0d; p[1] = 6;
	    p[5] = 60; p[7] = 75;
	    return 8;
	case 0x0e:          /* CD audio control */
	    memset (p, 0, 16);
	    p[0] = 0x0e; p[1] = 14;
	    p[2] = 0x04;    /* immed */
	    p[8] = 1;  p[9]  = pc == 1 ? 0xff : u->volume[0];
	    p[10] = 2; p[11] = pc == 1 ? 0xff : u->volume[1];
	    return 16;
	case 0x2a:          /* capabilities and mechanical status */
	    memset (p, 0, 20);
	    p[0] = 0x2a; p[1] = 18;
	    p[2] = 0x03;    /* reads CD-R/RW */
	    p[4] = 0x71;    /* audio play, mode 2 form 1/2, multisession */
	    p[5] = 0x03;    /* CD-DA commands, accurate */
	    p[6] = 0x29 | (u->prevent ? 0x02 : 0);   /* lock, eject, tray */
	    p[7] = 0x03;    /* separate volume / mute */
	    ww (p + 8, 1764 * 8);
	    ww (p + 10, 256);
	    ww (p + 12, 64);
	    ww (p + 14, 1764 * 8);
	    return 20;
    }
    return 0;
}

static void do_mode_sense (struct cdunit *u, const uae_u8 *cmd, int ten, struct xfer *x)
{
    uae_u8 b[256];
    int page = cmd[2] & 0x3f, pc = cmd[2] >> 6;
    int dbd = cmd[1] & 0x08;
    int hl = ten ? 8 : 4, len, n;

    memset (b, 0, sizeof b);
    len = hl;
    if (!dbd) {
	/* block descriptor */
	uae_u8 *d = b + hl;
	wl (d, u->inserted ? u->img.leadout : 0);
	d[0] = 0;
	wl (d + 4, 2048);
	len += 8;
    }
    if (page == 0x3f) {
	static const int pages[] = { 0x01, 0x0d, 0x0e, 0x2a };
	int i;
	for (i = 0; i < 4; i++)
	    len += mode_page (u, pages[i], pc, b + len);
    } else {
	n = mode_page (u, page, pc, b + len);
	if (!n && page) {
	    ILLEGAL_FIELD ();
	    return;
	}
	len += n;
    }
    if (ten) {
	ww (b, len - 2);
	b[2] = u->inserted ? 0x01 : 0x70;     /* 120 mm data, or no disc */
	ww (b + 6, dbd ? 0 : 8);
    } else {
	b[0] = len - 1;
	b[1] = u->inserted ? 0x01 : 0x70;
	b[3] = dbd ? 0 : 8;
    }
    xfer_put (x, b, len);
}

static void do_mode_select (struct cdunit *u, const uae_u8 *data, int len, int ten)
{
    int hl = ten ? 8 : 4, bdl, p;

    if (!data || len < hl)
	return;
    bdl = ten ? rw (data + 6) : data[3];
    p = hl + bdl;
    while (p + 2 <= len) {
	int page = data[p] & 0x3f, plen = data[p + 1];
	if (page == 0x0e && p + 12 <= len) {
	    u->volume[0] = data[p + 9];
	    u->volume[1] = data[p + 11];
	}
	p += plen + 2;
    }
}

static void do_read_toc (struct cdunit *u, const uae_u8 *cmd, struct xfer *x)
{
    uae_u8 b[4 + 11 * (CD_MAX_TRACKS + 3)];
    struct cdimage *img = &u->img;
    int msf = cmd[1] & 0x02;
    int format = cmd[2] & 0x0f;
    int start = cmd[6];
    int len = 4, i;
    struct cdtrack *first = &img->tracks[0], *last = &img->tracks[img->ntracks - 1];

    if (!format && (cmd[9] & 0xc0))
	format = cmd[9] >> 6;      /* old ATAPI (SFF-8020) format field */

    memset (b, 0, sizeof b);
    switch (format) {
	case 0:
	    if (start > last->number && start != 0xaa) {
		ILLEGAL_FIELD ();
		return;
	    }
	    b[2] = first->number;
	    b[3] = last->number;
	    for (i = 0; i < img->ntracks; i++) {
		struct cdtrack *t = &img->tracks[i];
		if (t->number < start)
		    continue;
		b[len + 1] = ctrl_of (t);
		b[len + 2] = t->number;
		put_addr (b + len + 4, t->start, msf);
		len += 8;
	    }
	    b[len + 1] = ctrl_of (last);
	    b[len + 2] = 0xaa;
	    put_addr (b + len + 4, img->leadout, msf);
	    len += 8;
	    break;
	case 1:     /* session info */
	    b[2] = 1; b[3] = 1;
	    b[len + 1] = ctrl_of (first);
	    b[len + 2] = first->number;
	    put_addr (b + len + 4, first->start, msf);
	    len += 8;
	    break;
	case 2: {   /* full TOC */
	    int disctype = 0x00;
	    for (i = 0; i < img->ntracks; i++)
		if (img->tracks[i].type == TRK_MODE2 || img->tracks[i].type == TRK_MODE2_RAW)
		    disctype = 0x20;
	    b[2] = 1; b[3] = 1;
	    /* A0: first track, A1: last track, A2: lead-out */
	    b[len + 0] = 1; b[len + 1] = ctrl_of (first); b[len + 3] = 0xa0;
	    b[len + 8] = first->number; b[len + 9] = disctype;
	    len += 11;
	    b[len + 0] = 1; b[len + 1] = ctrl_of (last); b[len + 3] = 0xa1;
	    b[len + 8] = last->number;
	    len += 11;
	    b[len + 0] = 1; b[len + 1] = ctrl_of (last); b[len + 3] = 0xa2;
	    lba_to_msf (img->leadout, b + len + 8);
	    len += 11;
	    for (i = 0; i < img->ntracks; i++) {
		struct cdtrack *t = &img->tracks[i];
		b[len + 0] = 1; b[len + 1] = ctrl_of (t); b[len + 3] = t->number;
		lba_to_msf (t->start, b + len + 8);
		len += 11;
	    }
	    break;
	}
	default:
	    ILLEGAL_FIELD ();
	    return;
    }
    ww (b, len - 2);
    xfer_put (x, b, len);
}

static void do_read_subchannel (struct cdunit *u, const uae_u8 *cmd, struct xfer *x)
{
    uae_u8 b[48];
    int msf = cmd[1] & 0x02, subq = cmd[2] & 0x40, format = cmd[3];
    int len = 4;

    memset (b, 0, sizeof b);
    b[1] = u->audio_status;
    if (subq) {
	switch (format) {
	    case 1: {
		int lba = current_lba (u);
		struct cdtrack *t = u->inserted ? find_track (&u->img, lba) : NULL;
		b[4] = 1;
		if (t) {
		    b[5] = ctrl_of (t);
		    b[6] = t->number;
		    b[7] = lba >= t->start ? 1 : 0;
		    put_addr (b + 8, lba, msf);
		    if (msf) {
			int rel = lba - t->start;
			if (rel < 0)
			    rel = -rel;
			b[12] = 0;
			b[13] = rel / (60 * 75);
			b[14] = (rel / 75) % 60;
			b[15] = rel % 75;
		    } else
			wl (b + 12, (uae_u32)(lba - t->start));
		}
		len = 16;
		break;
	    }
	    case 2:     /* media catalog number: none */
	    case 3:     /* ISRC: none */
		b[4] = format;
		if (format == 3)
		    b[6] = cmd[6];
		len = 24;
		break;
	    default:
		ILLEGAL_FIELD ();
		return;
	}
    }
    ww (b + 2, len - 4);
    xfer_put (x, b, len);
    /* "completed" / "error" is reported only once */
    if (u->audio_status == AUDIO_COMPLETED || u->audio_status == AUDIO_ERROR)
	u->audio_status = AUDIO_NONE;
}

/* READ CD / READ CD MSF */
static void do_read_cd (struct cdunit *u, int lba, int count, const uae_u8 *cmd, struct xfer *x)
{
    int exptype = (cmd[1] >> 2) & 7;
    int flags = cmd[9], sub = cmd[10] & 7;
    int i;

    for (i = 0; i < count; i++) {
	struct cdtrack *t;
	uae_u8 *raw = u->secbuf;
	int s = 0, e = 0;

	if (lba + i >= u->img.leadout) {
	    LBA_RANGE ();
	    return;
	}
	t = read_raw (u, lba + i, raw);
	if (!t) {
	    LBA_RANGE ();
	    return;
	}
	if (t->type == TRK_AUDIO) {
	    if (exptype != 0 && exptype != 1) {
		SENSE (5, 0x64, 0x00);
		return;
	    }
	    if (flags & 0xf8) {
		s = 0;
		e = CD_RAW_SECTOR;
	    }
	} else {
	    int uo = user_offset (t, raw);
	    int ue = t->type == TRK_MODE2_RAW ? CD_RAW_SECTOR : uo + 2048;
	    int m2 = uo == 24 || t->type == TRK_MODE2_RAW;
	    if (exptype == 1 || (exptype == 2 && m2) || (exptype >= 3 && !m2)) {
		SENSE (5, 0x64, 0x00);
		return;
	    }
	    s = CD_RAW_SECTOR;
	    e = 0;
	    if (flags & 0x80) { s = 0; e = 12; }
	    if (flags & 0x20) { if (s > 12) s = 12; if (e < 16) e = 16; }
	    if ((flags & 0x40) && uo == 24) { if (s > 16) s = 16; if (e < 24) e = 24; }
	    if (flags & 0x10) { if (s > uo) s = uo; if (e < ue) e = ue; }
	    if (flags & 0x08) { if (s > ue) s = ue; e = CD_RAW_SECTOR; }
	    if (e <= s)
		s = e = 0;
	}
	if (e > s)
	    xfer_put (x, raw + s, e - s);
	if (flags & 0x06) {           /* C2 error information: none */
	    static const uae_u8 z[296];
	    xfer_put (x, z, (flags & 0x06) == 0x04 ? 296 : 294);
	}
	if (sub) {                    /* sub-channel: not stored in the image */
	    static const uae_u8 z[96];
	    xfer_put (x, z, sub == 2 ? 16 : 96);
	}
    }
}

static void do_get_event_status (struct cdunit *u, const uae_u8 *cmd, struct xfer *x)
{
    uae_u8 b[8];
    int classes = cmd[4];

    memset (b, 0, sizeof b);
    if (!(cmd[1] & 1)) {         /* only polled mode */
	ILLEGAL_FIELD ();
	return;
    }
    b[3] = 0x10;                 /* supported: media */
    if (classes & 0x10) {
	ww (b, 6);
	b[2] = 4;                /* media class */
	b[4] = u->media_event;
	b[5] = u->inserted ? 0x02 : 0x00;
	u->media_event = 0;
	xfer_put (x, b, 8);
    } else {
	ww (b, 2);
	b[2] = 0x80;             /* no event available */
	xfer_put (x, b, 4);
    }
}

static void do_get_configuration (struct cdunit *u, const uae_u8 *cmd, struct xfer *x)
{
    uae_u8 b[8 + 12];
    memset (b, 0, sizeof b);
    wl (b, sizeof b - 4);
    ww (b + 6, u->inserted ? 0x0008 : 0x0000);   /* current profile: CD-ROM */
    /* feature 0: profile list */
    ww (b + 8, 0x0000);
    b[10] = 0x03;
    b[11] = 4;
    ww (b + 12, 0x0008);
    b[14] = u->inserted ? 1 : 0;
    xfer_put (x, b, 16);
}

/*
 * Execute one SCSI command. Returns the SCSI status (0 = good,
 * 2 = check condition); sense data is kept in the unit.
 */
static int scsi_command (struct cdunit *u, const uae_u8 *cmd, int cmdlen,
			 const uae_u8 *dataout, int dataoutlen, struct xfer *x)
{
    uae_u8 op = cmd[0];
    uae_u8 b[40];
    int lba, cnt;

    if (op != 0x03) {
	u->sense_key = 0;
	u->asc = u->ascq = 0;
    }
    if (u->unit_attention && op != 0x12 && op != 0x03 && op != 0x4a) {
	u->unit_attention = 0;
	SENSE (6, 0x28, 0x00);           /* medium may have changed */
	return 2;
    }
    if (needs_medium (op) && !u->inserted) {
	NOT_READY ();
	return 2;
    }

    switch (op) {
	case 0x00:      /* TEST UNIT READY */
	    break;
	case 0x03: {    /* REQUEST SENSE */
	    memset (b, 0, 18);
	    b[0] = 0x70;
	    b[2] = u->sense_key;
	    b[7] = 10;
	    b[12] = u->asc;
	    b[13] = u->ascq;
	    xfer_put (x, b, cmd[4] < 18 ? cmd[4] : 18);
	    u->sense_key = 0;
	    u->asc = u->ascq = 0;
	    return 0;
	}
	case 0x12:      /* INQUIRY */
	    do_inquiry (u, cmd, x);
	    break;
	case 0x1a:      /* MODE SENSE (6) */
	    do_mode_sense (u, cmd, 0, x);
	    break;
	case 0x5a:      /* MODE SENSE (10) */
	    do_mode_sense (u, cmd, 1, x);
	    break;
	case 0x15:      /* MODE SELECT (6) */
	    do_mode_select (u, dataout, dataoutlen, 0);
	    break;
	case 0x55:      /* MODE SELECT (10) */
	    do_mode_select (u, dataout, dataoutlen, 1);
	    break;
	case 0x1b:      /* START STOP UNIT */
	    if ((cmd[4] & 0x03) == 0x02) {        /* eject */
		if (u->prevent) {
		    SENSE (5, 0x53, 0x02);        /* medium removal prevented */
		    break;
		}
		if (u->inserted)
		    u->eject_request = 1;
	    } else if ((cmd[4] & 0x03) == 0x00)
		stop_audio (u, AUDIO_NONE);
	    break;
	case 0x1e:      /* PREVENT ALLOW MEDIUM REMOVAL */
	    u->prevent = cmd[4] & 1;
	    break;
	case 0x25:      /* READ CAPACITY */
	    wl (b, u->img.leadout - 1);
	    wl (b + 4, 2048);
	    xfer_put (x, b, 8);
	    break;
	case 0x08:      /* READ (6) */
	case 0x28:      /* READ (10) */
	case 0xa8: {    /* READ (12) */
	    int n, maxblk;
	    if (op == 0x08) {
		lba = ((cmd[1] & 0x1f) << 16) | (cmd[2] << 8) | cmd[3];
		cnt = cmd[4] ? cmd[4] : 256;
	    } else if (op == 0x28) {
		lba = (int)rl (cmd + 2);
		cnt = rw (cmd + 7);
	    } else {
		lba = (int)rl (cmd + 2);
		cnt = (int)rl (cmd + 6);
	    }
	    if (lba < 0 || lba + cnt > u->img.leadout) {
		LBA_RANGE ();
		break;
	    }
	    maxblk = (x->maxlen - x->len) / 2048;
	    if (cnt > maxblk)
		cnt = maxblk;
	    while (cnt > 0) {
		int chunk = cnt > CD_BLOCK_CHUNK ? CD_BLOCK_CHUNK : cnt;
		n = read_blocks (u, lba, chunk, u->blockbuf);
		xfer_put (x, u->blockbuf, n * 2048);
		if (n < chunk)
		    break;
		lba += n;
		cnt -= n;
	    }
	    if (cnt > 0 && u->sense_key)
		break;
	    u->last_lba = lba;
	    break;
	}
	case 0x0b:      /* SEEK (6) */
	case 0x2b:      /* SEEK (10) */
	    lba = op == 0x0b ? (((cmd[1] & 0x1f) << 16) | (cmd[2] << 8) | cmd[3]) : (int)rl (cmd + 2);
	    if (lba >= u->img.leadout)
		LBA_RANGE ();
	    else {
		stop_audio (u, AUDIO_NONE);
		u->last_lba = lba;
	    }
	    break;
	case 0x43:      /* READ TOC */
	    do_read_toc (u, cmd, x);
	    break;
	case 0x42:      /* READ SUB-CHANNEL */
	    do_read_subchannel (u, cmd, x);
	    break;
	case 0x44: {    /* READ HEADER */
	    struct cdtrack *t;
	    lba = (int)rl (cmd + 2);
	    t = find_track (&u->img, lba);
	    if (!t) {
		LBA_RANGE ();
		break;
	    }
	    memset (b, 0, 8);
	    b[0] = t->type == TRK_AUDIO ? 0 : (t->type == TRK_MODE1 ? 1 : 2);
	    put_addr (b + 4, lba, cmd[1] & 0x02);
	    xfer_put (x, b, 8);
	    break;
	}
	case 0x45:      /* PLAY AUDIO (10) */
	case 0xa5: {    /* PLAY AUDIO (12) */
	    uae_u32 l = op == 0x45 ? rw (cmd + 7) : rl (cmd + 6);
	    lba = (int)rl (cmd + 2);
	    if (lba == -1)
		lba = current_lba (u);
	    if (l == 0)
		break;
	    start_play (u, lba, lba + (int)l);
	    break;
	}
	case 0x47: {    /* PLAY AUDIO MSF */
	    int s, e;
	    if (cmd[3] == 0xff && cmd[4] == 0xff && cmd[5] == 0xff)
		s = current_lba (u);
	    else
		s = msf_to_lba (cmd + 3);
	    e = msf_to_lba (cmd + 6);
	    if (e == s)
		break;
	    if (e < s) {
		ILLEGAL_FIELD ();
		break;
	    }
	    start_play (u, s, e);
	    break;
	}
	case 0x48: {    /* PLAY AUDIO TRACK INDEX */
	    struct cdtrack *ts = track_by_number (&u->img, cmd[4]);
	    struct cdtrack *te;
	    int etrk = cmd[7];
	    if (!ts) {
		ILLEGAL_FIELD ();
		break;
	    }
	    if (etrk > u->img.tracks[u->img.ntracks - 1].number)
		etrk = u->img.tracks[u->img.ntracks - 1].number;
	    te = track_by_number (&u->img, etrk);
	    if (!te || te->number < ts->number) {
		ILLEGAL_FIELD ();
		break;
	    }
	    start_play (u, ts->start, te->end);
	    break;
	}
	case 0x4b:      /* PAUSE / RESUME */
	    if (u->audio_status != AUDIO_PLAYING && u->audio_status != AUDIO_PAUSED) {
		SENSE (5, 0x2c, 0x00);    /* command sequence error */
		break;
	    }
	    u->audio_status = (cmd[8] & 1) ? AUDIO_PLAYING : AUDIO_PAUSED;
	    break;
	case 0x4e:      /* STOP PLAY / SCAN */
	case 0x01:      /* REZERO UNIT */
	    stop_audio (u, AUDIO_NONE);
	    break;
	case 0xbe: {    /* READ CD */
	    lba = (int)rl (cmd + 2);
	    cnt = (cmd[6] << 16) | (cmd[7] << 8) | cmd[8];
	    do_read_cd (u, lba, cnt, cmd, x);
	    break;
	}
	case 0xb9: {    /* READ CD MSF */
	    int s = msf_to_lba (cmd + 3), e = msf_to_lba (cmd + 6);
	    if (e < s) {
		ILLEGAL_FIELD ();
		break;
	    }
	    do_read_cd (u, s, e - s, cmd, x);
	    break;
	}
	case 0x4a:      /* GET EVENT STATUS NOTIFICATION */
	    do_get_event_status (u, cmd, x);
	    break;
	case 0x46:      /* GET CONFIGURATION */
	    do_get_configuration (u, cmd, x);
	    break;
	case 0x51: {    /* READ DISC INFORMATION */
	    memset (b, 0, 34);
	    ww (b, 32);
	    b[2] = 0x0e;                 /* complete disc, complete session */
	    b[3] = u->img.tracks[0].number;
	    b[4] = 1;
	    b[5] = u->img.tracks[0].number;
	    b[6] = u->img.tracks[u->img.ntracks - 1].number;
	    b[7] = 0x20;                 /* unrestricted use */
	    xfer_put (x, b, 34);
	    break;
	}
	case 0x2f:      /* VERIFY (10) */
	case 0x35:      /* SYNCHRONIZE CACHE */
	case 0xbb:      /* SET CD SPEED */
	case 0xa7:      /* SET READ AHEAD */
	case 0x16:      /* RESERVE */
	case 0x17:      /* RELEASE */
	    break;
	case 0x1d:      /* SEND DIAGNOSTIC */
	    break;
	case 0x0a:      /* WRITE (6) */
	case 0x2a:      /* WRITE (10) */
	case 0xaa:      /* WRITE (12) */
	    SENSE (7, 0x27, 0x00);       /* write protected */
	    break;
	default:
	    CDLOG (("CDIMAGE: unsupported SCSI command %02x\n", op));
	    ILLEGAL_OPCODE ();
	    break;
    }
    return u->sense_key ? 2 : 0;
}

/* ---------------------------------------------------------------------- */
/* device_functions                                                       */

static struct cdunit *get_unit (int unitnum)
{
    if (unitnum < 0 || unitnum >= MAX_CD_DRIVES || !bus_open)
	return NULL;
    if (!cdunits[unitnum].present)
	return NULL;
    return &cdunits[unitnum];
}

static int cd_open_bus (int flags)
{
    int i;

    if (!bus_open) {
	for (i = 0; i < MAX_CD_DRIVES; i++) {
	    struct cdunit *u = &cdunits[i];
	    memset (u, 0, sizeof *u);
	    cd_lock_init (&u->lock);
	    u->audio_status = AUDIO_NONE;
	    u->volume[0] = u->volume[1] = 0xff;
	    u->cache_lba = -1;
	    {
		int j;
		for (j = 0; j < CD_MAX_FILES; j++)
		    u->img.files[j].fd = -1;
	    }
	}
	bus_open = 1;
    }
    cdimage_reset ();
    return 1;
}

static void cd_close_bus (void)
{
    int i;
    if (!bus_open)
	return;
    for (i = 0; i < MAX_CD_DRIVES; i++) {
	struct cdunit *u = &cdunits[i];
	cd_lock (&u->lock);
	stop_audio (u, AUDIO_NONE);
	image_close (&u->img);
	u->inserted = 0;
	u->path[0] = 0;
	u->present = 0;
	cd_unlock (&u->lock);
    }
}

/* v16: 2048-byte data blocks into a host buffer (built-in CD file system,
 * filesys_cdfs.c). Returns the number of bytes read, 0 without a disc. */
int cdimage_read_host (int unitnum, uae_u32 lba, int count, uae_u8 *buf)
{
    struct cdunit *u = get_unit (unitnum);
    int n;

    if (!u)
	return 0;
    cd_lock (&u->lock);
    if (!u->inserted) {
	cd_unlock (&u->lock);
	return 0;
    }
    {
	/* keep the sense data of the SCSI side */
	uae_u8 sk = u->sense_key, asc = u->asc, ascq = u->ascq;
	n = read_blocks (u, (int) lba, count, buf);
	u->sense_key = sk;
	u->asc = asc;
	u->ascq = ascq;
    }
    cd_unlock (&u->lock);
    return n * 2048;
}

/* changes with every disc change (eject / insert) */
int cdimage_change_count (int unitnum)
{
    struct cdunit *u = get_unit (unitnum);
    return u ? u->changecnt : -1;
}

/* v16: open the image drives without uaescsi.device (ATAPI drive on the
 * IDE controller, gayle.c); applies the configuration like a reset */
void cdimage_start (void)
{
    cd_open_bus (0);
}

/* Bytes the host sends with this command (data-out phase), 0 for none */
int cdimage_dataout_len (const uae_u8 *cmd)
{
    switch (cmd[0]) {
	case 0x15: return cmd[4];                     /* MODE SELECT (6)  */
	case 0x55: return (cmd[7] << 8) | cmd[8];     /* MODE SELECT (10) */
    }
    return 0;
}

/* Largest data-in size of this command (for the transfer buffer) */
int cdimage_datain_max (const uae_u8 *cmd)
{
    uae_u32 n;
    switch (cmd[0]) {
	case 0x08: n = cmd[4] ? cmd[4] : 256; return n * 2048;
	case 0x28: return ((cmd[7] << 8) | cmd[8]) * 2048;
	case 0xa8: n = ((uae_u32) cmd[6] << 24) | (cmd[7] << 16) | (cmd[8] << 8) | cmd[9];
		   return n > 8192 ? 8192 * 2048 : (int) n * 2048;
	case 0xbe: n = (cmd[6] << 16) | (cmd[7] << 8) | cmd[8];
		   return n > 8192 ? 8192 * (CD_RAW_SECTOR + 296 + 96) : (int) n * (CD_RAW_SECTOR + 296 + 96);
	case 0xb9: {
		   int s = msf_to_lba (cmd + 3), e = msf_to_lba (cmd + 6);
		   n = e > s ? (uae_u32) (e - s) : 0;
		   return n > 8192 ? 8192 * (CD_RAW_SECTOR + 296 + 96) : (int) n * (CD_RAW_SECTOR + 296 + 96);
	}
    }
    return 65536;
}

/*
 * v16: one SCSI command from the ATAPI emulation (gayle.c). The sense data
 * stays in the drive until REQUEST SENSE, as on a real ATAPI drive.
 * Returns the SCSI status (0 = good, 2 = check condition).
 */
int cdimage_scsi_cmd (int unitnum, const uae_u8 *cmd, int cmdlen,
		      const uae_u8 *dataout, int dataoutlen,
		      uae_u8 *datain, int datainmax, int *datainlen, uae_u8 *sensekey)
{
    struct cdunit *u = get_unit (unitnum);
    struct xfer x;
    int st;

    *datainlen = 0;
    *sensekey = 0;
    if (!u)
	return -1;
    memset (&x, 0, sizeof x);
    x.host = datain;
    x.maxlen = datainmax;
    cd_lock (&u->lock);
    st = scsi_command (u, cmd, cmdlen, dataout, dataoutlen, &x);
    *sensekey = u->sense_key;
    cd_unlock (&u->lock);
    *datainlen = x.len;
    gui_cd_led (1);
    return st;
}

/* End of the emulation: stop CD audio, close all images (they are opened
 * again by cdimage_reset() at the next start) */
void cdimage_shutdown (void)
{
    cd_close_bus ();
}

static int cd_open_device (int unitnum)
{
    struct cdunit *u = get_unit (unitnum);
    /* v16: a drive on the IDE controller (ATAPI, gayle.c) is not a unit
     * of uaescsi.device */
    if (!u || !u->on_scsi)
	return 0;
    u->opencnt++;
    return 1;
}

static void cd_close_device (int unitnum)
{
    struct cdunit *u = get_unit (unitnum);
    if (u && u->opencnt > 0)
	u->opencnt--;
}

static struct device_info *cd_info (int unitnum, struct device_info *di)
{
    struct cdunit *u = get_unit (unitnum);
    if (!u)
	return NULL;
    memset (di, 0, sizeof *di);
    cd_lock (&u->lock);
    di->type = INQ_ROMD;
    di->media_inserted = u->inserted;
    di->write_protected = 1;
    di->bytespersector = 2048;
    di->cylinders = u->inserted ? u->img.leadout : 0;
    di->trackspercylinder = 1;
    di->sectorspertrack = 1;
    di->bus = 0;
    di->target = unitnum;
    di->lun = 0;
    di->id = unitnum + 1;
    sprintf (di->label, "CD image %d", unitnum);
    cd_unlock (&u->lock);
    return di;
}

static const uae_u8 *cd_exec_out (int unitnum, const uae_u8 *cmd, int cmdlen)
{
    struct cdunit *u = get_unit (unitnum);
    struct xfer x;
    int st;

    if (!u)
	return NULL;
    memset (&x, 0, sizeof x);
    x.host = u->scsibuf;
    x.maxlen = 0;
    cd_lock (&u->lock);
    st = scsi_command (u, cmd, cmdlen, NULL, 0, &x);
    cd_unlock (&u->lock);
    return st == 0 ? cmd : NULL;
}

static const uae_u8 *cd_exec_in (int unitnum, const uae_u8 *cmd, int cmdlen, int *outlen)
{
    struct cdunit *u = get_unit (unitnum);
    struct xfer x;
    int st;

    if (!u)
	return NULL;
    memset (&x, 0, sizeof x);
    x.host = u->scsibuf;
    x.maxlen = DEVICE_SCSI_BUFSIZE;
    cd_lock (&u->lock);
    memset (u->scsibuf, 0, DEVICE_SCSI_BUFSIZE);
    st = scsi_command (u, cmd, cmdlen, NULL, 0, &x);
    cd_unlock (&u->lock);
    if (outlen)
	*outlen = x.len;
    gui_cd_led (1);
    return st == 0 ? u->scsibuf : NULL;
}

#define HFERR_BadStatus 45
#define SCSIF_READ      1
#define SCSIF_AUTOSENSE 2

static int cd_exec_direct (int unitnum, uaecptr acmd)
{
    struct cdunit *u = get_unit (unitnum);
    uaecptr scsi_data    = get_long (acmd + 0);
    uae_u32 scsi_len     = get_long (acmd + 4);
    uaecptr scsi_cmd     = get_long (acmd + 12);
    int     scsi_cmd_len = get_word (acmd + 16);
    uae_u8  scsi_flags   = get_byte (acmd + 20);
    uaecptr scsi_sense   = get_long (acmd + 22);
    uae_u16 scsi_senselen = get_word (acmd + 26);
    uae_u8 cmd[16], *dataout = NULL;
    struct xfer x;
    int i, st, io_error = 0, senseactual = 0;

    if (!u)
	return -1;
    if (scsi_cmd_len < 1 || scsi_cmd_len > 16)
	scsi_cmd_len = scsi_cmd_len < 1 ? 1 : 16;
    memset (cmd, 0, sizeof cmd);
    for (i = 0; i < scsi_cmd_len; i++)
	cmd[i] = get_byte (scsi_cmd + i);
    if (scsi_len && !valid_address (scsi_data, scsi_len))
	return -5;   /* IOERR_BADADDRESS */

    memset (&x, 0, sizeof x);
    x.abuf = scsi_data;
    x.maxlen = (scsi_flags & SCSIF_READ) ? (int)scsi_len : 0;
    if (!(scsi_flags & SCSIF_READ) && scsi_len) {
	dataout = get_real_address (scsi_data);
    }

    cd_lock (&u->lock);
    st = scsi_command (u, cmd, scsi_cmd_len, dataout, dataout ? (int)scsi_len : 0, &x);
    if (st == 2) {
	io_error = HFERR_BadStatus;
	if ((scsi_flags & SCSIF_AUTOSENSE) && scsi_sense && scsi_senselen) {
	    uae_u8 s[18];
	    memset (s, 0, sizeof s);
	    s[0] = 0x70;
	    s[2] = u->sense_key;
	    s[7] = 10;
	    s[12] = u->asc;
	    s[13] = u->ascq;
	    senseactual = scsi_senselen < 18 ? scsi_senselen : 18;
	    for (i = 0; i < senseactual; i++)
		put_byte (scsi_sense + i, s[i]);
	    /* autosense consumes the sense data */
	    u->sense_key = 0;
	    u->asc = u->ascq = 0;
	}
    }
    cd_unlock (&u->lock);

    CDLOG (("CDIMAGE: unit %d cmd %02x len %d -> status %d actual %d\n",
	    unitnum, cmd[0], scsi_len, st, x.len));
    put_long (acmd + 8, (scsi_flags & SCSIF_READ) ? (uae_u32)x.len : (st == 0 ? scsi_len : 0));
    put_word (acmd + 18, scsi_cmd_len);
    put_byte (acmd + 21, st);
    put_word (acmd + 28, senseactual);
    gui_cd_led (1);
    return io_error;
}

static int cd_isatapi (int unitnum)
{
    return 0;
}

struct device_functions devicefunc_cdimage = {
    cd_open_bus, cd_close_bus, cd_open_device, cd_close_device, cd_info,
    cd_exec_out, cd_exec_in, cd_exec_direct,
    0, 0, 0, 0, 0, 0, 0,
    cd_isatapi,
    0, 0
};

/* ---------------------------------------------------------------------- */
/* Direct block read for uaescsi.device CMD_READ / TD_READ64              */

int cdimage_read_blocks (int unitnum, uae_u64 offset, uae_u32 length, uaecptr data, uae_u32 *actual)
{
    struct cdunit *u = get_unit (unitnum);
    uae_u8 *tmp;
    uae_u32 done = 0;
    int err = 0;

    *actual = 0;
    if (!u)
	return 32;           /* TDERR_BadUnitNum */
    cd_lock (&u->lock);
    if (!u->inserted) {
	cd_unlock (&u->lock);
	return 29;           /* TDERR_DiskChanged */
    }
    tmp = u->blockbuf;
    while (done < length) {
	uae_u64 pos = offset + done;
	int lba = (int)(pos / 2048), skip = (int)(pos % 2048);
	int want = (int)((length - done + skip + 2047) / 2048), n, bytes;
	if (want > CD_BLOCK_CHUNK)
	    want = CD_BLOCK_CHUNK;
	n = read_blocks (u, lba, want, tmp);
	if (n <= 0) {
	    err = 20;        /* TDERR_NotSpecified ... no sector */
	    break;
	}
	bytes = n * 2048 - skip;
	if ((uae_u32)bytes > length - done)
	    bytes = length - done;
	if (valid_address (data + done, bytes))
	    memcpy (get_real_address (data + done), tmp + skip, bytes);
	else {
	    int i;
	    for (i = 0; i < bytes; i++)
		put_byte (data + done + i, tmp[skip + i]);
	}
	done += bytes;
	if (n < want)
	    break;
    }
    cd_unlock (&u->lock);
    *actual = done;
    if (!err && done < length)
	err = 20;
    gui_cd_led (1);
    return err;
}

/* ---------------------------------------------------------------------- */
/* Configuration / media change                                           */

/* Apply the configured drives (at reset) */
void cdimage_reset (void)
{
    int i;
    if (!bus_open)
	return;
    for (i = 0; i < MAX_CD_DRIVES; i++) {
	struct cdunit *u = &cdunits[i];
	cd_lock (&u->lock);
	/* v17: a CD32 always has its drive (also without cdimage0=) */
	u->present = currprefs.cddrive[i] || (i == 0 && CD_ON_AKIKO (&currprefs));
	u->on_akiko = u->present && i == 0 && CD_ON_AKIKO (&currprefs);
	u->on_scsi = u->present && !u->on_akiko && currprefs.cd_controller == CD_CTRL_UAESCSI;
	u->insert_delay = 0;
	u->eject_request = 0;
	stop_audio (u, AUDIO_NONE);
	if (!u->present) {
	    if (u->inserted || u->path[0])
		unit_eject (u);
	} else if (strcmp (u->path, currprefs.cdimage[i]) || (currprefs.cdimage[i][0] && !u->inserted)) {
	    if (!unit_insert (u, currprefs.cdimage[i]))
		currprefs.cdimage[i][0] = changed_prefs.cdimage[i][0] = 0;
	}
	u->unit_attention = 0;
	u->media_event = 0;
	u->prevent = 0;
	cd_unlock (&u->lock);
    }
}

int cdimage_unit_present (int unitnum)
{
    return unitnum >= 0 && unitnum < MAX_CD_DRIVES && bus_open && cdunits[unitnum].present;
}

/*
 * Called once per frame from the emulator thread:
 *  - media changes from the GUI (changed_prefs.cdimage[]),
 *  - eject requests from the Amiga side,
 *  - CD-DA position when there is no sound output.
 */
void cdimage_vsync (void)
{
    int i;

    if (!bus_open)
	return;
    for (i = 0; i < MAX_CD_DRIVES; i++) {
	struct cdunit *u = &cdunits[i];
	int changed = 0;

	if (!u->present)
	    continue;
	cd_lock (&u->lock);

	if (u->eject_request) {
	    u->eject_request = 0;
	    unit_eject (u);
	    currprefs.cdimage[i][0] = changed_prefs.cdimage[i][0] = 0;
	    u->insert_delay = 0;
	    changed = 1;
	    gui_filename (4 + i, "");        /* GUI: Media page shows the eject */
	}

	if (strcmp (changed_prefs.cdimage[i], currprefs.cdimage[i])) {
	    strcpy (currprefs.cdimage[i], changed_prefs.cdimage[i]);
	    if (u->inserted) {
		unit_eject (u);
		changed = 1;
	    }
	    if (currprefs.cdimage[i][0]) {
		/* leave the drive empty for a moment, so that the file
		 * system notices the change even when it does not use
		 * the change interrupt */
		strcpy (u->insert_path, currprefs.cdimage[i]);
		u->insert_delay = changed ? (vblank_hz > 0 ? vblank_hz : 50) : 1;
	    } else
		u->insert_delay = 0;
	}

	if (u->insert_delay > 0 && --u->insert_delay == 0) {
	    if (!unit_insert (u, u->insert_path)) {
		write_log ("CDIMAGE: unit %d: '%s' could not be opened\n", i, u->insert_path);
		currprefs.cdimage[i][0] = changed_prefs.cdimage[i][0] = 0;
		gui_filename (4 + i, "");
	    }
	    changed = 1;
	}

	/* CD audio without sound output: advance by time */
	if (u->audio_status == AUDIO_PLAYING) {
	    if (++u->vsyncs_since_mix > 25) {
		int hz = vblank_hz > 0 ? vblank_hz : 50;
		u->play_frame += 44100 / hz;
		if (u->play_frame >= u->play_end) {
		    u->play_frame = u->play_end;
		    stop_audio (u, AUDIO_COMPLETED);
		}
	    }
	}
	cd_unlock (&u->lock);

	if (changed) {
	    gui_cd_led (1);
	    /* uaescsi.device change interrupts; an ATAPI drive reports the
	     * change through unit attention / GET EVENT STATUS only */
	    if (u->on_scsi)
		scsi_do_disk_change (i + 1, u->inserted);
	}
    }
}

/* ---------------------------------------------------------------------- */
/* CD audio                                                               */

/* Fetch one stereo frame (little-endian 16 bit in the image) */
static int audio_frame (struct cdunit *u, uae_u64 frame, int *l, int *r)
{
    int lba = (int)(frame / CD_FRAMES_SECTOR);
    int k = (int)(frame % CD_FRAMES_SECTOR);
    const uae_u8 *p;

    if (u->cache_lba < 0 || lba < u->cache_lba || lba >= u->cache_lba + 2) {
	struct cdtrack *t;
	int j;
	u->cache_lba = lba;
	for (j = 0; j < 2; j++) {
	    t = read_raw (u, lba + j, u->cache + j * CD_RAW_SECTOR);
	    if (!t || t->type != TRK_AUDIO)
		memset (u->cache + j * CD_RAW_SECTOR, 0, CD_RAW_SECTOR);
	}
    }
    p = u->cache + (lba - u->cache_lba) * CD_RAW_SECTOR + k * 4;
    *l = (uae_s16)(p[0] | (p[1] << 8));
    *r = (uae_s16)(p[2] | (p[3] << 8));
    return 1;
}

static void mix_unit (struct cdunit *u, uae_s16 *buf, int frames, int channels, int freq)
{
    uae_u32 step = (uae_u32)(((uae_u64)44100 << 16) / freq);
    int i;

    for (i = 0; i < frames; i++) {
	int l0, r0, l1, r1, l, r;
	uae_u32 f = u->play_frac;

	if (u->play_frame + 1 >= u->play_end) {
	    u->play_frame = u->play_end;
	    stop_audio (u, AUDIO_COMPLETED);
	    break;
	}
	audio_frame (u, u->play_frame, &l0, &r0);
	audio_frame (u, u->play_frame + 1, &l1, &r1);
	l = l0 + (int)(((l1 - l0) * (uae_s64)f) >> 16);
	r = r0 + (int)(((r1 - r0) * (uae_s64)f) >> 16);
	l = l * u->volume[0] / 255;
	r = r * u->volume[1] / 255;

	if (channels == 2) {
	    int a = buf[0] + l, b = buf[1] + r;
	    buf[0] = a > 32767 ? 32767 : (a < -32768 ? -32768 : a);
	    buf[1] = b > 32767 ? 32767 : (b < -32768 ? -32768 : b);
	    buf += 2;
	} else {
	    int a = buf[0] + (l + r) / 2;
	    buf[0] = a > 32767 ? 32767 : (a < -32768 ? -32768 : a);
	    buf++;
	}

	f += step;
	u->play_frame += f >> 16;
	u->play_frac = f & 0xffff;
    }
}

/*
 * Mix playing CD audio into a sound buffer of native 16-bit samples
 * (called from the sound driver before the buffer is sent).
 */
void cdimage_mix (uae_s16 *buf, int samples, int channels, int freq)
{
    int i;

    if (!bus_open || freq <= 0 || channels < 1)
	return;
    for (i = 0; i < MAX_CD_DRIVES; i++) {
	struct cdunit *u = &cdunits[i];
	if (!u->present || u->audio_status != AUDIO_PLAYING)
	    continue;
	cd_lock (&u->lock);
	u->vsyncs_since_mix = 0;
	if (u->audio_status == AUDIO_PLAYING && u->inserted)
	    mix_unit (u, buf, samples / channels, channels, freq);
	cd_unlock (&u->lock);
    }
}

/* ---------------------------------------------------------------------- */
/* v17: high-level access for the CD32 Akiko chip (akiko.c)               */
/*                                                                        */
/* The same operations WinUAE's Akiko code uses (sys_command_cd_*): TOC,  */
/* raw 2352-byte sectors, CD-DA play/pause/stop, Q sub-channel. Called    */
/* from the Akiko thread and the emulator thread; all take the unit lock. */

static struct cdunit *hl_unit (int unitnum)
{
    struct cdunit *u = get_unit (unitnum);
    return u && u->on_akiko ? u : NULL;
}

/* 1 = disc in the drive, 0 = empty, -1 = no drive */
int cdimage_hl_ismedia (int unitnum)
{
    struct cdunit *u = hl_unit (unitnum);
    int r;
    if (!u)
	return -1;
    cd_lock (&u->lock);
    r = u->inserted ? 1 : 0;
    cd_unlock (&u->lock);
    return r;
}

/* TOC in WinUAE's layout: A0, tracks 1..n, A1, A2 (points = n + 3) */
int cdimage_hl_toc (int unitnum, struct cdimage_toc_head *th)
{
    struct cdunit *u = hl_unit (unitnum);
    struct cdimage *img;
    struct cdimage_toc *toc;
    int i, ctrl_mask = 4;

    memset (th, 0, sizeof *th);
    if (!u)
	return 0;
    cd_lock (&u->lock);
    img = &u->img;
    if (!u->inserted || img->ntracks <= 0) {
	cd_unlock (&u->lock);
	return 0;
    }
    th->first_track = img->tracks[0].number;
    th->last_track = img->tracks[img->ntracks - 1].number;
    th->tracks = img->ntracks;
    th->points = img->ntracks + 3;
    th->firstaddress = 0;
    th->lastaddress = img->leadout;
    for (i = 0; i < img->ntracks; i++)
	if (img->tracks[i].type == TRK_AUDIO)
	    ctrl_mask = 0;

    toc = &th->toc[0];
    toc->adr = 1; toc->control = ctrl_mask; toc->point = 0xa0; toc->track = th->first_track;
    toc++;
    th->first_track_offset = 1;
    for (i = 0; i < img->ntracks; i++) {
	struct cdtrack *t = &img->tracks[i];
	toc->adr = 1;
	toc->control = ctrl_of (t) & 0x0f;
	toc->track = t->number;
	toc->point = t->number;
	toc->paddress = t->start;
	toc++;
    }
    th->last_track_offset = img->ntracks;
    toc->adr = 1; toc->control = ctrl_mask; toc->point = 0xa1; toc->track = th->last_track;
    toc->paddress = th->lastaddress;
    toc++;
    toc->adr = 1; toc->control = ctrl_mask; toc->point = 0xa2;
    toc->paddress = th->lastaddress;
    cd_unlock (&u->lock);
    return 1;
}

/* count raw 2352-byte sectors from lsn; 1 = all read */
int cdimage_hl_rawread (int unitnum, uae_u8 *buf, int lsn, int count)
{
    struct cdunit *u = hl_unit (unitnum);
    int i, ok = 1;

    if (!u)
	return 0;
    cd_lock (&u->lock);
    if (!u->inserted)
	ok = 0;
    for (i = 0; ok && i < count; i++) {
	if (!read_raw (u, lsn + i, buf + i * CD_RAW_SECTOR))
	    ok = 0;
    }
    cd_unlock (&u->lock);
    return ok;
}

/* play CD audio from start to end (LSN); 0 = not an audio track */
int cdimage_hl_play (int unitnum, int start, int end)
{
    struct cdunit *u = hl_unit (unitnum);
    int r;

    if (!u)
	return 0;
    cd_lock (&u->lock);
    r = u->inserted ? start_play (u, start, end) : 0;
    if (!r)
	stop_audio (u, AUDIO_ERROR);
    cd_unlock (&u->lock);
    return r;
}

void cdimage_hl_pause (int unitnum, int paused)
{
    struct cdunit *u = hl_unit (unitnum);
    if (!u)
	return;
    cd_lock (&u->lock);
    if (paused && u->audio_status == AUDIO_PLAYING)
	u->audio_status = AUDIO_PAUSED;
    else if (!paused && u->audio_status == AUDIO_PAUSED) {
	u->audio_status = AUDIO_PLAYING;
	u->vsyncs_since_mix = 0;
    }
    cd_unlock (&u->lock);
}

void cdimage_hl_stop (int unitnum)
{
    struct cdunit *u = hl_unit (unitnum);
    if (!u)
	return;
    cd_lock (&u->lock);
    stop_audio (u, AUDIO_NONE);
    cd_unlock (&u->lock);
}

/* 0..255 per channel */
void cdimage_hl_volume (int unitnum, int left, int right)
{
    struct cdunit *u = hl_unit (unitnum);
    if (!u)
	return;
    cd_lock (&u->lock);
    u->volume[0] = left < 0 ? 0 : (left > 255 ? 255 : left);
    u->volume[1] = right < 0 ? 0 : (right > 255 ? 255 : right);
    cd_unlock (&u->lock);
}

/* AUDIO_* status (0x11 playing ... 0x15 none), current position in *lsn */
int cdimage_hl_audio_status (int unitnum, int *lsn)
{
    struct cdunit *u = hl_unit (unitnum);
    int st;
    if (!u)
	return AUDIO_NONE;
    cd_lock (&u->lock);
    st = u->audio_status;
    if (lsn)
	*lsn = current_lba (u);
    cd_unlock (&u->lock);
    return st;
}

static void tolongbcd (uae_u8 *p, int lba)
{
    uae_u8 m[3];
    lba_to_msf (lba, m);
    p[0] = tobcd (m[0]);
    p[1] = tobcd (m[1]);
    p[2] = tobcd (m[2]);
}

/* Q sub-channel (12 bytes) of a sector, as regenerated by WinUAE for
 * images without sub-channel data */
static int make_subq (struct cdunit *u, int lba, uae_u8 *q)
{
    struct cdtrack *t = u->inserted ? find_track (&u->img, lba) : NULL;
    int rel;

    memset (q, 0, 12);
    if (!t)
	return 0;
    q[0] = ctrl_of (t) >> 4 | (ctrl_of (t) & 0x0f) << 4;   /* control << 4 | adr */
    q[1] = tobcd (t->number);
    q[2] = tobcd (lba >= t->start ? 1 : 0);
    rel = lba - t->start;
    if (rel < 0)
	rel = -rel;
    tolongbcd (q + 3, rel - CD_LEADIN);
    tolongbcd (q + 7, lba);
    return 1;
}

/*
 * WinUAE's sys_command_cd_qcode():
 *  buf[0..3]: header, buf[1] = audio status; buf[4..15]: Q sub-channel of
 *  the current position (sector < 0) or of the given sector.
 */
int cdimage_hl_qcode (int unitnum, uae_u8 *buf, int sector)
{
    struct cdunit *u = hl_unit (unitnum);
    int pos;

    memset (buf, 0, 16);
    if (!u)
	return 0;
    cd_lock (&u->lock);
    pos = sector < 0 ? current_lba (u) : sector;
    buf[1] = u->audio_status;
    buf[3] = 12;
    if (!make_subq (u, pos, buf + 4)) {
	cd_unlock (&u->lock);
	return 0;
    }
    cd_unlock (&u->lock);
    return 1;
}

/* 96 bytes of de-interleaved sub-channel data (P..W, 12 bytes each; only
 * Q is set) of a sector */
int cdimage_hl_subchannel (int unitnum, uae_u8 *sub, int sector)
{
    struct cdunit *u = hl_unit (unitnum);
    int r;

    memset (sub, 0, 96);
    if (!u)
	return 0;
    cd_lock (&u->lock);
    r = make_subq (u, sector < 0 ? current_lba (u) : sector, sub + 12);
    cd_unlock (&u->lock);
    return r;
}
