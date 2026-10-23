 /*
  * E-UAE - The portable Amiga Emulator
  *
  * v16: built-in CD file system (filesys_cdfs.c)
  *
  * The CD-ROM image drive can be mounted as a read-only UAE file system
  * unit "CD0" (option cdimage0_boot), so it appears in the boot menu and
  * can be booted from with any Kickstart. The unit's root directory is the
  * virtual path CDFS_ROOT; all host file accesses of filesys.c / fsdb*.c to
  * paths below it are served from the ISO 9660 file system of the image.
  *
  * Include this header AFTER all system headers: it redirects the POSIX
  * calls of the including file to the wrappers below.
  */

#ifndef FILESYS_CDFS_H
#define FILESYS_CDFS_H

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

#define CDFS_ROOT     "@UAE-CD@"
#define CDFS_DEVNAME  "CD0"

struct fs_usage;
struct vdir;
typedef struct vdir vdir_t;

/* mount helpers (filesys.c) */
extern int  cdfs_wanted (void);                       /* CD drive + boot option */
extern void cdfs_volume_name (char *buf, int size);    /* ISO volume id or "CD-ROM" */

/* wrappers */
extern int     cdfs_w_open (const char *path, int flags, ...);
extern ssize_t cdfs_w_read (int fd, void *buf, size_t n);
extern ssize_t cdfs_w_write (int fd, const void *buf, size_t n);
extern off_t   cdfs_w_lseek (int fd, off_t off, int whence);
extern int     cdfs_w_close (int fd);
extern int     cdfs_w_stat (const char *path, struct stat *st);
extern int     cdfs_w_access (const char *path, int mode);
extern int     cdfs_w_truncate (const char *path, off_t len);
extern FILE   *cdfs_w_fopen (const char *path, const char *mode);
extern int     cdfs_w_unlink (const char *path);
extern int     cdfs_w_rmdir (const char *path);
extern int     cdfs_w_mkdir (const char *path, mode_t mode);
extern int     cdfs_w_rename (const char *a, const char *b);
extern vdir_t *cdfs_w_opendir (const char *path);
extern struct dirent *cdfs_w_readdir (vdir_t *d);
extern int     cdfs_w_closedir (vdir_t *d);
extern int     cdfs_w_get_fs_usage (const char *path, const char *disk, struct fs_usage *fsp);

#ifndef FILESYS_CDFS_IMPL
# define open(...)          cdfs_w_open (__VA_ARGS__)
# define read(a,b,c)        cdfs_w_read (a, b, c)
# define write(a,b,c)       cdfs_w_write (a, b, c)
# define lseek(a,b,c)       cdfs_w_lseek (a, b, c)
# define close(a)           cdfs_w_close (a)
# define stat(a,b)          cdfs_w_stat (a, b)
# define access(a,b)        cdfs_w_access (a, b)
# define truncate(a,b)      cdfs_w_truncate (a, b)
# define fopen(a,b)         cdfs_w_fopen (a, b)
# define unlink(a)          cdfs_w_unlink (a)
# define rmdir(a)           cdfs_w_rmdir (a)
# define mkdir(a,b)         cdfs_w_mkdir (a, b)
# define rename(a,b)        cdfs_w_rename (a, b)
# define DIR                vdir_t
# define opendir(a)         cdfs_w_opendir (a)
# define readdir(a)         cdfs_w_readdir (a)
# define closedir(a)        cdfs_w_closedir (a)
# define get_fs_usage(a,b,c) cdfs_w_get_fs_usage (a, b, c)
#endif

#endif
