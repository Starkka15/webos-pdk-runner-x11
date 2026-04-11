/*
 * font_redirect.c — ARM LD_PRELOAD shim for webOS PDK runner
 *
 * Intercepts filesystem calls from ARM game binaries and redirects
 * paths under /usr/share/fonts/ to the runner's local fonts/ directory
 * (set via WEBOS_FONT_PATH environment variable).
 *
 * Compile: arm-linux-gnueabi-gcc -shared -fPIC -O2 -o font_redirect.so font_redirect.c -ldl
 *
 * ARM glibc headers use __REDIRECT to alias open→open64, fopen→fopen64, etc.
 * so simply writing "int open(...)" emits the symbol "open64".
 * We use __asm__("symbol") on each prototype to force the exact symbol name
 * the game binary calls.
 */
#define _GNU_SOURCE   /* for RTLD_NEXT */
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <dirent.h>

/*
 * The PDK game binary was compiled with CodeSourcery GCC + old glibc (2.x).
 * Its struct dirent has 32-bit ino_t/off_t → d_name at byte offset 11.
 * ARM glibc 2.34+ uses 64-bit ino64_t/off64_t → d_name at byte offset 19.
 * We return this explicit layout so the game reads filenames correctly.
 */
typedef struct {
    uint32_t d_ino;
    int32_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
} pdk_dirent_t;

/* ---- path remapping ---- */

#define FONTS_PREFIX     "/usr/share/fonts/"
#define FONTS_PREFIX_LEN (sizeof(FONTS_PREFIX) - 1)

static const char *font_dir(void) {
    return getenv("WEBOS_FONT_PATH");
}

static const char *remap(const char *path, char *buf, size_t bufsz) {
    if (path && strncmp(path, FONTS_PREFIX, FONTS_PREFIX_LEN) == 0) {
        const char *dir = font_dir();
        if (dir) {
            const char *name = path + FONTS_PREFIX_LEN;
            const char *slash = strrchr(name, '/');
            if (slash) name = slash + 1;
            snprintf(buf, bufsz, "%s/%s", dir, name);
            fprintf(stderr, "[FREDIR] font: %s -> %s\n", path, buf);
            return buf;
        }
    }
    /* Log all non-font file opens to help debug M3G loading */
    if (path && path[0] != '\0' && getenv("FREDIR_TRACE"))
        fprintf(stderr, "[FREDIR] open: %s\n", path);
    return path;
}

static void log_open_result(const char *path, int flags, int fd) {
    if (!getenv("FREDIR_TRACE")) return;
    if (!path || path[0] == '\0') return;
    /* Only log files that were NOT already logged by remap() */
    if (strncmp(path, FONTS_PREFIX, FONTS_PREFIX_LEN) == 0) return;
    if (fd < 0) {
        fprintf(stderr, "[FREDIR] open FAILED (flags=%x): %s\n", flags, path);
    }
}

/*
 * Declare each wrapper with __asm__("symbol") BEFORE defining it.
 * This forces the compiler to emit the correct symbol name regardless
 * of any __REDIRECT macros in the system headers.
 */

/* open */
int fr_open(const char *path, int flags, ...) __asm__("open");
int fr_open(const char *path, int flags, ...) {
    static int (*real)(const char *, int, ...) = NULL;
    if (!real) real = (int(*)(const char*,int,...))dlsym(RTLD_NEXT, "open");
    const char *orig_path = path;
    char buf[512]; path = remap(path, buf, sizeof(buf));
    int fd;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode_t mode = va_arg(ap, mode_t); va_end(ap);
        fd = real(path, flags, mode);
    } else {
        fd = real(path, flags);
    }
    log_open_result(orig_path, flags, fd);
    return fd;
}

/* open64 */
int fr_open64(const char *path, int flags, ...) __asm__("open64");
int fr_open64(const char *path, int flags, ...) {
    static int (*real)(const char *, int, ...) = NULL;
    if (!real) real = (int(*)(const char*,int,...))dlsym(RTLD_NEXT, "open64");
    char buf[512]; path = remap(path, buf, sizeof(buf));
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode_t mode = va_arg(ap, mode_t); va_end(ap);
        return real(path, flags, mode);
    }
    return real(path, flags);
}

/* fopen */
FILE *fr_fopen(const char *path, const char *mode) __asm__("fopen");
FILE *fr_fopen(const char *path, const char *mode) {
    static FILE *(*real)(const char *, const char *) = NULL;
    if (!real) real = (FILE*(*)(const char*,const char*))dlsym(RTLD_NEXT, "fopen");
    char buf[512]; path = remap(path, buf, sizeof(buf));
    return real(path, mode);
}

/* fopen64 */
FILE *fr_fopen64(const char *path, const char *mode) __asm__("fopen64");
FILE *fr_fopen64(const char *path, const char *mode) {
    static FILE *(*real)(const char *, const char *) = NULL;
    if (!real) real = (FILE*(*)(const char*,const char*))dlsym(RTLD_NEXT, "fopen64");
    char buf[512]; path = remap(path, buf, sizeof(buf));
    return real(path, mode);
}

/* access */
int fr_access(const char *path, int mode) __asm__("access");
int fr_access(const char *path, int mode) {
    static int (*real)(const char *, int) = NULL;
    if (!real) real = (int(*)(const char*,int))dlsym(RTLD_NEXT, "access");
    char buf[512]; path = remap(path, buf, sizeof(buf));
    return real(path, mode);
}

/* openat */
int fr_openat(int dirfd, const char *path, int flags, ...) __asm__("openat");
int fr_openat(int dirfd, const char *path, int flags, ...) {
    static int (*real)(int, const char *, int, ...) = NULL;
    if (!real) real = (int(*)(int,const char*,int,...))dlsym(RTLD_NEXT, "openat");
    char buf[512]; path = remap(path, buf, sizeof(buf));
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode_t mode = va_arg(ap, mode_t); va_end(ap);
        return real(dirfd, path, flags, mode);
    }
    return real(dirfd, path, flags);
}

/* openat64 */
int fr_openat64(int dirfd, const char *path, int flags, ...) __asm__("openat64");
int fr_openat64(int dirfd, const char *path, int flags, ...) {
    static int (*real)(int, const char *, int, ...) = NULL;
    if (!real) real = (int(*)(int,const char*,int,...))dlsym(RTLD_NEXT, "openat64");
    char buf[512]; path = remap(path, buf, sizeof(buf));
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode_t mode = va_arg(ap, mode_t); va_end(ap);
        return real(dirfd, path, flags, mode);
    }
    return real(dirfd, path, flags);
}

/* __xstat — glibc routes stat() through this internally */
int fr_xstat(int ver, const char *path, struct stat *st) __asm__("__xstat");
int fr_xstat(int ver, const char *path, struct stat *st) {
    static int (*real)(int, const char *, struct stat *) = NULL;
    if (!real) real = (int(*)(int,const char*,struct stat*))dlsym(RTLD_NEXT, "__xstat");
    char buf[512]; path = remap(path, buf, sizeof(buf));
    return real(ver, path, st);
}

int fr_lxstat(int ver, const char *path, struct stat *st) __asm__("__lxstat");
int fr_lxstat(int ver, const char *path, struct stat *st) {
    static int (*real)(int, const char *, struct stat *) = NULL;
    if (!real) real = (int(*)(int,const char*,struct stat*))dlsym(RTLD_NEXT, "__lxstat");
    char buf[512]; path = remap(path, buf, sizeof(buf));
    return real(ver, path, st);
}

int fr_xstat64(int ver, const char *path, struct stat *st) __asm__("__xstat64");
int fr_xstat64(int ver, const char *path, struct stat *st) {
    static int (*real)(int, const char *, struct stat *) = NULL;
    if (!real) real = (int(*)(int,const char*,struct stat*))dlsym(RTLD_NEXT, "__xstat64");
    if (!real) return -1;
    char buf[512]; path = remap(path, buf, sizeof(buf));
    return real(ver, path, st);
}

int fr_lxstat64(int ver, const char *path, struct stat *st) __asm__("__lxstat64");
int fr_lxstat64(int ver, const char *path, struct stat *st) {
    static int (*real)(int, const char *, struct stat *) = NULL;
    if (!real) real = (int(*)(int,const char*,struct stat*))dlsym(RTLD_NEXT, "__lxstat64");
    if (!real) return -1;
    char buf[512]; path = remap(path, buf, sizeof(buf));
    return real(ver, path, st);
}

/* opendir — trace directory opens */
DIR *fr_opendir(const char *path) __asm__("opendir");
DIR *fr_opendir(const char *path) {
    static DIR *(*real)(const char *) = NULL;
    if (!real) real = (DIR*(*)(const char*))dlsym(RTLD_NEXT, "opendir");
    if (path && path[0] != '\0' && getenv("FREDIR_TRACE"))
        fprintf(stderr, "[FREDIR] opendir: %s\n", path);
    return real(path);
}

/*
 * readdir — PDK game uses 32-bit struct dirent (d_name at offset 11).
 * ARM glibc 2.39's readdir is broken (returns 0 entries); readdir64 works
 * but returns 64-bit struct dirent64 (d_name at offset 19).
 * We call readdir64 and convert the result into a 32-bit pdk_dirent_t.
 */
pdk_dirent_t *fr_readdir(DIR *dirp) __asm__("readdir");
pdk_dirent_t *fr_readdir(DIR *dirp) {
    static struct dirent64 *(*real)(DIR *) = NULL;
    if (!real) real = (struct dirent64*(*)(DIR*))dlsym(RTLD_NEXT, "readdir64");
    struct dirent64 *ent64 = real(dirp);
    if (!ent64) {
        if (getenv("FREDIR_TRACE"))
            fprintf(stderr, "[FREDIR] readdir: EOF\n");
        return NULL;
    }
    static pdk_dirent_t d32;
    d32.d_ino    = (uint32_t)ent64->d_ino;
    d32.d_off    = (int32_t)ent64->d_off;
    d32.d_reclen = (uint16_t)sizeof(pdk_dirent_t);
    d32.d_type   = ent64->d_type;
    strncpy(d32.d_name, ent64->d_name, sizeof(d32.d_name) - 1);
    d32.d_name[sizeof(d32.d_name) - 1] = '\0';
    if (getenv("FREDIR_TRACE"))
        fprintf(stderr, "[FREDIR] readdir: %s\n", d32.d_name);
    return &d32;
}

/* readdir64 — ARM glibc REDIRECT makes readdir compile as readdir64 */
struct dirent *fr_readdir64(DIR *dirp) __asm__("readdir64");
struct dirent *fr_readdir64(DIR *dirp) {
    static struct dirent *(*real)(DIR *) = NULL;
    if (!real) real = (struct dirent*(*)(DIR*))dlsym(RTLD_NEXT, "readdir64");
    struct dirent *ent = real(dirp);
    if (ent && getenv("FREDIR_TRACE"))
        fprintf(stderr, "[FREDIR] readdir64: %s\n", ent->d_name);
    return ent;
}
