 /*
  * E-UAE - The portable Amiga Emulator
  *
  * v16: built-in CD file system - the CD-ROM image drive as a read-only
  * UAE file system unit "CD0" (like WinUAE's "CDFS automount").
  *
  * filesys.c, fsdb.c and fsdb_unix.c handle a UAE directory unit with plain
  * POSIX calls on host paths. Including filesys_cdfs.h redirects these calls
  * to the wrappers here: paths below CDFS_ROOT (the root directory of the CD
  * unit) are served from the ISO 9660 file system of the disc in the image
  * drive (blkdev-cdimage.c), everything else goes to the host unchanged.
  *
  * ISO 9660 level 1-3 with Rock Ridge names (NM) when present; directories
  * are read from the disc on every lookup (the image file is cached by the
  * host). Files above 4 GB (multi-extent) are not supported.
  */

#define FILESYS_CDFS_IMPL

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "fsusage.h"
#include "filesys_cdfs.h"
#ifdef SCSIEMU
#include "cdimage.h"
#else
/* built without the CD-ROM image drive: no disc, the unit is never mounted */
static int cdimage_read_host (int unitnum, uae_u32 lba, int count, uae_u8 *buf) { return 0; }
static int cdimage_change_count (int unitnum) { return -1; }
#endif

#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifndef O_ACCMODE
#define O_ACCMODE 3
#endif
#ifndef EROFS
#define EROFS EACCES
#endif

#define CDFS_UNIT      0
#define CDFS_FD_BASE   0x7ff00000
#define CDFS_MAX_FILES 64

struct iso_entry {
    uae_u32 extent;          /* first block */
    uae_u32 size;            /* bytes */
    int     isdir;
    time_t  mtime;
    char    name[256];
};

struct cdfs_file {
    int     used;
    uae_u32 extent, size, pos;
    int     changecnt;
};

struct vdir {
    DIR    *host;            /* host directory, or NULL for the CD */
    struct iso_entry dir;    /* CD directory */
    uae_u32 off;             /* byte offset in the directory extent */
    int     changecnt;
    struct dirent de;
};

static struct cdfs_file cdfiles[CDFS_MAX_FILES];

/* volume descriptor cache */
static int     vol_changecnt = -2;
static int     vol_ok;
static struct iso_entry vol_root;
static char    vol_name[33];

/* ---------------------------------------------------------------------- */

int cdfs_wanted (void)
{
#ifdef SCSIEMU
    /* v17: a CD32 boots its CD itself (extended ROM) */
    return currprefs.cddrive[0] && currprefs.cd_boot && !CD_ON_AKIKO (&currprefs);
#else
    return 0;
#endif
}

static int is_cd_path (const char *p)
{
    return p && !strncmp (p, CDFS_ROOT, sizeof CDFS_ROOT - 1);
}

static uae_u32 le32 (const uae_u8 *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uae_u32) p[3] << 24);
}

static time_t iso_time (const uae_u8 *d)
{
    /* 7-byte date: years since 1900, month, day, hour, minute, second, offset */
    struct tm tm;
    int days;
    static const int mdays[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    int y = d[0] + 1900, m = d[1] ? d[1] - 1 : 0;

    memset (&tm, 0, sizeof tm);
    if (m > 11)
	m = 11;
    /* days since 1970 without timegm() (not in every libc) */
    days = (y - 1970) * 365 + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400
	   + mdays[m] + (d[2] ? d[2] - 1 : 0);
    if (m > 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
	days++;
    return (time_t) days * 86400 + d[3] * 3600 + d[4] * 60 + d[5] - (signed char) d[6] * 15 * 60;
}

/* Directory record -> entry. Returns record length or 0 at the end of a block. */
static int parse_record (const uae_u8 *r, int avail, struct iso_entry *e)
{
    int len = r[0], nlen, su, i;
    char rr[256];
    int rrlen = 0, have_rr = 0;

    if (len == 0 || len > avail || len < 34)
	return 0;
    nlen = r[32];
    e->extent = le32 (r + 2);
    e->size = le32 (r + 10);
    e->isdir = (r[25] & 2) != 0;
    e->mtime = iso_time (r + 18);

    /* Rock Ridge alternate name (SUSP "NM" entries after the name) */
    su = 33 + nlen + ((nlen & 1) ? 0 : 1);
    while (su + 4 <= len) {
	const uae_u8 *p = r + su;
	int elen = p[2];
	if (elen < 4 || su + elen > len)
	    break;
	if (p[0] == 'N' && p[1] == 'M' && elen > 5 && !(p[4] & 6)) {
	    for (i = 5; i < elen && rrlen < 255; i++)
		rr[rrlen++] = p[i];
	    have_rr = 1;
	}
	su += elen;
    }
    if (have_rr && rrlen > 0) {
	memcpy (e->name, rr, rrlen);
	e->name[rrlen] = 0;
    } else if (nlen == 1 && (r[33] == 0 || r[33] == 1)) {
	strcpy (e->name, r[33] ? ".." : ".");
    } else {
	/* ISO name: without ";version" and without a trailing dot */
	for (i = 0; i < nlen && i < 255 && r[33 + i] != ';'; i++)
	    e->name[i] = r[33 + i];
	e->name[i] = 0;
	if (i > 0 && e->name[i - 1] == '.')
	    e->name[i - 1] = 0;
    }
    return len;
}

/* block buffers: static, the file system threads have small stacks; only
 * the CD unit's thread uses them once the unit runs */
static uae_u8 vol_buf[2048], dir_buf[2048], file_buf[2048];

static int load_volume (void)
{
    int cc = cdimage_change_count (CDFS_UNIT);
    uae_u8 *buf = vol_buf;
    int s, i;

    if (cc == vol_changecnt)
	return vol_ok;
    vol_changecnt = cc;
    vol_ok = 0;
    strcpy (vol_name, "CD-ROM");
    memset (&vol_root, 0, sizeof vol_root);
    vol_root.isdir = 1;
    for (s = 16; s < 32; s++) {
	if (cdimage_read_host (CDFS_UNIT, s, 1, buf) != 2048)
	    return 0;
	if (memcmp (buf + 1, "CD001", 5))
	    return 0;
	if (buf[0] == 255)
	    return 0;
	if (buf[0] == 1) {
	    parse_record (buf + 156, 34, &vol_root);
	    vol_root.isdir = 1;
	    memcpy (vol_name, buf + 40, 32);
	    vol_name[32] = 0;
	    for (i = 31; i >= 0 && (vol_name[i] == ' ' || vol_name[i] == 0); i--)
		vol_name[i] = 0;
	    if (!vol_name[0])
		strcpy (vol_name, "CD-ROM");
	    vol_ok = 1;
	    return 1;
	}
    }
    return 0;
}

void cdfs_volume_name (char *buf, int size)
{
    load_volume ();
    strncpy (buf, vol_name, size - 1);
    buf[size - 1] = 0;
}

/* Next entry of a directory, *off advanced; returns 0 at the end */
static int dir_next (const struct iso_entry *dir, uae_u32 *off, struct iso_entry *e)
{
    uae_u8 *buf = dir_buf;

    while (*off < dir->size) {
	uae_u32 blk = dir->extent + *off / 2048;
	int bo = *off % 2048, n;

	if (cdimage_read_host (CDFS_UNIT, blk, 1, buf) != 2048)
	    return 0;
	n = parse_record (buf + bo, 2048 - bo, e);
	if (n == 0) {
	    /* rest of the block is padding */
	    *off = (*off / 2048 + 1) * 2048;
	    continue;
	}
	*off += n;
	if (!strcmp (e->name, ".") || !strcmp (e->name, ".."))
	    continue;
	return 1;
    }
    return 0;
}

/* Path below CDFS_ROOT -> entry (case-insensitive, like AmigaDOS) */
static int lookup (const char *path, struct iso_entry *out)
{
    const char *p = path + sizeof CDFS_ROOT - 1;
    struct iso_entry cur;

    if (!load_volume ()) {
	/* no disc: an empty root directory */
	if (*p == 0 || (*p == '/' && p[1] == 0)) {
	    memset (out, 0, sizeof *out);
	    out->isdir = 1;
	    return 1;
	}
	return 0;
    }
    cur = vol_root;
    while (*p) {
	char comp[256];
	int n = 0;
	uae_u32 off = 0;
	struct iso_entry e;
	int found = 0;

	while (*p == '/' || *p == ':')
	    p++;
	while (*p && *p != '/' && n < 255)
	    comp[n++] = *p++;
	comp[n] = 0;
	if (!n)
	    break;
	if (!cur.isdir)
	    return 0;
	while (dir_next (&cur, &off, &e)) {
	    if (!strcasecmp (e.name, comp)) {
		found = 1;
		break;
	    }
	}
	if (!found)
	    return 0;
	cur = e;
    }
    *out = cur;
    return 1;
}

/* ---------------------------------------------------------------------- */
/* Wrappers                                                               */

int cdfs_w_open (const char *path, int flags, ...)
{
    int mode = 0, i;
    struct iso_entry e;

    if (flags & O_CREAT) {
	va_list ap;
	va_start (ap, flags);
	mode = va_arg (ap, int);
	va_end (ap);
    }
    if (!is_cd_path (path))
	return open (path, flags, mode);
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC))) {
	errno = EROFS;
	return -1;
    }
    if (!lookup (path, &e)) {
	errno = ENOENT;
	return -1;
    }
    if (e.isdir) {
	errno = EISDIR;
	return -1;
    }
    for (i = 0; i < CDFS_MAX_FILES; i++) {
	if (!cdfiles[i].used) {
	    cdfiles[i].used = 1;
	    cdfiles[i].extent = e.extent;
	    cdfiles[i].size = e.size;
	    cdfiles[i].pos = 0;
	    cdfiles[i].changecnt = cdimage_change_count (CDFS_UNIT);
	    return CDFS_FD_BASE + i;
	}
    }
    errno = EMFILE;
    return -1;
}

static struct cdfs_file *cd_fd (int fd)
{
    if (fd >= CDFS_FD_BASE && fd < CDFS_FD_BASE + CDFS_MAX_FILES && cdfiles[fd - CDFS_FD_BASE].used)
	return &cdfiles[fd - CDFS_FD_BASE];
    return NULL;
}

ssize_t cdfs_w_read (int fd, void *buf, size_t n)
{
    struct cdfs_file *f = cd_fd (fd);
    uae_u8 *out = (uae_u8 *) buf;
    size_t done = 0;
    uae_u8 *blk = file_buf;

    if (fd < CDFS_FD_BASE)
	return read (fd, buf, n);
    if (!f) {
	errno = EBADF;
	return -1;
    }
    if (f->changecnt != cdimage_change_count (CDFS_UNIT)) {
	errno = EIO;            /* the disc was changed */
	return -1;
    }
    if (f->pos >= f->size)
	return 0;
    if (n > f->size - f->pos)
	n = f->size - f->pos;
    while (done < n) {
	uae_u32 lba = f->extent + f->pos / 2048;
	int bo = f->pos % 2048, len;
	if (bo == 0 && n - done >= 2048) {
	    /* whole blocks straight into the buffer */
	    int cnt = (int) ((n - done) / 2048), got;
	    if (cnt > 64)
		cnt = 64;
	    got = cdimage_read_host (CDFS_UNIT, lba, cnt, out + done);
	    if (got <= 0)
		break;
	    done += got;
	    f->pos += got;
	    if (got < cnt * 2048)
		break;
	    continue;
	}
	if (cdimage_read_host (CDFS_UNIT, lba, 1, blk) != 2048)
	    break;
	len = 2048 - bo;
	if ((size_t) len > n - done)
	    len = (int) (n - done);
	memcpy (out + done, blk + bo, len);
	done += len;
	f->pos += len;
    }
    if (done == 0 && n > 0) {
	errno = EIO;
	return -1;
    }
    return (ssize_t) done;
}

ssize_t cdfs_w_write (int fd, const void *buf, size_t n)
{
    if (fd < CDFS_FD_BASE)
	return write (fd, buf, n);
    errno = EROFS;
    return -1;
}

off_t cdfs_w_lseek (int fd, off_t off, int whence)
{
    struct cdfs_file *f = cd_fd (fd);
    long long np;

    if (fd < CDFS_FD_BASE)
	return lseek (fd, off, whence);
    if (!f) {
	errno = EBADF;
	return -1;
    }
    switch (whence) {
	case SEEK_SET: np = off; break;
	case SEEK_CUR: np = (long long) f->pos + off; break;
	case SEEK_END: np = (long long) f->size + off; break;
	default: errno = EINVAL; return -1;
    }
    if (np < 0) {
	errno = EINVAL;
	return -1;
    }
    f->pos = (uae_u32) np;
    return (off_t) np;
}

int cdfs_w_close (int fd)
{
    struct cdfs_file *f = cd_fd (fd);
    if (fd < CDFS_FD_BASE)
	return close (fd);
    if (!f) {
	errno = EBADF;
	return -1;
    }
    f->used = 0;
    return 0;
}

int cdfs_w_stat (const char *path, struct stat *st)
{
    struct iso_entry e;

    if (!is_cd_path (path))
	return stat (path, st);
    if (!lookup (path, &e)) {
	errno = ENOENT;
	return -1;
    }
    memset (st, 0, sizeof *st);
    st->st_mode = (e.isdir ? S_IFDIR : S_IFREG) | 0555;
    st->st_size = e.isdir ? 0 : e.size;
    st->st_mtime = st->st_ctime = st->st_atime = e.mtime;
    st->st_nlink = 1;
    return 0;
}

int cdfs_w_access (const char *path, int mode)
{
    struct iso_entry e;

    if (!is_cd_path (path))
	return access (path, mode);
    if (mode & W_OK) {
	errno = EROFS;
	return -1;
    }
    if (!lookup (path, &e)) {
	errno = ENOENT;
	return -1;
    }
    return 0;
}

int cdfs_w_truncate (const char *path, off_t len)
{
    if (!is_cd_path (path))
	return truncate (path, len);
    errno = EROFS;
    return -1;
}

FILE *cdfs_w_fopen (const char *path, const char *mode)
{
    if (!is_cd_path (path))
	return fopen (path, mode);
    /* no fsdb files (UAE attribute databases) on the CD */
    errno = mode[0] == 'r' && !strchr (mode, '+') ? ENOENT : EROFS;
    return NULL;
}

int cdfs_w_unlink (const char *path)
{
    if (!is_cd_path (path))
	return unlink (path);
    errno = EROFS;
    return -1;
}

int cdfs_w_rmdir (const char *path)
{
    if (!is_cd_path (path))
	return rmdir (path);
    errno = EROFS;
    return -1;
}

int cdfs_w_mkdir (const char *path, mode_t mode)
{
    if (!is_cd_path (path))
	return mkdir (path, mode);
    errno = EROFS;
    return -1;
}

int cdfs_w_rename (const char *a, const char *b)
{
    if (!is_cd_path (a) && !is_cd_path (b))
	return rename (a, b);
    errno = EROFS;
    return -1;
}

vdir_t *cdfs_w_opendir (const char *path)
{
    vdir_t *d;
    struct iso_entry e;

    if (!is_cd_path (path)) {
	DIR *h = opendir (path);
	if (!h)
	    return NULL;
	d = (vdir_t *) calloc (1, sizeof *d);
	if (!d) {
	    closedir (h);
	    return NULL;
	}
	d->host = h;
	return d;
    }
    if (!lookup (path, &e) || !e.isdir) {
	errno = ENOENT;
	return NULL;
    }
    d = (vdir_t *) calloc (1, sizeof *d);
    if (!d)
	return NULL;
    d->dir = e;
    d->off = 0;
    d->changecnt = cdimage_change_count (CDFS_UNIT);
    return d;
}

struct dirent *cdfs_w_readdir (vdir_t *d)
{
    struct iso_entry e;

    if (!d)
	return NULL;
    if (d->host)
	return readdir (d->host);
    if (d->changecnt != cdimage_change_count (CDFS_UNIT) || !vol_ok)
	return NULL;
    if (!dir_next (&d->dir, &d->off, &e))
	return NULL;
    strncpy (d->de.d_name, e.name, sizeof d->de.d_name - 1);
    d->de.d_name[sizeof d->de.d_name - 1] = 0;
    return &d->de;
}

int cdfs_w_closedir (vdir_t *d)
{
    int r = 0;
    if (!d)
	return -1;
    if (d->host)
	r = closedir (d->host);
    free (d);
    return r;
}

int cdfs_w_get_fs_usage (const char *path, const char *disk, struct fs_usage *fsp)
{
    if (!is_cd_path (path))
	return get_fs_usage (path, disk, fsp);
    memset (fsp, 0, sizeof *fsp);
    /* blocks of 512 bytes, as get_fs_usage() reports them: the volume space
     * size of the primary volume descriptor */
    if (load_volume () && cdimage_read_host (CDFS_UNIT, 16, 1, vol_buf) == 2048)
	fsp->fsu_blocks = (long) le32 (vol_buf + 80) * 4;
    fsp->fsu_bfree = fsp->fsu_bavail = 0;
    return 0;
}
