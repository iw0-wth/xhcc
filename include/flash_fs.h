/* Ark C Compiler - flash_fs.h
 * Filesystem abstraction layer: the compiler reads source code uniformly via fs_read_file.
 * Host uses stdio; STM32 uses W25Q SPI Flash.
 */
#ifndef ARK_FLASH_FS_H
#define ARK_FLASH_FS_H

#include "common.h"
#include <stdint.h>

/* Read the entire file into buf. Returns byte count read; -1 on failure.
 * If file is larger than max_len-1, it is truncated and buf is guaranteed NUL-terminated.
 */
int fs_read_file(const char *path, char *buf, int max_len);

/* Read at most count bytes without loading the entire file or adding NUL.
 * Returns actual bytes (including short/zero EOF), or -1 with fs_last_error.
 * path must be nonempty, buf may be NULL only for count=0. offset/count and
 * offset+count must fit INT_MAX (current <=2 GiB archive/file contract).
 * Bytes beyond the returned count are untouched on successful reads. */
int fs_read_range(const char *path, uint32_t offset, unsigned char *buf, uint32_t count);

/* Size of the file in bytes, or -1 if it cannot be opened / is unknown.
 * Callers use it to tell "the buffer was big enough" from "the file was cut
 * off": fs_read_file() truncates silently, so comparing sizes is the only way
 * to avoid compiling or linking half a file. */
int fs_file_size(const char *path);

/* Write binary data to output file/Flash. Returns bytes written; -1 on failure.
 * path "-" means stdout (host) or callback (STM32).
 * WASI/host files use a same-directory exclusive temporary, then replacement
 * only after write/flush/close succeeds. Failure preserves an existing output;
 * atomic visibility depends on verified filesystem rename support, not crash durability.
 * XHCC_OUTPUT_POLICY unset or atomic-replace uses that platform replacement;
 * no-replace refuses an existing target before rename (EEXIST). no-replace
 * assumes one task exclusively owns output names: check+rename is NOT an atomic
 * concurrent no-replace operation. Use versioned outputs or verified host
 * publication on NuttX/FAT. Unknown/empty policy fails (EINVAL). This policy
 * applies equally to fs_spool_publish; it does not change stdout semantics.
 * Replacement creates a new file with 0666 filtered by umask; existing mode,
 * ownership and hard-link identity are not preserved. stdout cannot roll back.
 */
int fs_write_file(const char *path, const unsigned char *data, int len);

/* Bounded temporary storage used by the file frontend. No source-sized RAM.
 * Internal spools use XHCC_TMPDIR, or the current writable directory by default.
 * Publication spools always share the output directory for atomic rename.
 * create is exclusive (never overwrites a user file); append retries short
 * writes; finish flushes/closes before range reads or publication. Total size
 * is limited to INT_MAX bytes. destroy is required on every exit, including
 * after failures. Disk space must hold the entire preprocessed translation
 * unit in addition to existing outputs; AST/IR memory still grows with code.
 * Host/WASI also cleans active spools on exit(), including fatal arena errors.
 * Abrupt termination (_Exit, a runtime trap, or power loss) cannot run cleanup.
 * The host/WASI adapter implements these operations. */
#define FS_SPOOL_PATH_MAX 1024
typedef struct {
    void *handle;
    uint32_t size;
    uint32_t cursor;
    int finished;
    int failed;
    char path[FS_SPOOL_PATH_MAX];
} FsSpool;
int fs_spool_create(FsSpool *spool, const char *input, const char *publish_path);
int fs_spool_append(FsSpool *spool, const unsigned char *data, uint32_t count);
int fs_spool_finish(FsSpool *spool);
int fs_spool_range(void *spool, uint32_t offset, unsigned char *buf, uint32_t count);
int fs_spool_rewind(FsSpool *spool);
int fs_spool_read(FsSpool *spool, unsigned char *buf, uint32_t count);
int fs_spool_publish(FsSpool *spool, const char *path);
void fs_spool_destroy(FsSpool *spool);

/* Why the most recent fs_read_file / fs_write_file failed, as a short phrase
 * ("No such file or directory"). Never NULL; backends that cannot know the
 * reason return a generic phrase rather than an empty string. Diagnostics
 * print this verbatim, so it must read like the C library's strerror(). */
const char *fs_last_error(void);

/* List all files on the flash FS (STM32 only; prints via ark_log).
 * Returns the number of files found, or -1 if the backing store cannot be
 * listed at all (e.g. the host port, which has no flash FS).
 */
int fs_list_files(void);

#endif /* ARK_FLASH_FS_H */

/* --- RAM-resident files (STM32 port) ----------------------------------
 * The board has to compile sources it produced itself (and feed the
 * resulting objects back into -link) without a flash filesystem, so the
 * port keeps a few flat-name slots in RAM.  Lookups try RAM first, then
 * the flash table, which keeps the flash path working unchanged.
 * fs_ram_put("name", data, -1) stores data as a NUL-terminated string. */
int  fs_ram_put(const char *name, const char *data, int len);
int  fs_ram_count(void);
const char *fs_ram_name_at(int i);
int  fs_ram_size_at(int i);
void fs_ram_clear(void);
