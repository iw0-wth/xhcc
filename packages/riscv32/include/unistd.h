/* Restricted POSIX type compatibility for this target package.
 * This header supplies ssize_t only. No POSIX syscall implementation or
 * read/write/close/lseek interface is provided by this package. Platforms
 * requiring those interfaces must supply their own headers and runtime. */
#ifndef XH_UNISTD_H
#define XH_UNISTD_H
#include <stddef.h>
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef ptrdiff_t ssize_t;
#endif
#endif
