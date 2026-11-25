/* Ark C Compiler - codegen.h
 * Common back-end interface shared by all code generators.
 * Wraps AsmBuffer so codegen_program and the backends stay independent.
 */
#ifndef ARK_CODEGEN_H
#define ARK_CODEGEN_H

#include "ir.h"

typedef struct {
    char *buf;
    int   len;
    int   cap;
    unsigned fixed;  /* 0: arena-owned dynamic storage; 1: borrowed, never realloc */
    unsigned failed; /* sticky capacity/argument failure; existing text stays valid */
} AsmBuffer;

void asmbuf_init(AsmBuffer *b);
void asmbuf_init_fixed(AsmBuffer *b, char *storage, int capacity);
/* desired text length excludes NUL; exact fit requires desired < capacity.
 * Returns 0 on success, -1 on sticky failure. Fixed storage never allocates. */
int asmbuf_require(AsmBuffer *b, int desired);
int asmbuf_failed(const AsmBuffer *b);
void asmbuf_reserve(AsmBuffer *b, int total);
void asmbuf_putc(AsmBuffer *b, char c);
void asmbuf_puts(AsmBuffer *b, const char *s);
void asmbuf_int(AsmBuffer *b, long long v);
void asmbuf_printf_int(AsmBuffer *b, const char *prefix, int v, const char *suffix);

#endif /* ARK_CODEGEN_H */
