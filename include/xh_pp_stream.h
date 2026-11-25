/* Bounded file preprocessing. Existing preproc_set_* options apply. */
#ifndef XH_PP_STREAM_H
#define XH_PP_STREAM_H
#include <stdint.h>
#define XH_PP_STREAM_WINDOW 1024
#define XH_PP_STREAM_SCRATCH 8192
/* Return zero on success, negative on failure. data is borrowed for this call. */
typedef int (*XhPpSink)(void *context, const char *data, int length);
/* Origin of each emitted chunk; path is borrowed until the callback returns.
 * Expansion bytes refer to their invocation, not a fabricated physical column. */
typedef void (*XhPpMap)(void *context, uint32_t emitted_offset,
                       const char *path, uint32_t raw_offset, int line);
int preproc_stream_file(const char *path, const char *const *forced_files,
                        int forced_count, XhPpSink sink, void *context,
                        XhPpMap map, void *map_context);
#endif
