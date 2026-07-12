/* fs.h -- tiny directory-listing helper for the file-browser dialog.
 *
 * The application is pure C89, but a usable file picker needs POSIX opendir /
 * stat / getcwd.  Rather than pull feature-test macros into the C89 unit, this
 * lives in its own translation unit (fs.c) compiled with a relaxed standard,
 * exposing a minimal C API -- the same split the project uses for stbiw.c.
 */
#ifndef OV_FS_H
#define OV_FS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { char name[256]; int isDir; } FSEntry;

/* List entries of 'dir' into out[0..maxOut-1].  Directories are listed first,
 * then files whose name ends with 'ext' (case-insensitive; ext NULL/"" matches
 * all).  Hidden dotfiles are skipped except the ".." parent.  Returns the
 * number of entries written. */
int  fs_list( const char *dir, const char *ext, FSEntry *out, int maxOut );

/* Current working directory into buf (falls back to "." on failure). */
void fs_cwd( char *buf, int buflen );

/* 1 if 'path' names a directory. */
int  fs_is_dir( const char *path );

/* Build a clean path from 'dir' and 'child' into buf.  child "" appends
 * nothing; ".." strips the last component; an absolute child replaces dir. */
void fs_join( const char *dir, const char *child, char *buf, int buflen );

#ifdef __cplusplus
}
#endif

#endif /* OV_FS_H */
