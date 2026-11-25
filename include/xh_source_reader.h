/* Bounded binary source access. This is a reader primitive, not a streaming
 * preprocessor/lexer implementation. No allocation and no read-all operation. */
#ifndef XH_SOURCE_READER_H
#define XH_SOURCE_READER_H
#include <stdint.h>

#define XH_SOURCE_WINDOW_DEFAULT 1024u
#define XH_SOURCE_WINDOW_MAX 4096u
/* OK delivers one byte (which may be NUL). EOF never delivers a byte. */
enum { XH_SOURCE_OK = 1, XH_SOURCE_EOF = 0,
       XH_SOURCE_IO = -1, XH_SOURCE_INVALID = -2, XH_SOURCE_LOOKAHEAD = -3 };
/* Return 0..count bytes or -1. Positive short reads are retried. Returning zero
 * before declared size is reached is an I/O error (e.g. a truncated file).
 * Storage must remain immutable and callbacks must not reenter this reader.
 * callback/context/window are borrowed until close; count is always <=4096. */
typedef int (*XhSourceRange)(void *context, uint32_t offset,
                             unsigned char *buffer, uint32_t count);
typedef struct {
    uint32_t offset, line, column;
    unsigned char previous_cr;
} XhSourceMark;
typedef struct {
    XhSourceRange range;
    void *context;
    const char *path; /* borrowed only by the file adapter; no owned FILE */
    unsigned char *window;
    uint32_t capacity, size, window_offset, window_length;
    uint32_t offset, line, column;
    uint32_t peak_window_bytes; /* payload resident high-water, <=capacity */
    unsigned char previous_cr;
    int error; /* sticky backend failure; reopen/init to recover */
} XhSourceReader;

/* size must fit INT_MAX. No backend reads occur during initialization. */
int xh_source_init(XhSourceRange range, void *context, uint32_t size,
                   unsigned char *window, uint32_t capacity, XhSourceReader *reader);
/* Uses fs_file_size/fs_read_range. path must outlive reader. Range adapter
 * reopens each refill; do not copy/move a live file reader (context points to
 * the reader itself). Persistent FILE/Flash/WASI handles belong in context
 * supplied to init instead. close does not close or free borrowed resources. */
int xh_source_open(const char *path, unsigned char *window, uint32_t capacity,
                   XhSourceReader *reader);
void xh_source_close(XhSourceReader *reader);
/* Absolute byte lookup may evict the cache; never moves offset/line/column.
 * offset==size is EOF, offset>size is INVALID. At most one window resident. */
int xh_source_read_byte(XhSourceReader *reader, uint32_t offset, unsigned char *byte);
/* Relative offset must be <capacity, even at EOF. Does not consume input. */
int xh_source_peek(XhSourceReader *reader, uint32_t lookahead, unsigned char *byte);
/* Consumes one byte; CRLF counts as one newline, lone CR/LF also newline.
 * Columns count bytes (not UTF-8 display cells), starting at 1. */
int xh_source_consume(XhSourceReader *reader, unsigned char *byte);
XhSourceMark xh_source_mark(const XhSourceReader *reader);
/* Restore only a trusted mark made by this reader, still in current window
 * (or same EOF cursor). No I/O or unbounded line rescan; otherwise LOOKAHEAD.
 * Arbitrary historical diagnostics should use absolute lookup + source maps. */
int xh_source_restore(XhSourceReader *reader, XhSourceMark mark);
#endif
