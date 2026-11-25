/* XHCC C99 assertions. Re-inclusion observes the current NDEBUG setting.
 * Enabled assertions use the target runtime's fprintf, stderr and abort ABI;
 * no host-specific assertion helper is required. */
#undef assert

#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
#include <stdio.h>
#include <stdlib.h>
#define assert(expression) ((expression) ? (void)0 : (void)(fprintf(stderr, \
    "%s:%d: %s: Assertion `%s' failed.\n", __FILE__, __LINE__, __func__, \
    #expression), abort()))
#endif
