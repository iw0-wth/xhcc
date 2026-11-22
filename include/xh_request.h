/* CompileRequest v1: explicit little-endian, length-prefixed records; no pointers. */
#ifndef XH_REQUEST_H
#define XH_REQUEST_H
#include <stdint.h>
#include "compiler.h"
#include "config.h"
#define XH_REQUEST_MAJOR 1
#define XH_REQUEST_MINOR 0
#define XH_REQUEST_MAX_BYTES (256u * 1024u)
/* Strings returned by read live until the next read; non-reentrant. Read applies
 * the transported configuration to g_config only after complete validation. */
void xh_request_set_tool(const char *tool);
/* Diagnostic options are transported separately from ArkccConfig. */
int xh_request_add_diagnostic_arg(const char *arg);
void xh_request_apply_diagnostics(void);
int xh_request_diagnostic_arg_count(void);
const char *xh_request_diagnostic_arg(int index);
int xh_request_write(const ArkOptions *opts, const char *path);
int xh_request_read(const char *path, ArkOptions *opts);
const char *xh_request_tool(void);
uint32_t xh_request_fingerprint(void);
#endif
