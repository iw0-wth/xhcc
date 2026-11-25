/* xhcc stack allocation extension. */
#ifndef XH_ALLOCA_H
#define XH_ALLOCA_H

/* Evaluated calls currently produce an explicit compiler diagnostic pending
 * verified dynamic-stack lowering.  This alias never declares a libc call.
 * The intended extension contract is as follows.
 * Storage lasts until the allocating function returns; do not free it or
 * return its address.  The byte count is converted to the target size_t width.
 * Zero bytes may return a non-unique pointer that must not be dereferenced.
 * Negative counts become large unsigned sizes.  No stack-capacity or rounding
 * overflow check is provided: callers must bound the count to available stack.
 * Alignment follows the target's IR_ALLOCA implementation. */
#define alloca __builtin_alloca

#endif
