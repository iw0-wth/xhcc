#include "xh_source_reader.h"
#include "flash_fs.h"
#include <limits.h>
#include <stddef.h>

static int file_range(void *context, uint32_t offset, unsigned char *buffer,
                      uint32_t count) {
    XhSourceReader *reader = (XhSourceReader *)context;
    return fs_read_range(reader->path, offset, buffer, count);
}

void xh_source_close(XhSourceReader *reader) {
    if (reader) {
        reader->range = NULL; reader->context = NULL; reader->path = NULL;
        reader->window = NULL; reader->capacity = reader->size = 0;
        reader->window_offset = reader->window_length = reader->offset = 0;
        reader->line = reader->column = 1; reader->previous_cr = 0;
        reader->peak_window_bytes = 0; reader->error = XH_SOURCE_INVALID;
    }
}

int xh_source_init(XhSourceRange range, void *context, uint32_t size,
                   unsigned char *window, uint32_t capacity, XhSourceReader *reader) {
    if (!reader) return XH_SOURCE_INVALID;
    xh_source_close(reader);
    if (!range || !window || !capacity || capacity > XH_SOURCE_WINDOW_MAX ||
        size > (uint32_t)INT_MAX) return XH_SOURCE_INVALID;
    reader->range = range; reader->context = context; reader->size = size;
    reader->window = window; reader->capacity = capacity; reader->error = 0;
    return XH_SOURCE_OK;
}

int xh_source_open(const char *path, unsigned char *window, uint32_t capacity,
                   XhSourceReader *reader) {
    int size, result;
    if (!reader) return XH_SOURCE_INVALID;
    xh_source_close(reader);
    if (!path || !*path || !window || !capacity || capacity > XH_SOURCE_WINDOW_MAX)
        return XH_SOURCE_INVALID;
    size = fs_file_size(path);
    if (size < 0) { reader->error = XH_SOURCE_IO; return XH_SOURCE_IO; }
    result = xh_source_init(file_range, reader, (uint32_t)size, window, capacity, reader);
    if (result == XH_SOURCE_OK) reader->path = path;
    return result;
}

static int refill(XhSourceReader *reader, uint32_t offset) {
    uint32_t wanted = reader->size - offset, filled = 0;
    if (wanted > reader->capacity) wanted = reader->capacity;
    reader->window_length = 0;
    reader->window_offset = offset;
    while (filled < wanted) {
        uint32_t remaining = wanted - filled;
        int got = reader->range(reader->context, offset + filled,
                                reader->window + filled, remaining);
        /* Never trust count before validating it, including buggy callbacks. */
        if (got <= 0 || (uint32_t)got > remaining) {
            reader->error = XH_SOURCE_IO;
            return XH_SOURCE_IO;
        }
        filled += (uint32_t)got;
        if (filled > reader->peak_window_bytes) reader->peak_window_bytes = filled;
    }
    reader->window_length = filled;
    return XH_SOURCE_OK;
}

int xh_source_read_byte(XhSourceReader *reader, uint32_t offset, unsigned char *byte) {
    int result;
    if (!reader || !reader->range || !byte) return XH_SOURCE_INVALID;
    if (reader->error) return reader->error;
    if (offset > reader->size) return XH_SOURCE_INVALID;
    if (offset == reader->size) return XH_SOURCE_EOF;
    if (offset < reader->window_offset ||
        offset - reader->window_offset >= reader->window_length) {
        result = refill(reader, offset);
        if (result != XH_SOURCE_OK) return result;
    }
    *byte = reader->window[offset - reader->window_offset];
    return XH_SOURCE_OK;
}

int xh_source_peek(XhSourceReader *reader, uint32_t lookahead, unsigned char *byte) {
    if (!reader || !reader->range || !byte) return XH_SOURCE_INVALID;
    if (reader->error) return reader->error;
    if (lookahead >= reader->capacity) return XH_SOURCE_LOOKAHEAD;
    /* Avoid overflow when a lookahead reaches beyond declared EOF. */
    if (lookahead >= reader->size - reader->offset) return XH_SOURCE_EOF;
    return xh_source_read_byte(reader, reader->offset + lookahead, byte);
}

int xh_source_consume(XhSourceReader *reader, unsigned char *byte) {
    int result = xh_source_peek(reader, 0, byte);
    if (result != XH_SOURCE_OK) return result;
    reader->offset++;
    if (*byte == '\r') {
        reader->line++; reader->column = 1; reader->previous_cr = 1;
    } else if (*byte == '\n') {
        if (!reader->previous_cr) reader->line++;
        reader->column = 1; reader->previous_cr = 0;
    } else {
        reader->column++; reader->previous_cr = 0;
    }
    return XH_SOURCE_OK;
}

XhSourceMark xh_source_mark(const XhSourceReader *reader) {
    XhSourceMark mark = {0, 1, 1, 0};
    if (reader) {
        mark.offset = reader->offset; mark.line = reader->line;
        mark.column = reader->column; mark.previous_cr = reader->previous_cr;
    }
    return mark;
}

int xh_source_restore(XhSourceReader *reader, XhSourceMark mark) {
    if (!reader || !reader->range || !mark.line || !mark.column ||
        mark.offset > reader->size) return XH_SOURCE_INVALID;
    if (reader->error) return reader->error;
    if (!(mark.offset == reader->size && mark.offset == reader->offset) &&
        (mark.offset < reader->window_offset ||
         mark.offset - reader->window_offset >= reader->window_length))
        return XH_SOURCE_LOOKAHEAD;
    reader->offset = mark.offset; reader->line = mark.line;
    reader->column = mark.column; reader->previous_cr = mark.previous_cr;
    return XH_SOURCE_OK;
}
