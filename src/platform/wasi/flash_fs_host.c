/* Ark C Compiler - flash_fs_host
 * Host-side stdio-based file read implementation.
 */
#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif
#include "flash_fs.h"
#include "platform.h"
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <limits.h>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <windows.h>
#define xh_open _open
#define xh_close _close
#define xh_fdopen _fdopen
#define xh_getpid _getpid
#else
#include <unistd.h>
#define xh_open open
#define xh_close close
#define xh_fdopen fdopen
#ifndef __wasi__
#define xh_getpid getpid
#endif
#endif

static unsigned long output_sequence;

/* Rename provides visibility atomicity on supporting filesystems; no fsync or
 * crash/power-loss durability guarantee is made. Never unlink the destination. */
static int replace_output(const char *temporary, const char *path) {
#ifdef _WIN32
    if (MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING)) return 0;
    /* Preserve a useful errno contract without adding a second filesystem op. */
    errno = EIO;
    return -1;
#else
    return rename(temporary, path);
#endif
}

/* Reason text of the last failed operation; see flash_fs.h. */
static char g_fs_error[128];

static void set_error(int err) {
    const char *s = strerror(err);
    int i;
    if (!s || !s[0]) s = "I/O error";
    for (i = 0; i < (int)sizeof(g_fs_error) - 1 && s[i]; i++) g_fs_error[i] = s[i];
    g_fs_error[i] = 0;
}

const char *fs_last_error(void) {
    return g_fs_error[0] ? g_fs_error : "I/O error";
}

int fs_read_file(const char *path, char *buf, int max_len) {
    FILE *fp;
    int n;
    g_fs_error[0] = 0;
    if (!path || !buf || max_len <= 0) return -1;
    fp = fopen(path, "rb");
    if (!fp) { set_error(errno); return -1; }
    n = (int)fread(buf, 1, (size_t)(max_len - 1), fp);
    if (ferror(fp)) { set_error(errno ? errno : EIO); fclose(fp); return -1; }
    fclose(fp);
    buf[n] = '\0';
    return n;
}

int fs_read_range(const char *path, uint32_t offset, unsigned char *buf, uint32_t count) {
    FILE *fp;
    size_t n;
    int error = 0;
    g_fs_error[0] = 0;
    if (!path || !*path || (!buf && count)) { set_error(EINVAL); return -1; }
    /* Keep the bounded file API consistent on Windows/WASI long32 and hosts. */
    if (offset > (uint32_t)INT_MAX || count > (uint32_t)INT_MAX ||
        count > (uint32_t)INT_MAX - offset) { set_error(EOVERFLOW); return -1; }
    fp = fopen(path, "rb");
    if (!fp) { set_error(errno ? errno : EIO); return -1; }
    if (fseek(fp, (long)offset, SEEK_SET)) {
        error = errno ? errno : EIO;
        fclose(fp); set_error(error); return -1;
    }
    n = count ? fread(buf, 1, (size_t)count, fp) : 0;
    if (ferror(fp)) error = errno ? errno : EIO;
    if (fclose(fp) && !error) error = errno ? errno : EIO;
    if (error) { set_error(error); return -1; }
    return (int)n;
}

int fs_file_size(const char *path) {
    FILE *fp;
    long sz;
    g_fs_error[0] = 0;
    if (!path || !path[0]) return -1;
    fp = fopen(path, "rb");
    if (!fp) { set_error(errno); return -1; }
    if (fseek(fp, 0, SEEK_END) != 0) { set_error(errno); fclose(fp); return -1; }
    sz = ftell(fp);
    fclose(fp);
    if (sz < 0) { set_error(errno); return -1; }
    if (sz > INT_MAX) { set_error(EOVERFLOW); return -1; }
    return (int)sz;
}

int fs_write_file(const char *path, const unsigned char *data, int len) {
    int wr;
    g_fs_error[0] = 0;
    if (!path || !data || len < 0) return -1;
    /* "-" means stdout (default behavior of -E / -S) */
    if (path[0] == '-' && path[1] == 0) {
        wr = (int)fwrite(data, 1, (size_t)len, stdout);
        if (fflush(stdout) || wr != len) { set_error(errno ? errno : EIO); return -1; }
        return wr;
    }
    {
        char temporary[1024];
        unsigned long process = 0;
        int fd = -1, attempt, error = 0;
        FILE *fp;
#ifndef __wasi__
        process = (unsigned long)xh_getpid();
#endif
        /* O_EXCL is the authority for uniqueness, including concurrent writers.
         * Appending to the destination keeps both files on the same directory. */
        for (attempt = 0; attempt < 128; attempt++) {
            int n = snprintf(temporary, sizeof(temporary), "%s.xhcc-tmp-%lu-%lu",
                             path, process, ++output_sequence);
            if (n < 0 || (size_t)n >= sizeof(temporary)) {
                set_error(ENAMETOOLONG); return -1;
            }
#ifdef _WIN32
            fd = xh_open(temporary, O_CREAT | O_EXCL | O_WRONLY | O_BINARY, 0666);
#else
            fd = xh_open(temporary, O_CREAT | O_EXCL | O_WRONLY, 0666);
#endif
            if (fd >= 0) break;
            if (errno != EEXIST) { set_error(errno ? errno : EIO); return -1; }
        }
        if (fd < 0) { set_error(EEXIST); return -1; }
        fp = xh_fdopen(fd, "wb");
        if (!fp) {
            error = errno ? errno : EIO;
            xh_close(fd); remove(temporary); set_error(error); return -1;
        }
        wr = 0;
        while (wr < len) {
            size_t n = fwrite(data + wr, 1, (size_t)(len - wr), fp);
            if (!n || ferror(fp)) { error = errno ? errno : EIO; break; }
            wr += (int)n;
        }
        if (!error && fflush(fp)) error = errno ? errno : EIO;
        /* Always close, even after an earlier failure; preserve first error. */
        if (fclose(fp) && !error) error = errno ? errno : EIO;
        if (!error && replace_output(temporary, path)) error = errno ? errno : EIO;
        if (error) { remove(temporary); set_error(error); return -1; }
        return len;
    }
}

/* Host-side stub: the host has no flash FS backing store, so a listing is
 * not available. Return -1 (error) rather than 0 (an empty listing) so that
 * callers cannot mistake "not supported" for "no files present". */
int fs_list_files(void) {
    ark_log("[flash_fs] fs_list_files: not implemented on host\r\n");
    return -1;
}
