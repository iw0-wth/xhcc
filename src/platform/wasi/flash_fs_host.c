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
#include <stdlib.h>
#include <sys/stat.h>
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
    const char *policy = getenv("XHCC_OUTPUT_POLICY");
    if (policy && strcmp(policy, "atomic-replace")) {
        if (strcmp(policy, "no-replace")) { errno = EINVAL; return -1; }
        /* This check is not atomic rename-noreplace. The deployment must give
         * this compiler exclusive ownership of the output namespace until
         * rename returns. It avoids destructive NuttX/FAT overwrite paths. */
#ifdef _WIN32
        {
            DWORD attrs = GetFileAttributesA(path);
            if (attrs != INVALID_FILE_ATTRIBUTES) { errno = EEXIST; return -1; }
            if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) {
                errno = EACCES; return -1;
            }
        }
#else
        {
            struct stat existing;
            if (!lstat(path, &existing)) { errno = EEXIST; return -1; }
            if (errno != ENOENT) return -1;
        }
#endif
    }
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

/* Entries own their paths and streams, never pointers into frontend stack
 * frames. Memory scales with active spools and actual path lengths only. */
typedef struct SpoolCleanup {
    struct SpoolCleanup *next;
    FILE *stream;
    char path[];
} SpoolCleanup;
static SpoolCleanup *spool_cleanup_list;
static int spool_cleanup_registered;

static void spool_cleanup_exit(void) {
    while (spool_cleanup_list) {
        SpoolCleanup *entry = spool_cleanup_list;
        spool_cleanup_list = entry->next;
        if (entry->stream) fclose(entry->stream);
        remove(entry->path);
        free(entry);
    }
}

static SpoolCleanup *spool_cleanup_find(const char *path) {
    SpoolCleanup *entry;
    for (entry = spool_cleanup_list; entry; entry = entry->next)
        if (!strcmp(entry->path, path)) return entry;
    return NULL;
}

static void spool_cleanup_forget(const char *path) {
    SpoolCleanup **link = &spool_cleanup_list;
    while (*link) {
        SpoolCleanup *entry = *link;
        if (!strcmp(entry->path, path)) {
            *link = entry->next;
            free(entry);
            return;
        }
        link = &entry->next;
    }
}

int fs_spool_create(FsSpool *spool, const char *input, const char *publish_path) {
    const char *directory = getenv("XHCC_TMPDIR");
    char prefix[FS_SPOOL_PATH_MAX];
    unsigned long process = 0;
    int fd = -1, attempt, n, error;
    (void)input;
    if (!spool) { set_error(EINVAL); return -1; }
    memset(spool, 0, sizeof(*spool));
    if (!spool_cleanup_registered) {
        if (atexit(spool_cleanup_exit)) { set_error(ENOMEM); return -1; }
        spool_cleanup_registered = 1;
    }
    if (publish_path) {
        n = snprintf(prefix, sizeof(prefix), "%s", publish_path);
    } else if (directory && *directory) {
        n = snprintf(prefix, sizeof(prefix), "%s/.xhcc-spool", directory);
    } else {
        n = snprintf(prefix, sizeof(prefix), "./.xhcc-spool");
    }
    if (n < 0 || (size_t)n >= sizeof(prefix)) { set_error(ENAMETOOLONG); return -1; }
#ifndef __wasi__
    process = (unsigned long)xh_getpid();
#endif
    for (attempt = 0; attempt < 128; ++attempt) {
        n = snprintf(spool->path, sizeof(spool->path), "%s.xhcc-tmp-%lu-%lu",
                     prefix, process, ++output_sequence);
        if (n < 0 || (size_t)n >= sizeof(spool->path)) {
            spool->path[0] = 0; set_error(ENAMETOOLONG); return -1;
        }
#ifdef _WIN32
        fd = xh_open(spool->path, O_CREAT | O_EXCL | O_WRONLY | O_BINARY, 0666);
#else
        fd = xh_open(spool->path, O_CREAT | O_EXCL | O_WRONLY, 0666);
#endif
        if (fd >= 0) break;
        if (errno != EEXIST) { error = errno ? errno : EIO;
            spool->path[0] = 0; set_error(error); return -1; }
    }
    /* Failed exclusive creation never makes that collision ours to remove. */
    if (fd < 0) { spool->path[0] = 0; set_error(EEXIST); return -1; }
    {
        size_t path_bytes = strlen(spool->path) + 1;
        SpoolCleanup *entry = (SpoolCleanup *)malloc(sizeof(*entry) + path_bytes);
        if (!entry) {
            xh_close(fd); remove(spool->path); spool->path[0] = 0;
            set_error(ENOMEM); return -1;
        }
        memcpy(entry->path, spool->path, path_bytes);
        entry->stream = NULL;
        entry->next = spool_cleanup_list;
        spool_cleanup_list = entry;
    }
    spool->handle = xh_fdopen(fd, "wb");
    if (!spool->handle) {
        error = errno ? errno : EIO; xh_close(fd); remove(spool->path);
        spool_cleanup_forget(spool->path);
        spool->path[0] = 0; set_error(error); return -1;
    }
    spool_cleanup_find(spool->path)->stream = (FILE *)spool->handle;
    return 0;
}

int fs_spool_append(FsSpool *spool, const unsigned char *data, uint32_t count) {
    uint32_t done = 0;
    FILE *fp;
    if (!spool || !spool->handle || spool->finished || spool->failed ||
        (!data && count)) { set_error(EINVAL); return -1; }
    if (count > (uint32_t)INT_MAX - spool->size) {
        spool->failed = 1; set_error(EOVERFLOW); return -1;
    }
    fp = (FILE *)spool->handle;
    while (done < count) {
        size_t n = fwrite(data + done, 1, count - done, fp);
        if (!n || ferror(fp)) {
            spool->failed = 1; set_error(errno ? errno : EIO); return -1;
        }
        done += (uint32_t)n;
    }
    spool->size += count;
    return 0;
}

int fs_spool_finish(FsSpool *spool) {
    int error = 0;
    FILE *fp;
    if (!spool || spool->failed) { set_error(EIO); return -1; }
    if (spool->finished) return 0;
    if (!spool->handle) { set_error(EINVAL); return -1; }
    fp = (FILE *)spool->handle;
    if (fflush(fp)) error = errno ? errno : EIO;
    spool->handle = NULL;
    {
        SpoolCleanup *entry = spool_cleanup_find(spool->path);
        if (entry) entry->stream = NULL;
    }
    if (fclose(fp) && !error) error = errno ? errno : EIO;
    if (error) { spool->failed = 1; set_error(error); return -1; }
    spool->finished = 1;
    return 0;
}

int fs_spool_range(void *context, uint32_t offset, unsigned char *buf, uint32_t count) {
    FsSpool *spool = (FsSpool *)context;
    if (!spool || !spool->finished || spool->failed || offset > spool->size) {
        set_error(EINVAL); return -1;
    }
    if (count > spool->size - offset) count = spool->size - offset;
    return fs_read_range(spool->path, offset, buf, count);
}

int fs_spool_rewind(FsSpool *spool) {
    if (!spool || !spool->finished || spool->failed) { set_error(EINVAL); return -1; }
    spool->cursor = 0;
    return 0;
}

int fs_spool_read(FsSpool *spool, unsigned char *buf, uint32_t count) {
    int n;
    if (!spool) { set_error(EINVAL); return -1; }
    n = fs_spool_range(spool, spool->cursor, buf, count);
    if (n > 0) spool->cursor += (uint32_t)n;
    return n;
}

int fs_spool_publish(FsSpool *spool, const char *path) {
    if (!spool || !path || !*path || !spool->finished || spool->failed || !spool->path[0]) {
        set_error(EINVAL); return -1;
    }
    if (replace_output(spool->path, path)) { set_error(errno ? errno : EIO); return -1; }
    spool_cleanup_forget(spool->path);
    spool->path[0] = 0;
    return 0;
}

void fs_spool_destroy(FsSpool *spool) {
    if (!spool) return;
    if (spool->handle) {
        FILE *fp = (FILE *)spool->handle;
        SpoolCleanup *entry = spool_cleanup_find(spool->path);
        spool->handle = NULL;
        if (entry) entry->stream = NULL;
        fclose(fp);
    }
    if (spool->path[0]) {
        remove(spool->path);
        spool_cleanup_forget(spool->path);
    }
    memset(spool, 0, sizeof(*spool));
}

/* Host-side stub: the host has no flash FS backing store, so a listing is
 * not available. Return -1 (error) rather than 0 (an empty listing) so that
 * callers cannot mistake "not supported" for "no files present". */
int fs_list_files(void) {
    ark_log("[flash_fs] fs_list_files: not implemented on host\r\n");
    return -1;
}
