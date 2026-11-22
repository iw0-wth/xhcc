/* Ark C Compiler - x86 32-bit (cdecl ABI) backend
 *
 * Emits GAS-syntax x86 32-bit assembly. The goal of this backend is "syntactically
 * correct, human-readable, and can be further assembled by GNU as", so the
 * assemble stage is a stub (passes through text).
 *
 * Calling convention: cdecl — all arguments are passed on the stack (right to
 * left), caller cleans up. Uses a push/pop virtual operand stack (identical
 * to the IR); integers are treated as 4-byte. Local variables and parameters
 * are accessed via negative offsets from %ebp.
 *
 * This file also holds the real 64-bit backend ("x86_64", System V AMD64),
 * because the two share one built-in assembler.  amd64_emit_internal() keeps
 * the compiler's 4-byte stack-word model and layers the ABI on top of it:
 * pointer_size is 8 (so the type system lays out pointers/long/aggregates
 * correctly) while a pointer still occupies a single "word" whose machine
 * stack slot holds the whole 64-bit address (IR_AUX_SLOT64).  Integer
 * arguments go in rdi/rsi/rdx/rcx/r8/r9, floating ones in xmm0-xmm7, the rest
 * on the stack, and al carries the number of vector registers used.
 */
#include "backend.h"
#include "codegen.h"
#include "memory.h"
#include "platform.h"
#include "common.h"
#include "ir.h"
#include "diag.h"
#include "agginit.h"
#include "xh_x86_rules.h"
#include "xh_rules.h"
#include "xh_relocation.h"
extern int g_xhcc_elf_mode;
#include <stdio.h>
/* On host, <stdlib.h> provides getenv() for the ARKCC_DUMP_SYMS symbol map
 * (see x86_assemble_mode); on STM32 getenv() does not exist. */
#if ARKCC_PROFILE != ARKCC_PROFILE_STM32_MIN
#  include <stdlib.h>
#endif

static int x86_abi_align(int bytes,const char *key) {
    int alignment=xh_x86_number(key);
    if(alignment<=0 || alignment>4096 || (alignment&(alignment-1)))return bytes;
    return (bytes+alignment-1)&~(alignment-1);
}

/* cdecl has no integer-class argument registers; all args are on the stack. */
static const char *arg_reg(int i, int abi) {
    (void)i; (void)abi;
    return 0;
}

static int arg_reg_count(int abi) {
    (void)abi;
    return xh_x86_number("abi.integer.count");
}

/* Byte offset (from ebp) of the (reg_idx)-th stack argument.
 * cdecl: stack args begin at [ebp+8] (above saved ebp + return addr),
 * each 4 bytes wide. */
static int stack_arg_offset(int reg_idx, int abi) {
    (void)abi;
    return xh_x86_number("abi.stack.argument_base") + reg_idx * xh_x86_number("abi.stack.word");
}

static void emit_str_literal_x(AsmBuffer *b, const char *data, int len) {
    int i;
    asmbuf_putc(b, '"');
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        switch (c) {
            case '\n': asmbuf_puts(b, xh_x86_rule("text.a80e516bfb196e1c")); break;
            case '\t': asmbuf_puts(b, xh_x86_rule("text.9e6ce97799490c6a")); break;
            case '\r': asmbuf_puts(b, xh_x86_rule("text.156de42b059b0f09")); break;
            case '\\': asmbuf_puts(b, xh_x86_rule("text.b11b4ca186c00769")); break;
            case '"':  asmbuf_puts(b, xh_x86_rule("text.1ad070bb285bc0fd")); break;
            default:
                if (c < 0x20 || c >= 0x7F) {
                    const char *hex = "0123456789abcdef";
                    asmbuf_puts(b, xh_x86_rule("text.a86d6dedf5bd962e"));
                    asmbuf_putc(b, hex[(c>>4)&0xF]);
                    asmbuf_putc(b, hex[c&0xF]);
                } else {
                    asmbuf_putc(b, (char)c);
                }
                break;
        }
    }
    asmbuf_putc(b, '"');
}

static const char *binop_x(int op) {
    switch (op) {
        case (int)TOK_PLUS:  return xh_x86_rule("text.7e9e5ac30f2216fd");
        case (int)TOK_MINUS: return xh_x86_rule("text.ddc6e2b224d0fd82");
        case (int)TOK_BAND:  return xh_x86_rule("text.6201111b83a0cb5b");
        case (int)TOK_BOR:   return xh_x86_rule("text.7175517a370b5cd2");
        case (int)TOK_BXOR:  return xh_x86_rule("text.1ad321bd42eb7199");
        default:             return 0;
    }
}

static void x86_scalar_rule(AsmBuffer *b,int op,int uns,int width) {
 const char *name=0;char key[80];
 switch(op){
 case TOK_PLUS:name="add";break;case TOK_MINUS:name="sub";break;
 case TOK_MUL:name="mul";break;case TOK_DIV:name="div";break;case TOK_MOD:name="mod";break;
 case TOK_BAND:name="and";break;case TOK_BOR:name="or";break;case TOK_BXOR:name="xor";break;
 case TOK_SHL:name="shl";break;case TOK_SHR:name="shr";break;
 case TOK_EQ:name="eq";break;case TOK_NE:name="ne";break;
 case TOK_LT:name="lt";break;case TOK_GT:name="gt";break;case TOK_LE:name="le";break;case TOK_GE:name="ge";break;
 default:name="invalid";break;
 }
 snprintf(key,sizeof(key),"scalar%d.%c.%s",width,uns?'u':'s',name);
 asmbuf_puts(b,xh_x86_rule(key));
}

/* === Aggregate initialisers ==============================================
 * The bytes of an aggregate initialiser come from the shared filler in
 * src/agginit.c -- the same walk the ARM and RISC-V backends use -- so brace
 * elision, designated elements, string elements and the bit-exact expansion of
 * floating constants are decided in exactly one place.
 *
 * An element that is an address is not a byte pattern, so the assembler gets
 * ".quad name" instead: the elements irgen recorded in init_refs split the
 * byte run.
 *
 * The private parser that stood here read only integer constants.  It left the
 * '.' of "1.0" in the text, the array loop then made no progress at all, and a
 * floating constant inside an aggregate initialiser stopped the compiler with a
 * segmentation fault after several billion spins -- and had the loop been
 * fixed alone, it would have written the integer 1 as the bytes of 1.0. */
static int x86_agg_addr_hook(void *ud, int off, Type *t) {
    IrGlobal *g = (IrGlobal *)ud;
    int i;
    (void)t;
    if (!g) return 0;
    for (i = 0; i < g->init_ref_count; i++)
        if (g->init_refs[i].offset == off) return 1;
    return 0;
}

/* One run of bytes as .byte/.word/.long/.quad, widened first, `off0` being the
 * byte offset of `p[0]` inside the object so the wide directives stay aligned
 * with the object rather than with the run. */
static void x86_emit_byte_run(AsmBuffer *b, const unsigned char *p, int n,
                              int off0) {
    int k = 0;
    while (k < n) {
        int at = off0 + k;
        int chunk = 1, i;
        long long v = 0;
        if ((at % 8) == 0 && k + 8 <= n) chunk = 8;
        else if ((at % 4) == 0 && k + 4 <= n) chunk = 4;
        else if ((at % 2) == 0 && k + 2 <= n) chunk = 2;
        for (i = 0; i < chunk; i++)
            v |= (long long)((unsigned long long)p[k + i] << (8 * i));
        asmbuf_puts(b, xh_x86_rule("text.1a0f564ddc603945"));
        asmbuf_puts(b, chunk <= 1 ? xh_x86_rule("text.315cff705ce8e96c")
                         : (chunk <= 2 ? xh_x86_rule("text.36316f60cbea771f")
                            : (chunk <= 4 ? xh_x86_rule("text.46303c2508c7b7c8") : xh_x86_rule("text.894f405db0ceb0e2"))));
        asmbuf_putc(b, ' ');
        asmbuf_int(b, v);
        asmbuf_putc(b, '\n');
        k += chunk;
    }
}

/* The bytes of one object, with every element the caller places itself spelled
 * as a symbol reference instead of as a numbered byte pattern. */
static void x86_emit_agg_bytes(AsmBuffer *b, const unsigned char *p, int n,
                               IrGlobal *g) {
    int psz = type_ptr_size();
    int k = 0;
    if (!g || !g->init_refs || g->init_ref_count <= 0) {
        x86_emit_byte_run(b, p, n, 0);
        return;
    }
    while (k < n) {
        int i, hit = -1;
        for (i = 0; i < g->init_ref_count; i++)
            if (g->init_refs[i].offset == k) { hit = i; break; }
        if (hit >= 0) {
            IrSymRef *r = &g->init_refs[hit];
            asmbuf_puts(b, psz == 8 ? xh_x86_rule("text.12e87f2cba994e97") : xh_x86_rule("text.7ad86ea132e66f36"));
            if (r->str) {
                asmbuf_puts(b, xh_x86_rule("text.2edd14ef94518f14"));
                asmbuf_puts(b, g->name);
                asmbuf_putc(b, '_');
                asmbuf_int(b, r->offset);
            } else {
                asmbuf_puts(b, r->sym);
                if (r->add > 0) {
                    asmbuf_puts(b, xh_x86_rule("text.46209f696e32b548"));
                    asmbuf_int(b, (int)r->add);
                } else if (r->add < 0) {
                    asmbuf_puts(b, xh_x86_rule("text.d3150495ce4f5d1a"));
                    asmbuf_int(b, (int)(-r->add));
                }
            }
            asmbuf_putc(b, '\n');
            k += psz;
            continue;
        }
        {
            int j = k + 1;
            while (j < n) {
                int h = 0;
                for (i = 0; i < g->init_ref_count; i++)
                    if (g->init_refs[i].offset == j) { h = 1; break; }
                if (h) break;
                j++;
            }
            x86_emit_byte_run(b, p + k, j - k, k);
            k = j;
        }
    }
}

/* Emit a global's aggregate initialiser as data.  A shape the filler cannot
 * place is reported and the storage keeps zeroes: emitting the object without
 * its initialiser without a word would be a wrong answer, not a missing
 * feature. */
static void x86_emit_aggregate_init(AsmBuffer *b, IrGlobal *g) {
    Type *t = g->type;
    const char *be = (type_ptr_size() == 8) ? "x86-64" : "x86";
    int sz = t ? type_size(t) : 4;
    unsigned char *bytes;
    int i, bad = 0;
    if (sz <= 0) sz = 4;
    bytes = (unsigned char *)mem_pool_alloc(sz);
    if (!bytes) bad = 1;
    else {
        for (i = 0; i < sz; i++) bytes[i] = 0;
        if (!t || ark_agg_fill_ex(t, g->init_aggregate, bytes, sz,
                                  x86_agg_addr_hook, (void *)g) != 0)
            bad = 1;
    }
    if (bad) {
        diag_tool(DIAG_SEV_ERROR, "arkcc",
                  "the %s backend cannot turn the initialiser of the global "
                  "'%s' into data; this backend turns integer and "
                  "floating-point constants, a string literal for a char array, "
                  "and address constants into bytes -- this initialiser is "
                  "none of those",
                  be, g->name, 0,0,0,0);
        asmbuf_puts(b, xh_x86_rule("text.df9f53e29ab944ee"));
        asmbuf_int(b, sz);
        asmbuf_putc(b, '\n');
        return;
    }
    x86_emit_agg_bytes(b, bytes, sz, g);
}

/* `char s[] = "ab";` fills the object, `char *p = "ab";` stores the address of
 * the literal (which gets a .rodata label of its own, see the data section).
 * Neither used to reach this backend at all: the object was written as a single
 * zero word, which is a silent wrong answer. */
static void x86_emit_string_init(AsmBuffer *b, IrGlobal *g) {
    Type *t = g->type;
    int sz = t ? type_size(t) : 0;
    if (t && t->kind == TYPE_ARRAY && t->base && type_size(t->base) == 1) {
        unsigned char *bytes;
        if (sz <= 0) sz = 1;
        bytes = (unsigned char *)mem_pool_alloc(sz);
        if (bytes) {
            ark_agg_fill_cstr(g->init_str, bytes, sz);
            x86_emit_byte_run(b, bytes, sz, 0);
            return;
        }
    } else if (t && t->kind == TYPE_PTR) {
        asmbuf_puts(b, type_ptr_size() == 8 ? xh_x86_rule("text.d4a3ff058ef4428a")
                                            : xh_x86_rule("text.ded4af16bb21ec91"));
        asmbuf_puts(b, g->name);
        asmbuf_putc(b, '\n');
        return;
    }
    diag_tool(DIAG_SEV_ERROR, "arkcc",
              "the %s backend cannot place the string initialiser of the global "
              "'%s' in data; only a char array and a pointer take one",
              type_ptr_size() == 8 ? "x86-64" : "x86", g->name, 0,0,0,0);
    asmbuf_puts(b, xh_x86_rule("text.df9f53e29ab944ee"));
    asmbuf_int(b, sz > 0 ? sz : 4);
    asmbuf_putc(b, '\n');
}

/* Set when the emitter produced text it knows is not assemblable. */
static int x86_emit_errors;

/* _Complex on this backend is limited to what a single expression needs:
 * a constant push, + - * /, and == / != .  Anything that has to move a whole
 * 16-byte object through a local, a global, an array element, a cast or a
 * copy used to be compiled as if it were a 4-byte scalar -- a silent wrong
 * answer that only showed up at run time.  Those paths are hard errors now:
 * x86_emit_errors makes the backend write no output at all. */
static void x86_complex_unsupported(AsmBuffer *b, const char *what) {
    ark_log("[x86] error: _Complex value not supported by the x86 backend (%s)\n", what);
    x86_emit_errors++;
    asmbuf_puts(b, xh_x86_rule("text.1f12afdaf3f980fb"));
    asmbuf_puts(b, what);
    asmbuf_putc(b, '\n');
}

/* Size in bytes of a named global (0 when unknown). */
static int x86_global_size(IrProgram *prog, const char *name) {
    int i;
    if (!name) return 0;
    for (i = 0; i < prog->global_count; i++) {
        IrGlobal *g = prog->globals[i];
        if (g && g->name && ark_streq(g->name, name))
            return g->type ? type_size(g->type) : 0;
    }
    return 0;
}

/* Emit the read-only (string literal) and data (global object) sections.
 * Shared by the i386 and AMD64 emitters so the two cannot drift apart.
 * The emitted text is unchanged from when this lived inside
 * x86_emit_internal; only the buffer became a parameter.
 *
 * The labels are `.L`-prefixed on purpose.  They used to be `ark_str_<id>`,
 * which the ARKOBJ2 writer exports (every label not starting with `.L` is
 * global), so two translation units that each contained a string literal
 * emitted two strong globals with the same name.  -link then printed
 * "duplicate strong symbol ark_str_0, using first definition" for every
 * literal in the image, and the second object's pointers bound to the *first*
 * object's text.  The self-host link of arkcc's own 25 objects produced
 * hundreds of those lines; a two-object test never showed it because only one
 * side had literals.  A literal is per-object state, so its label is local. */
static void x86_emit_data_sections(AsmBuffer *buf, IrProgram *prog) {
    asmbuf_puts(buf, xh_x86_rule("text.cc8f05622986ef9c"));
    {
        int i;
        for (i = 0; i < prog->string_count; i++) {
            IrString *s = prog->strings[i];
            asmbuf_puts(buf, xh_x86_rule("text.eaaf9f65c87c4410"));
            asmbuf_int(buf, s->id);
            asmbuf_puts(buf, xh_x86_rule("text.e9e59ed0a331a459"));
            emit_str_literal_x(buf, s->data, s->len);
            asmbuf_putc(buf, '\n');
        }
    }
    /* A string literal that initialises a pointer, or an element of an
     * aggregate, has no pool entry of its own: give it one label per object (or
     * per element) here, so the .data side only ever carries the address. */
    {
        int i;
        for (i = 0; i < prog->global_count; i++) {
            IrGlobal *g = prog->globals[i];
            int ri;
            if (g->is_extern) continue;
            if (g->has_init && g->init_str && g->type &&
                g->type->kind != TYPE_ARRAY && g->type->kind != TYPE_STRUCT &&
                g->type->kind != TYPE_UNION) {
                asmbuf_puts(buf, xh_x86_rule("text.2edd14ef94518f14"));
                asmbuf_puts(buf, g->name);
                asmbuf_puts(buf, xh_x86_rule("text.e9e59ed0a331a459"));
                emit_str_literal_x(buf, g->init_str, ark_strlen(g->init_str));
                asmbuf_putc(buf, '\n');
            }
            if (!g->init_refs || g->init_ref_count <= 0) continue;
            for (ri = 0; ri < g->init_ref_count; ri++) {
                IrSymRef *r = &g->init_refs[ri];
                if (!r->str) continue;
                asmbuf_puts(buf, xh_x86_rule("text.2edd14ef94518f14"));
                asmbuf_puts(buf, g->name);
                asmbuf_putc(buf, '_');
                asmbuf_int(buf, r->offset);
                asmbuf_puts(buf, xh_x86_rule("text.e9e59ed0a331a459"));
                emit_str_literal_x(buf, r->str, ark_strlen(r->str));
                asmbuf_putc(buf, '\n');
            }
        }
    }
    asmbuf_puts(buf, xh_x86_rule("text.7c91bedb5dfc818e"));
    {
        int i;
        for (i = 0; i < prog->global_count; i++) {
            IrGlobal *g = prog->globals[i];
            if (g->is_extern) continue;  /* extern: no definition */
            /* Only non-static globals get external linkage. Emitting ".globl"
             * together with ".local" for the same symbol was contradictory (and
             * ".local" is rejected by the PE-targeted GNU as shipped with some
             * toolchains); a plain label is already local, so statics need no
             * directive at all. The ARM backend guards the same way. */
            if (!g->is_static) {
                asmbuf_puts(buf, xh_x86_rule("text.9bd395d67adffd1c"));
                asmbuf_puts(buf, g->name);
                asmbuf_putc(buf, '\n');
            }
            asmbuf_puts(buf, xh_x86_rule("text.341e3bc0663e193f"));asmbuf_int(buf,g->align_bytes>type_align(g->type)?g->align_bytes:type_align(g->type));asmbuf_putc(buf,'\n');
            asmbuf_puts(buf, g->name);
            asmbuf_puts(buf, xh_x86_rule("text.f3743a0a18e53d13"));
            /* The x86_64 backend does not implement _Complex at all; a global
             * of that type must not quietly get its real part written as one
             * .quad and the imaginary half dropped. */
            if (g->type && ARK_IS_CX(g->type)) {
                x86_complex_unsupported(buf, "global object");
                asmbuf_puts(buf, xh_x86_rule("text.df9f53e29ab944ee"));
                asmbuf_int(buf, g->type && type_size(g->type) > 0 ? type_size(g->type) : 16);
                asmbuf_putc(buf, '\n');
            } else if (g->is_float && g->has_init) {
                /* Float/double global: emit raw bits.
                 * float (4 bytes) -> .long; double (8 bytes) -> .quad */
                int tsz = g->type ? type_size(g->type) : 8;
                if (tsz <= 4) {
                    union { float f; int i; } u;
                    u.f = (float)g->init_float;
                    asmbuf_puts(buf, xh_x86_rule("text.7ad86ea132e66f36"));
                    asmbuf_int(buf, u.i);
                    asmbuf_putc(buf, '\n');
                } else {
                    union { double d; long long i; } u;
                    u.d = g->init_float;
                    asmbuf_puts(buf, xh_x86_rule("text.12e87f2cba994e97"));
                    asmbuf_int(buf, u.i);
                    asmbuf_putc(buf, '\n');
                }
            } else if (g->init_sym && g->has_init) {
                asmbuf_puts(buf,type_ptr_size()==8?xh_x86_rule("text.12e87f2cba994e97"):xh_x86_rule("text.7ad86ea132e66f36"));
                asmbuf_puts(buf,g->init_sym);
                if(g->init_sym_off){asmbuf_puts(buf, xh_x86_rule("text.46209f696e32b548"));asmbuf_int(buf,g->init_sym_off);}
                asmbuf_putc(buf,'\n');
            } else if (g->init_aggregate && g->has_init) {
                x86_emit_aggregate_init(buf, g);
            } else if (g->init_str && g->has_init) {
                x86_emit_string_init(buf, g);
            } else {
                /* Scalar init: choose directive by type size. */
                int tsz = g->type ? type_size(g->type) : 4;
                const char *dir = ".long";
                if (tsz <= 1) dir = ".byte";
                else if (tsz <= 2) dir = ".word";
                else if (tsz <= 4) dir = ".long";
                else dir = ".quad";
                if (!g->has_init && tsz > 8) {
                    /* A zero-initialised object of any size is one .zero, not
                     * one directive per eight bytes.  The old per-8-byte fill
                     * emitted 8.4 million ".quad 0" lines for a 64 MB buffer,
                     * which consumed the compiler's own 64 MB arena before the
                     * assembler saw a single line: memory.c in this very tree
                     * (static unsigned char g_pool[ARK_POOL_SIZE]) could not be
                     * compiled for x86 or x86_64 at all.  Semantics are
                     * unchanged -- .data still carries the same zero bytes. */
                    asmbuf_puts(buf, xh_x86_rule("text.df9f53e29ab944ee"));
                    asmbuf_int(buf, tsz);
                    asmbuf_putc(buf, '\n');
                } else {
                    asmbuf_puts(buf, xh_x86_rule("text.1a0f564ddc603945"));
                    asmbuf_puts(buf, dir);
                    asmbuf_puts(buf, xh_x86_rule("text.36a9e7f1c95b82ff"));
                    asmbuf_int(buf, g->has_init ? g->init_int : 0);
                    asmbuf_putc(buf, '\n');
                    if (tsz > 8) {
                        /* Tail of a wide scalar: still zero bytes, still one
                         * directive (a 16-byte initialized object used to need
                         * a second ".quad 0" line; now it is ".zero 8"). */
                        asmbuf_puts(buf, xh_x86_rule("text.df9f53e29ab944ee"));
                        asmbuf_int(buf, tsz - 8);
                        asmbuf_putc(buf, '\n');
                    }
                }
            }
        }
    }
}

/* === 64-bit / floating-point helpers for the i386 emitter ================
 *
 * Every 8-byte value on the IR stack keeps its HIGH dword on top.  That is the
 * canonical order irgen, the ARM, RISC-V and AMD64 backends all use, and the
 * i386 emitter was the only one that disagreed -- with itself: IR_TRUNC and
 * irgen's own narrowing (a bare IR_POP, which discards "the high word on top")
 * assumed high-on-top while IR_PUSH_VAR / IR_LOAD_IDX / IR_64PUSH built the
 * mirror image.  The result was that a 64-bit value was only right when it was
 * never taken apart again, and `(int)some_long_long` read the high half.
 */

/* Flip the two dwords of an 8-byte stack slot in place.  Everything that has
 * to treat such a slot as a *blob* -- a double read with movq, a 64-bit value
 * handed to cdecl -- uses this instead of a separate movq encoding.  eax/edx
 * are scratch registers in this backend. */
static void x86_pair_swap_at(AsmBuffer *b, int off) {
    /* Swap the two dwords of the 8-byte blob at [esp+off].  Both
     * displacements must always be printed: the old version only emitted a
     * displacement when `off` was non-zero, so the off==0 case (the common
     * one) read the SAME slot twice and "swapped" nothing.  Every 64-bit
     * `movq xmm, [esp]` then loaded high:low backwards, which is why
     * `double d = 7.9; (int)d;` answered 0 on the i386 target. */
    asmbuf_puts(b, xh_x86_rule("text.3373df59b78890b1"));
    asmbuf_int(b, off);
    asmbuf_puts(b, xh_x86_rule("text.0a2ae89f61af0778"));
    asmbuf_int(b, off + 4);
    asmbuf_puts(b, xh_x86_rule("text.7fe6edd93915d970"));
    asmbuf_int(b, off);
    asmbuf_puts(b, xh_x86_rule("text.33a42d27bd462c00"));
    asmbuf_int(b, off + 4);
    asmbuf_puts(b, xh_x86_rule("text.44b5e9f85f2e61a3"));
}

/* A unique label inside one function.  The internal assembler has no numeric
 * local labels: "1:" defines a symbol literally named "1", so a reference to
 * "1f" never resolves.  Every helper-generated label therefore carries the
 * function name plus a per-function counter (def=1 defines it, 0 references
 * it). */
static void x86_elbl(AsmBuffer *b, const char *fname, const char *tag,
                     int n, int def) {
    char nb[24];
    asmbuf_puts(b, xh_x86_rule("text.4b6d79fc377f87b4"));
    asmbuf_puts(b, fname);
    asmbuf_putc(b, '_');
    asmbuf_puts(b, tag);
    asmbuf_puts(b, diag_num(nb, n));
    asmbuf_puts(b, def ? xh_x86_rule("text.e7ac0786668e0ff0") : xh_x86_rule("text.e3b0c44298fc1c14"));
    asmbuf_putc(b, '\n');
}

/* eax (as an UNSIGNED 32-bit value) -> double in `dst`.  cvtsi2sd is
 * signed-only, so split off the low bit: (u >> 1) always fits in 31 bits and
 * the answer is 2 * (u >> 1) + (u & 1).  Clobbers ecx, edx, xmm3; leaves eax
 * holding the original value again. */
static void x86_u32_to_xmm(AsmBuffer *b, const char *dst) {
    asmbuf_puts(b, xh_x86_rule("text.661bf837b8ae2516"));
    asmbuf_puts(b, dst);
    asmbuf_puts(b, xh_x86_rule("text.b68de343b23b037c"));
    asmbuf_puts(b, dst);
    asmbuf_puts(b, xh_x86_rule("text.0a07f659461970d8"));
    asmbuf_puts(b, dst);
    asmbuf_puts(b, xh_x86_rule("text.321df96026d848e1"));
    asmbuf_puts(b, dst);
    asmbuf_puts(b, xh_x86_rule("text.52eb16d9880540bf"));
}

static void xh_x86_inline_asm(AsmBuffer*b,IrInst*I,int wide){const char*regs[3];const char*pops[3];char key[40];int ri;for(ri=0;ri<3;ri++){snprintf(key,sizeof(key),"abi.inline32.%d",ri);regs[ri]=xh_x86_rule(key);snprintf(key,sizeof(key),"abi.inline64.%d",ri);pops[ri]=xh_x86_rule(key);}int k,outs=I->aux&1;const char*s=I->name?I->name:"";for(k=(int)I->val-1;k>=0;k--){asmbuf_puts(b, xh_x86_rule("text.361c2bed6cf28f5b"));asmbuf_puts(b,wide?pops[outs+k]:regs[outs+k]);asmbuf_putc(b,'\n');}asmbuf_puts(b, xh_x86_rule("text.1a0f564ddc603945"));while(*s){if(*s=='%'&&s[1]=='%'){asmbuf_putc(b,'%');s+=2;}else if(*s=='%'&&s[1]>='0'&&s[1]<='2'){asmbuf_puts(b,regs[s[1]-'0']);s+=2;}else asmbuf_putc(b,*s++);}asmbuf_putc(b,'\n');if(outs)asmbuf_puts(b,wide?xh_x86_rule("text.de1843ef75a9fb24"):xh_x86_rule("text.2ac65d444c4eb237"));}
static IrProgram *xh_x86_prog;
static int x86_emit_internal(IrProgram *prog, char *out, int cap, int abi) {
    if (xh_x86_rules_begin(0)) return -1;
    xh_x86_prog = prog;
    AsmBuffer buf;
    int fi;
    (void)abi;  /* cdecl only; abi is always ARK_ABI_SYSV (0) */
    asmbuf_init(&buf);

    asmbuf_puts(&buf, xh_x86_rule("text.5662c0e931064efe"));
    asmbuf_puts(&buf, xh_x86_rule("text.a8e02e4f5a197f3d"));
    x86_emit_data_sections(&buf, prog);
    asmbuf_puts(&buf, xh_x86_rule("text.a6b017eae27bd97f"));

    for (fi = 0; fi < prog->func_count; fi++) {
        IrFunc *f = prog->funcs[fi];
        int i;
        int local_size = x86_abi_align(f->local_size,"abi.frame.alignment");
        if (local_size < xh_x86_number("abi.frame.minimum_locals")) local_size = xh_x86_number("abi.frame.minimum_locals");
        /* 32-bit frame geometry: locals live at [ebp - framelo + off], with
         * `off` counted *upward* from the bottom of the local area -- exactly
         * like the AMD64 path's [rbp - FRAMELO + off].  This used to be
         * [ebp - (48 + off)], which counted downward, so element k of a local
         * array sat at a *lower* address than element 0 while IR_LOAD_IDX /
         * IR_PUSH_ADDR walk upward: every local array read a stale or
         * unrelated slot (int a[3]={1,2,3} read back 1,0,0).  Members of a
         * struct only "worked" because the upward offsets still landed in the
         * 36-byte gap between the saved registers and the local area. */
        int framelo = xh_x86_number("abi.frame.locals_gap") + local_size;
        /* One result buffer per call site that returns a struct/union wider
         * than 8 bytes (IR_CALL bit 36).  cdecl hands such a result back
         * through a buffer the caller owns, passed as the hidden first
         * argument ([ebp+8]), so a call site needs both a buffer and an extra
         * pushed word.  One buffer per *site*: in `struct S a = f(), b = g();`
         * two results are live at the same time.  The buffers sit below the
         * call-scratch area, which is why the reservation below is computed
         * instead of being the old constant 512. */
        int sret_off_tab[ARK_MAX_SRET_FUNCS];
        int sret_sz_tab[ARK_MAX_SRET_FUNCS];
        int sret_site_n = 0;
        int sret_site_cur = 0;
        int sret_pool = 0;
        int sret_area_off = framelo + xh_x86_number("abi.frame.scratch_bytes");
        int frame_reserve;
        {
            int ii;
            for (ii = 0; ii < f->code_count; ii++) {
                IrInst *si = &f->code[ii];
                int sz;
                if (si->op != IR_CALL || !((si->val >> 36) & 1)) continue;
                sz = (int)((si->val >> 37) & 0xFFFF);
                sz = x86_abi_align(sz,"abi.aggregate.sret_alignment");
                if (sz <= 0 || sz > 0xFFFF) {
                    char b1[12];
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "call site in '%s' returns a composite of %s bytes; "
                              "the x86 backend needs 1..65535",
                              f->name, diag_num(b1, sz), 0, 0, 0, 0);
                    sz = 8;
                }
                if (sret_site_n >= ARK_MAX_SRET_FUNCS) {
                    char b1[12], b2[12];
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "function '%s' has %s call sites returning a struct "
                              "wider than 8 bytes; at most %s are supported; raise "
                              "ARK_MAX_SRET_FUNCS and rebuild arkcc",
                              f->name, diag_num(b1, sret_site_n + 1),
                              diag_num(b2, ARK_MAX_SRET_FUNCS), 0, 0, 0);
                    break;
                }
                sret_off_tab[sret_site_n] = sret_pool;
                sret_sz_tab[sret_site_n] = sz;
                sret_pool += sz;
                sret_site_n++;
            }
        }
        /* The prologue pushes exactly 12 bytes (ebx, esi, edi), so the frame
         * bottom is ebp - (12 + frame_reserve).  The reservation must reach the
         * lowest byte of the sret pool, otherwise the buffer sits *below* esp:
         * restoring esp to the saved value and then `push eax` (the result
         * pointer) overwrites the buffer's first word, and the caller reads a
         * stack address back as the struct's leading member. */
        frame_reserve = (sret_area_off + sret_pool) - xh_x86_number("abi.frame.saved_bytes");
        /* sret detection: if the function returns a struct/union > 8 bytes,
         * cdecl passes a hidden first arg = return buffer pointer at [ebp+8]. */
        int sret_off = 0;
        if (f->ret_type &&
            (f->ret_type->kind == TYPE_STRUCT || f->ret_type->kind == TYPE_UNION) &&
            type_size(f->ret_type) > xh_x86_number("abi.aggregate.direct_limit")) {
            sret_off = local_size + xh_x86_number("abi.frame.locals_gap") + xh_x86_number("abi.frame.sret_pointer_offset");
        }

        /* Static functions must stay file-local; exporting them as strong
         * globals made two translation units with same-named statics collide. */
        if (!f->is_static) {
            asmbuf_puts(&buf, xh_x86_rule("text.9bd395d67adffd1c"));
            asmbuf_puts(&buf, f->name);
            asmbuf_putc(&buf, '\n');
        }
        asmbuf_puts(&buf, f->name);
        asmbuf_puts(&buf, xh_x86_rule("text.f3743a0a18e53d13"));
        /* cdecl prologue: push ebp; mov ebp, esp; save callee-saved (ebx, esi,
         * edi); sub esp for locals + scratch. 3 pushes (12 bytes) + 36 padding
         * = 48 byte gap before the local area, which spans
         * [ebp - framelo, ebp - 48) -- see the framelo comment above. */
        asmbuf_puts(&buf, xh_x86_rule("text.04eeefb078138dc3"));
        asmbuf_puts(&buf, xh_x86_rule("text.416ac8dda322036e"));
        asmbuf_puts(&buf, xh_x86_rule("text.1a4679f5966a2009"));
        asmbuf_puts(&buf, xh_x86_rule("text.35e4b15127f7957f"));
        asmbuf_int(&buf, frame_reserve);
        asmbuf_putc(&buf, '\n');
        /* Save sret pointer if this function uses sret */
        if (sret_off) {
            asmbuf_puts(&buf, xh_x86_rule("text.468360761906663c"));
            asmbuf_int(&buf, sret_off);
            asmbuf_puts(&buf, xh_x86_rule("text.44b5e9f85f2e61a3"));
        }

        /* cdecl: all args on stack at [ebp + 8 + idx * 4].
         * For sret functions, the hidden pointer is arg 0, so real args
         * start at index 1. param_idx tracks the original parameter index. */
        int arg_idx = sret_off ? 1 : 0;
        int param_idx = 0;
        /* Counter for helper-generated internal labels (see x86_elbl). */
        int lbl64 = 0;
        /* 16 spare bytes for the 64-bit divide loop, in the hole between the
         * call-argument scratch area (which ends at framelo + 496 for the 116
         * words the width check allows) and the sret pool (framelo + 512). */
        int divtmp = framelo + xh_x86_number("abi.frame.div_scratch_offset");
        for (i = 0; i < f->code_count; i++) {
            IrInst *I = &f->code[i];
            switch (I->op) {
                case IR_FUNC_BEGIN:
                case IR_FUNC_END:
                    break;
                case IR_GETARG: {
                    /* irgen encodes aux = (type_size << 8) | slot for regular
                     * params, and aux = k for variadic va_arg slots
                     * (val >= 8000). cdecl: all args on the stack. */
                    Type *pt = 0;
                    int reg_idx;
                    int max_int = arg_reg_count(abi);
                    if (I->val >= 8000) {
                        /* Bug-X4: variadic va_arg slot. aux is the raw integer
                         * arg index. All args on stack at [ebp + stack_arg_offset(k)]. */
                        reg_idx = I->aux & 0xFF;
                        if (reg_idx < max_int) {
                            /* cdecl: no register args, this branch is dead */
                            break;
                        } else {
                            /* The 8000..8063 mirror region is an AMD64 shape;
                             * the driver clears g_ir_va_slot_region for this
                             * target, so this path is unreachable by
                             * construction.  With the upward frame mapping an
                             * offset of 8000 would land *above* ebp, so refuse
                             * loudly instead of scribbling on the caller. */
                            ark_log("[x86] error: va_arg frame slot is an AMD64-only shape\n");
                            x86_emit_errors++;
                            asmbuf_puts(&buf, xh_x86_rule("text.60140dc629fa4eea"));
                        }
                        break;
                    }
                    /* Regular param: look up type */
                    if (f->param_types && param_idx < f->param_count)
                        pt = f->param_types[param_idx];
                    param_idx++;
                    reg_idx = arg_idx;
                    arg_idx++;
                    /* cdecl packs the arguments in declaration order with no
                     * padding, so the offset of slot k is the sum of the real
                     * widths of slots 0..k-1 -- not 4 * k.  A double or a long
                     * long takes two words; a composite wider than 8 bytes
                     * travels as one address word.  The old 4*k offset put
                     * every parameter after the first double in the wrong
                     * place (f(1.5, 2.25) read b's low word as its `a`).
                     * stack_arg_offset() is kept for the va tail's 4-byte
                     * walk, which is a different (and deliberately uniform)
                     * shape. */
                    {
                        int stk = xh_x86_number("abi.stack.argument_base") + (sret_off ? xh_x86_number("abi.stack.word") : 0);
                        int st_sz = (I->aux >> 8) & 0xFFFFFF;
                        /* param_idx was already bumped, so param_idx - 1 is the
                         * number of *real* parameters ahead of this one.  It must
                         * not be confused with reg_idx: for sret functions
                         * reg_idx also counts the hidden return-buffer pointer,
                         * and using it as the loop bound made the walk add this
                         * very parameter's own width -- mk5(int v) then read
                         * [ebp+16] instead of [ebp+12] and every >8-byte
                         * struct return on i386 came back as zeros. */
                        int nprev = param_idx - 1;
                        int j;
                        int dst = framelo - (int)I->val;
                        /* st_sz comes from irgen as the parameter's real
                         * width: float 4, double 8. */
                        for (j = 0; j < nprev && j < 64; j++) {
                            int w = 4;
                            if (f->param_types && j < f->param_count) {
                                int ps = type_size(f->param_types[j]);
                                w = xh_x86_aggregate_wire(ps);
                            }
                            stk += w;
                        }
                        if (st_sz > xh_x86_number("abi.aggregate.direct_limit")) {
                            /* Named composite wider than 8 bytes: the slot holds
                             * a pointer, and the payload is copied into the
                             * frame -- same wire shape the caller emitted. */
                            asmbuf_puts(&buf, xh_x86_rule("text.e70306ccc1c885dd"));
                            asmbuf_int(&buf, stk);
                            asmbuf_puts(&buf, xh_x86_rule("text.515863caf431bf57"));
                            asmbuf_int(&buf, dst);
                            asmbuf_puts(&buf, xh_x86_rule("text.bf5c869475327afb"));
                            asmbuf_int(&buf, st_sz);
                            asmbuf_puts(&buf, xh_x86_rule("text.8d3fc2867d2efe58"));
                        } else if (st_sz > xh_x86_number("abi.ir.word")) {
                            /* 64-bit scalar or a 5..8 byte composite: two
                             * words, low word at the lower address. */
                            asmbuf_puts(&buf, xh_x86_rule("text.f11eba8d7ffdb259"));
                            asmbuf_int(&buf, stk);
                            asmbuf_puts(&buf, xh_x86_rule("text.435527643a0ff4db"));
                            asmbuf_int(&buf, dst);
                            asmbuf_puts(&buf, xh_x86_rule("text.31a585f1e3640137"));
                            asmbuf_int(&buf, stk + 4);
                            asmbuf_puts(&buf, xh_x86_rule("text.435527643a0ff4db"));
                            asmbuf_int(&buf, dst - 4);
                            asmbuf_puts(&buf, xh_x86_rule("text.44b5e9f85f2e61a3"));
                        } else {
                            asmbuf_puts(&buf, xh_x86_rule("text.f11eba8d7ffdb259"));
                            asmbuf_int(&buf, stk);
                            asmbuf_puts(&buf, xh_x86_rule("text.435527643a0ff4db"));
                            asmbuf_int(&buf, dst);
                            asmbuf_puts(&buf, xh_x86_rule("text.44b5e9f85f2e61a3"));
                        }
                    }
                    break;
                }
                case IR_PUSH_INT:
                    asmbuf_puts(&buf, xh_x86_rule("text.276c8636c367c6c1"));
                    asmbuf_int(&buf, I->val);
                    asmbuf_putc(&buf, '\n');
                    break;
                case IR_PUSH_STR:
                    asmbuf_puts(&buf, xh_x86_rule("text.8828c73fbe316b78"));
                    asmbuf_int(&buf, I->val);
                    asmbuf_puts(&buf, xh_x86_rule("text.95b030d0ad1483d7"));
                    break;
                case IR_PUSH_VAR:
                    if ((I->aux & 0xFF) == 16) {
                        x86_complex_unsupported(&buf, "local variable");
                        break;
                    }
                    /* _Complex float is 8 bytes but occupies two doubles on the
                     * IR stack; a single dword load would be a silently short
                     * value, so refuse it here as well. */
                    if ((I->aux & IR_AUX_COMPLEX) && (I->aux & 0xFF) == 8) {
                        x86_complex_unsupported(&buf, "_Complex float local variable");
                        break;
                    }
                    if ((I->aux & 0xFF) == 8) {
                        /* double / long long local: two words with the HIGH
                         * one on top, the same shape IR_LOAD_IDX builds and the
                         * order irgen, ARM, RISC-V and AMD64 use.  Pushing
                         * only the low dword lost the high half of every
                         * 64-bit local, which is why `double r = f(); return
                         * (int)r;` answered 0. */
                        asmbuf_puts(&buf, xh_x86_rule("text.6bd382e4237dd86d"));
                        asmbuf_int(&buf, framelo - (int)I->val);
                        asmbuf_puts(&buf, xh_x86_rule("text.19c91e923eb5a4e0"));
                        asmbuf_int(&buf, framelo - (int)I->val - 4);
                        asmbuf_puts(&buf, xh_x86_rule("text.f9416e09f78ba316"));
                    } else {
                        asmbuf_puts(&buf, xh_x86_rule("text.6bd382e4237dd86d"));
                        asmbuf_int(&buf, framelo - (int)I->val);
                        asmbuf_puts(&buf, xh_x86_rule("text.f9416e09f78ba316"));
                    }
                    break;
                case IR_PUSH_ADDR:
                    asmbuf_puts(&buf, xh_x86_rule("text.137dbb4064852a52"));
                    asmbuf_int(&buf, framelo - (int)I->val);
                    asmbuf_puts(&buf, xh_x86_rule("text.95b030d0ad1483d7"));
                    break;
                case IR_PUSH_GLOBAL:
                    if (x86_global_size(prog, I->name) == 16) {
                        x86_complex_unsupported(&buf, "global variable");
                        break;
                    }
                    if ((I->aux & IR_AUX_COMPLEX) &&
                        x86_global_size(prog, I->name) == 8) {
                        x86_complex_unsupported(&buf, "_Complex float global variable");
                        break;
                    }
                    /* Bug-X4: For variadic functions, _ark_va_ptr points to
                     * the variadic tail on the stack. */
                    if (f->is_variadic && I->name && ark_streq(I->name, "_ark_va_ptr")) {
                        asmbuf_puts(&buf, xh_x86_rule("text.33cb64287aab8b51"));
                        asmbuf_int(&buf, 8 + f->param_count * 4);
                        asmbuf_puts(&buf, xh_x86_rule("text.95b030d0ad1483d7"));
                    } else if (x86_global_size(prog, I->name) == 8) {
                        asmbuf_puts(&buf, xh_x86_rule("text.ef531c7385208fe7"));
                        asmbuf_puts(&buf, I->name);
                        asmbuf_puts(&buf, xh_x86_rule("text.a891f632b224608d"));
                        asmbuf_puts(&buf, I->name);
                        asmbuf_puts(&buf, xh_x86_rule("text.5f7fcd0f3f66393f"));
                    } else {
                        asmbuf_puts(&buf, xh_x86_rule("text.ef531c7385208fe7"));
                        asmbuf_puts(&buf, I->name);
                        asmbuf_puts(&buf, xh_x86_rule("text.95b030d0ad1483d7"));
                    }
                    break;
                case IR_STORE_GLOBAL:
                    if (x86_global_size(prog, I->name) == 16) {
                        x86_complex_unsupported(&buf, "global variable");
                        break;
                    }
                    if ((I->aux & IR_AUX_COMPLEX) &&
                        x86_global_size(prog, I->name) == 8) {
                        x86_complex_unsupported(&buf, "_Complex float global variable");
                        break;
                    }
                    if (x86_global_size(prog, I->name) == 8) {
                        asmbuf_puts(&buf, xh_x86_rule("text.c037dc4b55d9e75e"));
                        asmbuf_puts(&buf, I->name);
                        asmbuf_puts(&buf, xh_x86_rule("text.27fe47257c0ddba9"));
                        asmbuf_puts(&buf, I->name);
                        asmbuf_puts(&buf, xh_x86_rule("text.a8e65c3e4bfced27"));
                    } else {
                        asmbuf_puts(&buf, xh_x86_rule("text.0bcc61c6f37c286f"));
                        asmbuf_puts(&buf, I->name);
                        asmbuf_puts(&buf, xh_x86_rule("text.44b5e9f85f2e61a3"));
                    }
                    break;
                case IR_POP:
                    asmbuf_puts(&buf, xh_x86_rule("text.290450c437d66e16"));
                    break;
                case IR_DUP:
                    if (I->val > 1) {
                        /* 8/16-byte value: one dword copy would leave the rest
                         * of the value behind and drift the operand stack. */
                        x86_complex_unsupported(&buf, "duplicate");
                        break;
                    }
                    asmbuf_puts(&buf, xh_x86_rule("text.d46e72ff4e3a07dc"));
                    break;
                case IR_SWAP:
                    asmbuf_puts(&buf, xh_x86_rule("text.5982900ff1bbf03d"));
                    break;
                case IR_STORE:
                    if ((I->aux & ~(IR_AUX_VOLATILE | IR_AUX_COMPLEX)) == 16) {
                        x86_complex_unsupported(&buf, "store");
                        break;
                    }
                    if ((I->aux & IR_AUX_COMPLEX) &&
                        (I->aux & ~(IR_AUX_VOLATILE | IR_AUX_COMPLEX)) == 8) {
                        x86_complex_unsupported(&buf, "_Complex float store");
                        break;
                    }
                    if ((I->aux & ~(IR_AUX_VOLATILE | IR_AUX_COMPLEX)) == 8) {
                        /* double / long long: pop both words (high on top). */
                        asmbuf_puts(&buf, xh_x86_rule("text.af0ef5ac0c7ee97d"));
                        asmbuf_int(&buf, framelo - (int)I->val);
                        asmbuf_puts(&buf, xh_x86_rule("text.2954163038f96120"));
                        asmbuf_int(&buf, framelo - (int)I->val - 4);
                        asmbuf_puts(&buf, xh_x86_rule("text.69b61f412b288a0a"));
                    } else {
                        asmbuf_puts(&buf, xh_x86_rule("text.75ab89ac001efdf8"));
                        asmbuf_int(&buf, framelo - (int)I->val);
                        asmbuf_puts(&buf, xh_x86_rule("text.44b5e9f85f2e61a3"));
                    }
                    break;
                case IR_LOAD_IDX: {
                    /* Pop index (ecx), pop base (eax). Load element at
                     * base + index*es. I->val = element size (1/2/4/8).
                     * I->aux bit 0x1 = unsigned (use movzx vs movsx). */
                    int es = I->val;
                    int is_unsigned = (I->aux & 0x1) != 0;
                    if (es == 16) {
                        x86_complex_unsupported(&buf, "array element load");
                        break;
                    }
                    if (es == 8 && (I->aux & IR_AUX_COMPLEX)) {
                        x86_complex_unsupported(&buf, "_Complex float array element load");
                        break;
                    }
                    asmbuf_puts(&buf, xh_x86_rule("text.5c895cb3ddd81f93"));
                    if (es > 1) {
                        asmbuf_puts(&buf, xh_x86_rule("text.5fbcbeada57c9106"));
                        asmbuf_int(&buf, es);
                        asmbuf_puts(&buf, xh_x86_rule("text.ebf104924e17f6a3"));
                    }
                    if (es == 1) {
                        if (is_unsigned)
                            asmbuf_puts(&buf, xh_x86_rule("text.9b1da4486bd2eb60"));
                        else
                            asmbuf_puts(&buf, xh_x86_rule("text.6a8395334660231c"));
                    } else if (es == 2) {
                        if (is_unsigned)
                            asmbuf_puts(&buf, xh_x86_rule("text.060019c7f5acdbda"));
                        else
                            asmbuf_puts(&buf, xh_x86_rule("text.2efe1d82233884e4"));
                    } else if (es == 8) {
                        /* 64-bit load: push low then high dword, so the high
                         * one ends up on top (canonical order). */
                        asmbuf_puts(&buf, xh_x86_rule("text.b5060857db8145cb"));
                        asmbuf_puts(&buf, xh_x86_rule("text.81deec0800941cf6"));
                        asmbuf_puts(&buf, xh_x86_rule("text.a5cb6273b64f6e59"));
                        asmbuf_puts(&buf, xh_x86_rule("text.2ac65d444c4eb237"));
                        break;
                    } else {
                        /* 4-byte (default): a dword load already writes all of
                         * eax, so signed and unsigned agree. "movsx eax, dword
                         * ptr [eax]" is a 32->32 sign extension, which does not
                         * exist and is rejected by GNU as, so never emit it. */
                        asmbuf_puts(&buf, xh_x86_rule("text.cb61e4998ba6c4dc"));
                    }
                    asmbuf_puts(&buf, xh_x86_rule("text.2ac65d444c4eb237"));
                    break;
                }
                case IR_STORE_IDX: {
                    /* Pop value (edx), pop address (eax). Store value.
                     * I->val = element size (1/2/4/8). */
                    int es = I->val;
                    if (es == 16) {
                        x86_complex_unsupported(&buf, "array element store");
                        break;
                    }
                    if (es == 8 && (I->aux & IR_AUX_COMPLEX)) {
                        x86_complex_unsupported(&buf, "_Complex float array element store");
                        break;
                    }
                    if (es == 8) {
                        /* 64-bit store: the value sits high-dword-on-top, so
                         * the pops are high, low, address.  The value is put
                         * back on the IR stack because an assignment is an
                         * expression -- the old code pushed one word only,
                         * which drifted the operand stack by 4 bytes. */
                        asmbuf_puts(&buf, xh_x86_rule("text.bca946fd76ebfa8a"));
                        asmbuf_puts(&buf, xh_x86_rule("text.6cd919c68ae2a155"));
                        asmbuf_puts(&buf, xh_x86_rule("text.1f46d6d8734ffe60"));
                        asmbuf_puts(&buf, xh_x86_rule("text.8ad98fe039692ddc"));
                    } else {
                        asmbuf_puts(&buf, xh_x86_rule("text.6f9c8b71fa5adc5a"));
                        if (es == 1)
                            asmbuf_puts(&buf, xh_x86_rule("text.70749b316408a8d4"));
                        else if (es == 2)
                            asmbuf_puts(&buf, xh_x86_rule("text.f7039d10f7efbef2"));
                        else
                            asmbuf_puts(&buf, xh_x86_rule("text.4ed9f77143aebefe"));
                        asmbuf_puts(&buf, xh_x86_rule("text.81deec0800941cf6"));
                    }
                    break;
                }
                case IR_BINARY: {
 x86_scalar_rule(&buf,I->aux & 0xFF,(I->aux & 0x100)!=0,32);
 break;
 }
                case IR_UNARY:
                    if (I->aux & IR_AUX_COMPLEX) {
                        /* __builtin conj() of a _Complex value. */
                        x86_complex_unsupported(&buf, "conj");
                        break;
                    }
                    if (I->val == 2) {
                        /* 64-bit integer unary: two words, HIGH one on top.
                         * A single-word `neg` here only touched the high half,
                         * so `-1000000000LL` came out as +1000000000 and every
                         * signed 64-bit divide / shift derived from a negated
                         * literal was silently wrong. */
                        if (I->aux == (int)TOK_MINUS) {
                            asmbuf_puts(&buf, xh_x86_rule("text.b716e4426333894b"));
                        } else if (I->aux == (int)TOK_BNOT) {
                            asmbuf_puts(&buf, xh_x86_rule("text.662530fc69420395"));
                        } else if (I->aux == (int)TOK_NOT) {
                            asmbuf_puts(&buf, xh_x86_rule("text.a78df3bfcd4a7ecc"));
                        } else {
                            asmbuf_puts(&buf, xh_x86_rule("text.ec2340ca8c1a28c7"));
                        }
                        break;
                    }
                    asmbuf_puts(&buf, xh_x86_rule("text.4e39c3f7bd9309f5"));
                    /* val==1 is a float/double `-x`.  Both are held as a double
                     * on this stack with the HIGH dword on top, and that dword
                     * carries the sign bit, so the integer `neg` (which returns
                     * +0.0 for -0.0 and wrecks the exponent) is replaced by a
                     * sign-bit flip. */
                    if (I->val == 1 && I->aux == (int)TOK_MINUS)
                        asmbuf_puts(&buf, xh_x86_rule("text.974cdc0414e43ced"));
                    else if (I->aux == (int)TOK_MINUS) asmbuf_puts(&buf, xh_x86_rule("text.e29bd740543bbe7f"));
                    else if (I->aux == (int)TOK_BNOT) asmbuf_puts(&buf, xh_x86_rule("text.cd9069b2dc376ed8"));
                    else if (I->aux == (int)TOK_NOT) asmbuf_puts(&buf, xh_x86_rule("text.fecfa68ae8bbca42"));
                    asmbuf_puts(&buf, xh_x86_rule("text.2ac65d444c4eb237"));
                    break;
                case IR_LABEL:
                    asmbuf_puts(&buf, xh_x86_rule("text.4b6d79fc377f87b4"));
                    asmbuf_puts(&buf, f->name);
                    asmbuf_putc(&buf, '_');
                    asmbuf_int(&buf, I->val);
                    asmbuf_puts(&buf, xh_x86_rule("text.f3743a0a18e53d13"));
                    break;
                case IR_LABEL_NAMED:
                    asmbuf_puts(&buf, xh_x86_rule("text.be1de5990bdec1cd"));
                    asmbuf_puts(&buf, f->name);
                    asmbuf_putc(&buf, '_');
                    asmbuf_puts(&buf, I->name);
                    asmbuf_puts(&buf, xh_x86_rule("text.f3743a0a18e53d13"));
                    break;
                case IR_JMP:
                    asmbuf_puts(&buf, xh_x86_rule("text.02b129850dd58dc5"));
                    asmbuf_puts(&buf, f->name);
                    asmbuf_putc(&buf, '_');
                    asmbuf_int(&buf, I->val);
                    asmbuf_putc(&buf, '\n');
                    break;
                case IR_JMP_NAMED:
                    asmbuf_puts(&buf, xh_x86_rule("text.66fe547640031826"));
                    asmbuf_puts(&buf, f->name);
                    asmbuf_putc(&buf, '_');
                    asmbuf_puts(&buf, I->name);
                    asmbuf_putc(&buf, '\n');
                    break;
                case IR_JZ:
                    asmbuf_puts(&buf, xh_x86_rule("text.209d29ef9eaf9e34"));
                    asmbuf_puts(&buf, f->name);
                    asmbuf_putc(&buf, '_');
                    asmbuf_int(&buf, I->val);
                    asmbuf_putc(&buf, '\n');
                    break;
                case IR_JNZ:
                    asmbuf_puts(&buf, xh_x86_rule("text.bf8c0396346ecd37"));
                    asmbuf_puts(&buf, f->name);
                    asmbuf_putc(&buf, '_');
                    asmbuf_int(&buf, I->val);
                    asmbuf_putc(&buf, '\n');
                    break;
                case IR_CMP_JMP_EQ: {
                    /* The internal assembler has no numeric local labels:
                     * "1:" defines a symbol literally named "1", so the
                     * `jne 1f` this used to emit never resolved.  Use a
                     * per-function name instead. */
                    int lcj = ++lbl64;
                    asmbuf_puts(&buf, xh_x86_rule("text.a9fdeff000117537"));
                    asmbuf_int(&buf, I->val);
                    asmbuf_puts(&buf, xh_x86_rule("text.097c31cac98ba0d2"));
                    x86_elbl(&buf, f->name, "cj", lcj, 0);
                    asmbuf_puts(&buf, xh_x86_rule("text.6b3942073184b317"));
                    asmbuf_puts(&buf, f->name);
                    asmbuf_putc(&buf, '_');
                    asmbuf_int(&buf, I->aux);
                    asmbuf_putc(&buf, '\n');
                    x86_elbl(&buf, f->name, "cj", lcj, 1);
                    break;
                }
                case IR_CALL: {
                    /* cdecl calling convention: all arguments passed on stack
                     * (right to left), caller cleans up via saved esp restore.
                     *   - Pop all args to scratch area
                     *   - Save esp
                     *   - Push args right-to-left from scratch
                     *   - Align to 16 bytes (for SSE)
                     *   - Call
                     *   - Restore esp (cleans up pushed args + alignment) */
                    int n = (int)I->val;
                    int argmask = I->aux;
                    int is_indirect = (I->name && I->name[0] == '?');
                    int k;
                    int stack_cnt = 0;
                    /* 16 bytes below the bottom of the local area, so the
                     * saved-esp slot can never overlap the lowest local. */
                    int scratch_base = framelo + xh_x86_number("abi.frame.call_scratch_gap");
                    int is_void_ret = 0;
                    int sret_call = (int)((I->val >> 36) & 1);
                    int sret_buf_off = 0;
                    int sret_buf_sz = 0;
                    const int *wires = (I->aux2 > 0) ? &prog->argbytes[I->aux2 - 1] : 0;
                    int wb[64];
                    int words_total = 0;
                    (void)argmask;  /* cdecl ignores float/int distinction for arg passing */

                    /* For indirect calls, the callee pointer was pushed last
                     * (arg n-1 on the virtual stack). Pop it into ecx
                     * (caller-saved scratch register). */
                    if (is_indirect) {
                        asmbuf_puts(&buf, xh_x86_rule("text.fe2690f9273802de"));
                        n = n - 1;
                    }
                    if (sret_call) {
                        if (sret_site_cur < sret_site_n) {
                            sret_buf_off = sret_off_tab[sret_site_cur];
                            sret_buf_sz = sret_sz_tab[sret_site_cur];
                            sret_site_cur++;
                        } else {
                            ark_log("[x86] error: more struct-returning calls in '%s' "
                                    "than the frame reserved\n", f->name);
                            x86_emit_errors++;
                            sret_call = 0;
                        }
                    }
                    /* The call-scratch area holds 4 bytes per argument; it ends
                     * where the sret buffers begin.  Overflow is an error, not
                     * a silent overlap. */
                    if (n > xh_x86_number("abi.call.i386_argument_limit")) {
                        ark_log("[x86] error: call in '%s' passes more than 120 "
                                "arguments\n", f->name);
                        x86_emit_errors++;
                        break;
                    }
                    /* Width of every argument word on the wire.  i386 keeps
                     * each floating value as a 2-word double on the IR stack
                     * (IR_PUSH_FLOAT promotes float constants), so a `float`
                     * parameter is 8 bytes here even though sizeof is 4 --
                     * IR_GETARG reads it with the same table.  Without this the
                     * pop loop moved one word per *argument*, so f(1.5,2.25)
                     * handed the callee the two low words and left the high
                     * words on the stack, and every double/float/long long
                     * argument lost its upper half. */
                    {
                        int wi, ptn = 0, found_pt = 0;
                        Type **pt = 0;
                        if (!is_indirect) {
                            for (wi = 0; wi < prog->func_count; wi++) {
                                if (ark_streq(prog->funcs[wi]->name, I->name)) {
                                    pt = prog->funcs[wi]->param_types;
                                    ptn = prog->funcs[wi]->param_count;
                                    found_pt = 1;
                                    break;
                                }
                            }
                        }
                        for (wi = 0; wi < n; wi++) {
                            int b = 4;
                            if (wi < 64) {
                                if (wires) b = wires[wi] & 0xFFFF;
                                else if (wi < 16 && ((argmask >> (2 * wi + 1)) & 1)) b = 8;
                            }
                            if (found_pt && pt && wi < ptn && pt[wi]) {
                                Type *t = pt[wi];
                                int ts = type_size(t);
                                if (t->kind == TYPE_DOUBLE || type_is_64bit_integer(t))
                                    b = 8;
                                else if (t->kind == TYPE_FLOAT)
                                    b = 4;
                                else if (t->kind == TYPE_STRUCT || t->kind == TYPE_UNION)
                                    b = xh_x86_aggregate_wire(ts);
                                else
                                    b = 4;
                            }
                            if (b <= 0) b = 4;
                            if (wi < 64) {
                                wb[wi] = b;
                                words_total += (b + 3) / 4;
                            }
                        }
                    }

                    /* The call-scratch area holds 4 bytes per argument word;
                     * it ends where the sret buffers begin.  The check has to
                     * run here, after the width table is built -- it used to
                     * sit above the loop that computes words_total, so it
                     * compared an initialised 0 and could never fire. */
                    if (words_total > xh_x86_number("abi.call.max_words")) {
                        ark_log("[x86] error: call in '%s' needs %d argument words "
                                "but the call-scratch area holds 116\n",
                                f->name, words_total);
                        x86_emit_errors++;
                        break;
                    }

                    /* Pop every argument word into the scratch area.  Slot k
                     * holds wire word k, and slot 0 ends up at [esp] so the
                     * callee reads it as its first argument.  A 2-word
                     * argument arrives high-dword-on-top, so its words are
                     * stored in the opposite order -- that is the internal to
                     * cdecl conversion, which is why IR_CDECL64 is a no-op in
                     * this backend. */
                    {
                        int ai2;
                        for (ai2 = 0; ai2 < n; ai2++) {
                            int w = (ai2 < 64) ? (wb[ai2] + 3) / 4 : 1;
                            if (w == 2) {
                                asmbuf_puts(&buf, xh_x86_rule("text.75ab89ac001efdf8"));
                                asmbuf_int(&buf, scratch_base + xh_x86_number("abi.frame.call_scratch_gap") + xh_x86_number("abi.ir.word") * (stack_cnt + 1));
                                asmbuf_puts(&buf, xh_x86_rule("text.d67539bdfd59d10f"));
                                asmbuf_int(&buf, scratch_base + xh_x86_number("abi.frame.call_scratch_gap") + xh_x86_number("abi.ir.word") * stack_cnt);
                                asmbuf_puts(&buf, xh_x86_rule("text.44b5e9f85f2e61a3"));
                                stack_cnt += 2;
                            } else {
                                int wi3;
                                for (wi3 = 0; wi3 < w; wi3++) {
                                    asmbuf_puts(&buf, xh_x86_rule("text.75ab89ac001efdf8"));
                                    asmbuf_int(&buf, scratch_base + xh_x86_number("abi.frame.call_scratch_gap") + xh_x86_number("abi.ir.word") * stack_cnt);
                                    asmbuf_puts(&buf, xh_x86_rule("text.44b5e9f85f2e61a3"));
                                    stack_cnt++;
                                }
                            }
                        }
                    }

                    /* Save esp before stack adjustments. */
                    asmbuf_puts(&buf, xh_x86_rule("text.fc1d629ae8b91275"));
                    asmbuf_int(&buf, scratch_base);
                    asmbuf_puts(&buf, xh_x86_rule("text.cd62d6384497a495"));

                    /* 16-byte alignment for the call (an external cdecl callee
                     * may use movaps).  It has to happen *before* the
                     * arguments are pushed: padding afterwards meant
                     * "and esp, -16" moved esp below the arguments it had just
                     * pushed, and the callee read padding as [ebp+8] -- i.e.
                     * every i386 call passed garbage whenever esp was not
                     * already aligned.  With N arguments pushed, the pad is
                     * (esp - 4N) mod 16, which leaves esp a multiple of 16 at
                     * the call instruction. */
                    asmbuf_puts(&buf, xh_x86_rule("text.83259b2ffb2f8cc1"));
                    if (stack_cnt + (sret_call ? 1 : 0) > 0) {
                        asmbuf_puts(&buf, xh_x86_rule("text.4a0fe7f169d9b9fd"));
                        asmbuf_int(&buf, xh_x86_number("abi.ir.word") * (stack_cnt + (sret_call ? 1 : 0)));
                        asmbuf_putc(&buf, '\n');
                    }
                    asmbuf_puts(&buf, xh_x86_rule("text.d0a85d0a7a5f8b57"));

                    /* Push stack args in reverse order (right to left) so
                     * leftmost arg is at [esp] (lowest address). */
                    for (k = stack_cnt - 1; k >= 0; k--) {
                        asmbuf_puts(&buf, xh_x86_rule("text.6bd382e4237dd86d"));
                        asmbuf_int(&buf, scratch_base + xh_x86_number("abi.frame.call_scratch_gap") + xh_x86_number("abi.ir.word") * k);
                        asmbuf_puts(&buf, xh_x86_rule("text.f9416e09f78ba316"));
                    }
                    /* Hidden first argument: the address of this site's result
                     * buffer.  Pushed last so it sits at [esp] = [ebp+8] in the
                     * callee, where the sret prologue reads it. */
                    if (sret_call) {
                        asmbuf_puts(&buf, xh_x86_rule("text.137dbb4064852a52"));
                        asmbuf_int(&buf, sret_area_off + sret_buf_off + sret_buf_sz);
                        asmbuf_puts(&buf, xh_x86_rule("text.95b030d0ad1483d7"));
                    }

                    /* For direct calls, check if the callee returns void.
                     * If so, skip the push of the return value. */
                    if (!is_indirect) {
                        int fi2;
                        int found = 0;
                        for (fi2 = 0; fi2 < prog->func_count; fi2++) {
                            if (ark_streq(prog->funcs[fi2]->name, I->name)) {
                                Type *rt = prog->funcs[fi2]->ret_type;
                                found = 1;
                                if (rt && rt->kind == TYPE_VOID)
                                    is_void_ret = 1;
                                break;
                            }
                        }
                        /* Declared-only callee: irgen knows the prototype, so
                         * take the result shape from the call site. */
                        if (!found)
                            is_void_ret = (int)((I->val >> 33) & 1);
                    }

                    if (is_indirect) {
                        asmbuf_puts(&buf, xh_x86_rule("text.039b60d9ab552cfd"));
                    } else {
                        asmbuf_puts(&buf, xh_x86_rule("text.7e873a94c44eeb9e"));
                        asmbuf_puts(&buf, I->name);
                        asmbuf_putc(&buf, '\n');
                    }

                    /* Restore esp from saved value (cleans up pushed args +
                     * alignment padding in one step). */
                    asmbuf_puts(&buf, xh_x86_rule("text.b237eaa684af0e1e"));
                    asmbuf_int(&buf, scratch_base);
                    asmbuf_puts(&buf, xh_x86_rule("text.f9416e09f78ba316"));

                    if (sret_call) is_void_ret = 0;
                    if (!is_void_ret) {
                        /* bit 53: the result came back in xmm0, bit 32: it is
                         * two words (edx:eax) -- the same encoding irgen uses
                         * for the AMD64 backend.  Pushing `eax` alone dropped
                         * every double return's high half and every 64-bit
                         * integer result's upper word. */
                        int retfloat = (int)((I->val >> 53) & 1);
                        int ret2 = (int)((I->val >> 32) & 1);
                        if (retfloat && !ret2)
                            asmbuf_puts(&buf, xh_x86_rule("text.bfbf7eb10d65f0c1"));
                        else if (retfloat) {
                            /* A double comes back in xmm0; store it and flip the
                             * two dwords so the high one is on top, the order
                             * every other 64-bit value on this stack uses. */
                            asmbuf_puts(&buf, xh_x86_rule("text.7674ef19adb6c5e8"));
                            x86_pair_swap_at(&buf, 0);
                        } else if (ret2) {
                            /* edx:eax is high:low, so the low word is pushed
                             * first and the high word ends up on top. */
                            asmbuf_puts(&buf, xh_x86_rule("text.a7a351cdc205c8ef"));
                        }
                        else
                            asmbuf_puts(&buf, xh_x86_rule("text.2ac65d444c4eb237"));
                    }
                    break;
                }
                case IR_RETURN:
                    if (sret_off) {
                        /* sret return: irgen pushes the *address* of the object
                         * being returned, so the copy into the caller's buffer
                         * belongs here (the AMD64 path does the same).  Without
                         * it the callee only handed back the buffer pointer and
                         * `struct S r = mk(1)` read uninitialised memory --
                         * e.g. s12b (1,2,3) came back as 3952145335.  rep movsb
                         * clobbers edi/esi/ecx, all of which are either
                         * callee-saved and restored below, or dead here. */
                        int rsz = f->ret_type ? type_size(f->ret_type) : 0;
                        if (rsz <= 0) rsz = 8;
                        asmbuf_puts(&buf, xh_x86_rule("text.2e89ab613cfb8cad"));
                        asmbuf_int(&buf, sret_off);
                        asmbuf_puts(&buf, xh_x86_rule("text.bf5c869475327afb"));
                        asmbuf_int(&buf, rsz);
                        asmbuf_puts(&buf, xh_x86_rule("text.ce846d3feb14f824"));
                        asmbuf_int(&buf, sret_off);
                        asmbuf_puts(&buf, xh_x86_rule("text.cf34b3c5e2d0a761"));
                    } else {
                        Type *rt = f->ret_type;
                        int is_fp = rt && (rt->kind == TYPE_FLOAT ||
                                           rt->kind == TYPE_DOUBLE);
                        int two = rt && ((type_is_64bit_integer(rt) ||
                                          rt->kind == TYPE_DOUBLE) ||
                                         ((rt->kind == TYPE_STRUCT ||
                                           rt->kind == TYPE_UNION) &&
                                          type_size(rt) > xh_x86_number("abi.ir.word") && type_size(rt) <= xh_x86_number("abi.aggregate.direct_limit")));
                        if (is_fp && rt->kind == TYPE_FLOAT) {
                            asmbuf_puts(&buf, xh_x86_rule("text.4afef8bb740fd333"));
                        } else if (is_fp) {
                            /* A double result travels in xmm0, the mirror of
                             * the call site's bit-53 handling.  `pop eax` used
                             * to hand back only the low word.  The two words
                             * arrive high-dword-on-top, so they are flipped
                             * before movq reads them as a blob. */
                            x86_pair_swap_at(&buf, 0);
                            asmbuf_puts(&buf, xh_x86_rule("text.c0d9bc14821121d4"));
                        } else if (two) {
                            /* 64-bit scalars and 5..8 byte composites come back
                             * in edx:eax (high:low); the IR stack keeps the high
                             * word on top, so edx is popped first. */
                            asmbuf_puts(&buf, xh_x86_rule("text.6f9c8b71fa5adc5a"));
                        } else {
                            asmbuf_puts(&buf, xh_x86_rule("text.4e39c3f7bd9309f5"));
                        }
                        asmbuf_puts(&buf, xh_x86_rule("text.1dae71cae98dd498"));
                    }
                    break;
                /* === B1: Missing IR operations for completeness === */

                case IR_LOCAL_ALLOC:
                    break;
                case IR_ARG:
                    break;

                /* IR_CDECL64: convert one 64-bit value from the internal
                 * order (high dword on top) to cdecl's (low dword first).
                 * Every 2-word value this backend builds -- and every cdecl
                 * argument -- is already in the internal order, with the swap
                 * folded into the call site's argument shuffle, so this is a
                 * no-op.  Falling through to `default` printed "# unknown IR
                 * op" and emitted nothing, which silently kept the wrong
                 * order on the wire. */
                case IR_CDECL64:
                    break;

                /* IR_SEXT / IR_ZEXT: widen a 32-bit value on the stack to 64
                 * bits.  Both used to fall through to `default` (no code at
                 * all), so `(long long)some_int` left the upper half as
                 * whatever happened to be below it on the IR stack. */
                case IR_SEXT:
                    asmbuf_puts(&buf, xh_x86_rule("text.206a9e43d351ab6f"));
                    break;
                case IR_ZEXT:
                    asmbuf_puts(&buf, xh_x86_rule("text.a001949c6c8f2386"));
                    break;

                /* IR_TRUNC: val = target width, aux = 1 zero-extend. */
                case IR_TRUNC:
                    asmbuf_puts(&buf, xh_x86_rule("text.4e39c3f7bd9309f5"));
                    if (I->val == 8)
                        asmbuf_puts(&buf, I->aux ? xh_x86_rule("text.344ade76d4acecae") : xh_x86_rule("text.fe88e51f8712bfcd"));
                    else if (I->val == 16)
                        asmbuf_puts(&buf, I->aux ? xh_x86_rule("text.edd53f9a0d5b9f63") : xh_x86_rule("text.8832c5e95cb1e8dd"));
                    asmbuf_puts(&buf, xh_x86_rule("text.2ac65d444c4eb237"));
                    break;

                /* IR_PUSH_FUNC: push the address of a function symbol. */
                case IR_PUSH_FUNC:
                    asmbuf_puts(&buf, xh_x86_rule("text.98f95df9e907f704"));
                    asmbuf_puts(&buf, I->name);
                    asmbuf_puts(&buf, xh_x86_rule("text.95b030d0ad1483d7"));
                    break;

                /* IR_PUSH_GLOBAL_ADDR: push &global (no dereference). */
                case IR_PUSH_GLOBAL_ADDR:
                    asmbuf_puts(&buf, xh_x86_rule("text.98f95df9e907f704"));
                    asmbuf_puts(&buf, I->name);
                    asmbuf_puts(&buf, xh_x86_rule("text.95b030d0ad1483d7"));
                    break;

                /* IR_MEMCPY: pop dst, pop src, copy I->val bytes.
                 * Uses edi (dest), esi (src), ecx (count) — all saved in prologue. */
                case IR_MEMCPY:
                    asmbuf_puts(&buf, xh_x86_rule("text.e71a0aee15f5fc12"));
                    asmbuf_int(&buf, I->val);
                    asmbuf_puts(&buf, xh_x86_rule("text.8d3fc2867d2efe58"));
                    break;

                /* IR_ALLOCA: VLA dynamic stack alloc.
                 * Pop byte count, align to 16, subtract from esp, push base. */
                case IR_ALLOCA:
                    asmbuf_puts(&buf, xh_x86_rule("text.8335dbd8a8d11c9e"));
                    asmbuf_puts(&buf, xh_x86_rule("text.0d64a5d6db372b81"));
                    break;

                /* IR_DIV / IR_MOD: integer division.
                 * aux=0 signed, aux=1 unsigned. */
                case IR_DIV:
                    asmbuf_puts(&buf, xh_x86_rule("text.5c895cb3ddd81f93"));
                    if (I->aux & 1)
                        asmbuf_puts(&buf, xh_x86_rule("text.c03dab15d958e712"));
                    else
                        asmbuf_puts(&buf, xh_x86_rule("text.794267a5f7c573da"));
                    break;
                case IR_MOD:
                    asmbuf_puts(&buf, xh_x86_rule("text.5c895cb3ddd81f93"));
                    if (I->aux & 1)
                        asmbuf_puts(&buf, xh_x86_rule("text.a303d5ea75714959"));
                    else
                        asmbuf_puts(&buf, xh_x86_rule("text.13386fbfacf1a09a"));
                    break;

                /* IR_PUSH_FLOAT: push a float/double constant.
                 * aux bit 0x200 = double.  val = index into prog->floats[].
                 * On x86-32, doubles are 8 bytes (pushed as two dwords). */
                case IR_PUSH_FLOAT: {
                    int is_dbl = (I->aux & 0x200) != 0;
                    /* IR_AUX_COMPLEX, not the old 0x400: irgen puts 0x200 too
                     * (a complex constant is a double pair), so this test has
                     * to run first, exactly like the ARM and RV backends. */
                    int is_complex = (I->aux & IR_AUX_COMPLEX) != 0;
                    if (is_complex) {
                        /* _Complex double: push real then imag (2 slots) */
                        int cid = I->val;
                        if (cid >= 0 && cid < prog->complex_count) {
                            double re = prog->complexes[cid].re;
                            double im = prog->complexes[cid].im;
                            union { double d; long long i; } ure, uim;
                            ure.d = re; uim.d = im;
                            asmbuf_puts(&buf, xh_x86_rule("text.92b99704ac6bafbc"));
                            asmbuf_int(&buf, (int)(ure.i & 0xFFFFFFFF));
                            asmbuf_puts(&buf, xh_x86_rule("text.686bd128725fa74d"));
                            asmbuf_puts(&buf, xh_x86_rule("text.92b99704ac6bafbc"));
                            asmbuf_int(&buf, (int)((ure.i >> 32) & 0xFFFFFFFF));
                            asmbuf_puts(&buf, xh_x86_rule("text.686bd128725fa74d"));
                            asmbuf_puts(&buf, xh_x86_rule("text.92b99704ac6bafbc"));
                            asmbuf_int(&buf, (int)(uim.i & 0xFFFFFFFF));
                            asmbuf_puts(&buf, xh_x86_rule("text.686bd128725fa74d"));
                            asmbuf_puts(&buf, xh_x86_rule("text.92b99704ac6bafbc"));
                            asmbuf_int(&buf, (int)((uim.i >> 32) & 0xFFFFFFFF));
                            asmbuf_puts(&buf, xh_x86_rule("text.686bd128725fa74d"));
                        } else {
                            asmbuf_puts(&buf, xh_x86_rule("text.2809e76015d3b65b"));
                        }
                    } else if (is_dbl) {
                        double v = (I->val >= 0 && I->val < prog->float_count) ?
                                   prog->floats[I->val] : 0.0;
                        union { double d; long long i; } u;
                        u.d = v;
                        asmbuf_puts(&buf, xh_x86_rule("text.92b99704ac6bafbc"));
                        asmbuf_int(&buf, (int)(u.i & 0xFFFFFFFF));
                        asmbuf_puts(&buf, xh_x86_rule("text.686bd128725fa74d"));
                        asmbuf_puts(&buf, xh_x86_rule("text.92b99704ac6bafbc"));
                        asmbuf_int(&buf, (int)((u.i >> 32) & 0xFFFFFFFF));
                        asmbuf_puts(&buf, xh_x86_rule("text.686bd128725fa74d"));
                    } else {
                        /* float: one word, the single-precision bit pattern.
                         * This used to widen the constant to a double and push
                         * two words, which disagreed with the IR model (a
                         * float expression is one stack word) and with
                         * IR_PUSH_VAR/IR_STORE for a float local, so every
                         * `float f = 1.5f;` left a word behind on the IR stack
                         * -- and IR_FADD then popped a double built from the
                         * float's bits plus whatever was underneath. */
                        float fv = (float)((I->val >= 0 && I->val < prog->float_count) ?
                                   prog->floats[I->val] : 0.0);
                        union { float f; unsigned int i; } uf;
                        uf.f = fv;
                        asmbuf_puts(&buf, xh_x86_rule("text.276c8636c367c6c1"));
                        asmbuf_int(&buf, (int)uf.i);
                        asmbuf_putc(&buf, '\n');
                    }
                    break;
                }

                /* IR_FADD/FSUB/FMUL/FDIV: floating-point arithmetic.
                 * Operands are doubles on the stack (8 bytes each = 2 dwords).
                 * Uses SSE2 for computation. */
                case IR_FADD:
                case IR_FSUB:
                case IR_FMUL:
                case IR_FDIV: {
                    const char *op_s = 0;
                    switch (I->op) {
                        case IR_FADD: op_s = "addsd"; break;
                        case IR_FSUB: op_s = "subsd"; break;
                        case IR_FMUL: op_s = "mulsd"; break;
                        case IR_FDIV: op_s = "divsd"; break;
                        default: break;
                    }
                    /* cexp() is a single-operand op that irgen encodes on the
                     * IR_FADD slot with (aux & 0xF00) == 0x900.  Treating it as
                     * a binary complex add would pop one operand too many. */
                    if ((I->aux & 0xF00) == 0x900) {
                        x86_complex_unsupported(&buf, "cexp");
                        break;
                    }
                    /* _Complex double: two ops on real/imag parts */
                    if (I->aux & IR_AUX_COMPLEX) {
                        asmbuf_puts(&buf, xh_x86_rule("text.010d4f88c41190b1"));  /* imag b */
                        asmbuf_puts(&buf, xh_x86_rule("text.e709176edbc858bf"));  /* real b */
                        asmbuf_puts(&buf, xh_x86_rule("text.94c200ba0642c541"));  /* imag a */
                        asmbuf_puts(&buf, xh_x86_rule("text.c0d9bc14821121d4"));  /* real a */
                        asmbuf_puts(&buf, xh_x86_rule("text.1a0f564ddc603945"));
                        asmbuf_puts(&buf, op_s);
                        asmbuf_puts(&buf, xh_x86_rule("text.bd74230bfaf7212e"));
                        asmbuf_puts(&buf, op_s);
                        asmbuf_puts(&buf, xh_x86_rule("text.9668ff9261324f75"));
                        asmbuf_puts(&buf, xh_x86_rule("text.7674ef19adb6c5e8"));
                        asmbuf_puts(&buf, xh_x86_rule("text.4a71ed58a4c7ae1d"));
                    } else if ((I->aux & 0x200) != 0) {
                        x86_pair_swap_at(&buf, 0);
                        asmbuf_puts(&buf, xh_x86_rule("text.010d4f88c41190b1"));
                        x86_pair_swap_at(&buf, 0);
                        asmbuf_puts(&buf, xh_x86_rule("text.e7746556032f1280"));
                        asmbuf_puts(&buf, op_s);
                        asmbuf_puts(&buf, xh_x86_rule("text.cb3f8202914f1fde"));
                        x86_pair_swap_at(&buf, 0);
                    } else {
                        /* float: one word per operand, single precision.
                         * `addsd` on a 4-byte operand read the neighbouring
                         * word as the high half of a double. */
                        const char *ops = "addss";
                        switch (I->op) {
                            case IR_FSUB: ops = "subss"; break;
                            case IR_FMUL: ops = "mulss"; break;
                            case IR_FDIV: ops = "divss"; break;
                            default: ops = "addss"; break;
                        }
                        asmbuf_puts(&buf, xh_x86_rule("text.4c1a9220c6b59e6a"));
                        asmbuf_puts(&buf, xh_x86_rule("text.ffe801bf0d7c4b9c"));
                        asmbuf_puts(&buf, ops);
                        asmbuf_puts(&buf, xh_x86_rule("text.8827580b6366c49c"));
                    }
                    break;
                }

                /* IR_FCMP: compare two doubles, push 0 or 1. */
                case IR_FCMP: {
                    const char *cc = 0;
                    int is_complex = (I->aux & IR_AUX_COMPLEX) != 0;
                    int cmp_op = I->aux & 0xFF;
                    if (is_complex) {
                        /* A _Complex value is four words and this emitter has
                         * no complex code generation at all; the old code
                         * popped three words and answered from the wrong
                         * halves.  Refuse loudly instead of inventing a value.
                         */
                        x86_complex_unsupported(&buf, "_Complex comparison");
                    } else {
                        switch (cmp_op) {
                            case (int)TOK_EQ: cc = "sete";  break;
                            case (int)TOK_NE: cc = "setne"; break;
                            case (int)TOK_LT: cc = "setb";  break;
                            case (int)TOK_GT: cc = "seta";  break;
                            case (int)TOK_LE: cc = "setbe"; break;
                            case (int)TOK_GE: cc = "setae"; break;
                            default: cc = "sete"; break;
                        }
                        if ((I->aux & 0x200) != 0) {
                            /* Flip each operand in place (high dword on top ->
                             * blob order) before reading it with movq. */
                            x86_pair_swap_at(&buf, 0);
                            asmbuf_puts(&buf, xh_x86_rule("text.010d4f88c41190b1"));
                            x86_pair_swap_at(&buf, 0);
                            asmbuf_puts(&buf, xh_x86_rule("text.c0d9bc14821121d4"));
                            asmbuf_puts(&buf, xh_x86_rule("text.dffd78210d40c71d"));
                        } else {
                            /* float: one word per side, ucomiss. */
                            asmbuf_puts(&buf, xh_x86_rule("text.4c1a9220c6b59e6a"));
                            asmbuf_puts(&buf, xh_x86_rule("text.4afef8bb740fd333"));
                            asmbuf_puts(&buf, xh_x86_rule("text.26f1e54784576a84"));
                        }
                        asmbuf_puts(&buf, cc);
                        asmbuf_puts(&buf, xh_x86_rule("text.8d145c1a3edd357d"));
                    }
                    break;
                }

                /* IR_F2I: double -> int (truncate toward zero) */
                case IR_F2I: {
                    if (I->aux & IR_AUX_COMPLEX) {
                        /* creal / cimag / cabs / carg of a _Complex value. */
                        x86_complex_unsupported(&buf, "creal/cimag/cabs/carg");
                        break;
                    }
                    if ((I->aux & 0x200) != 0) {
                        x86_pair_swap_at(&buf, 0);
                        asmbuf_puts(&buf, xh_x86_rule("text.c0d9bc14821121d4"));
                    } else {
                        asmbuf_puts(&buf, xh_x86_rule("text.4afef8bb740fd333"));
                    }
                    /* aux bit 0x400 = the destination is unsigned.  cvttsd2si
                     * is signed-only and answers 0xFFFFFFFE (the integer
                     * indefinite sentinel) for anything >= 2^31, so
                     * `(unsigned)3.5e9` came out as 4294967294.  Split at 2^31:
                     * above it, truncate v - 2^31 and add the bias back. */
                    if (I->aux & 0x400) {
                        int lu = ++lbl64;
                        if (!(I->aux & 0x200))
                            asmbuf_puts(&buf, xh_x86_rule("text.5d9f1cb0416a63f8"));
                        asmbuf_puts(&buf, xh_x86_rule("text.92e14fca1a661366"));
                        x86_elbl(&buf, f->name, "f2u", lu, 0);
                        asmbuf_puts(&buf, xh_x86_rule("text.4d3b5fd5c4d94c07"));
                        x86_elbl(&buf, f->name, "f2u", lu, 1);
                        x86_elbl(&buf, f->name, "f2u", lu, 0);
                        asmbuf_puts(&buf, xh_x86_rule("text.8cc0cd8c22cadc72"));
                        x86_elbl(&buf, f->name, "f2u", lu, 1);
                    } else if ((I->aux & 0x200) != 0) {
                        asmbuf_puts(&buf, xh_x86_rule("text.8cc0cd8c22cadc72"));
                    } else {
                        asmbuf_puts(&buf, xh_x86_rule("text.397d9424d08c5ab8"));
                    }
                    asmbuf_puts(&buf, xh_x86_rule("text.2ac65d444c4eb237"));
                    break;
                }

                /* IR_I2F: int -> double */
                case IR_I2F: {
                    int is_dbl = (I->aux & 0x200) != 0;
                    if (I->aux & IR_AUX_COMPLEX) {
                        /* widening a real value to _Complex double */
                        x86_complex_unsupported(&buf, "real to complex conversion");
                        break;
                    }
                    if (is_dbl) {
                        asmbuf_puts(&buf, xh_x86_rule("text.4e39c3f7bd9309f5"));
                        /* aux bit 0x400 = the source is unsigned.  cvtsi2sd is
                         * signed-only, so `(double)4000000000u` used to
                         * produce -1610612736.0.  x86_u32_to_xmm splits the
                         * low bit off: 2*(u >> 1) + (u & 1). */
                        if (I->aux & 0x400)
                            x86_u32_to_xmm(&buf, "xmm0");
                        else
                            asmbuf_puts(&buf, xh_x86_rule("text.59a1a43ff7880417"));
                        asmbuf_puts(&buf, xh_x86_rule("text.7674ef19adb6c5e8"));
                        x86_pair_swap_at(&buf, 0);
                    } else {
                        /* float result: one word, genuinely single precision.
                         * The old code produced a 2-word double that merely
                         * had float-ish precision, so a float store popped one
                         * word and left the other on the IR stack. */
                        asmbuf_puts(&buf, xh_x86_rule("text.4e39c3f7bd9309f5"));
                        if (I->aux & 0x400) {
                            x86_u32_to_xmm(&buf, "xmm0");
                            asmbuf_puts(&buf, xh_x86_rule("text.c39b283d66519bbc"));
                        } else {
                            asmbuf_puts(&buf, xh_x86_rule("text.8086d640eefab65a"));
                        }
                    }
                    break;
                }

                /* IR_F2F: float <-> double cast.  On i386 every floating
                 * value already lives on the IR stack as a 2-word double (see
                 * IR_PUSH_FLOAT), so widening needs no code at all and
                 * narrowing only has to round to single precision.  Without
                 * this case the emitter printed "# unknown IR op: F2F" and
                 * `double f(float x){return x;}` produced garbage. */
                case IR_F2F: {
                    /* aux 0x100 = float -> double (widen, 1 word -> 2),
                     * aux 0x200 = double -> float (narrow, 2 -> 1).  The old
                     * code printed "# unknown IR op: F2F", so every cast
                     * between the two widths produced garbage. */
                    int src_dbl = (I->aux & 0x200) != 0;
                    int dst_dbl = (I->aux & 0x100) != 0;
                    if (src_dbl && !dst_dbl) {
                        /* Read the 2-word double as a blob (flip first, the
                         * high dword is on top), then narrow. */
                        x86_pair_swap_at(&buf, 0);
                        asmbuf_puts(&buf, xh_x86_rule("text.c896dbafaae43e47"));
                    } else if (!src_dbl && dst_dbl) {
                        asmbuf_puts(&buf, xh_x86_rule("text.b7f0f3dfed7f198f"));
                        x86_pair_swap_at(&buf, 0);
                    }
                    break;
                }

                /* IR_64PUSH: push 64-bit immediate.
                 * val=high32, aux=low32. Pushed as two dwords, low first, so the
                 * high word ends up on top like every other 64-bit value. */
                case IR_64PUSH: {
                    long long v = ((long long)(int)I->val << 32) | (unsigned int)I->aux;
                    asmbuf_puts(&buf, xh_x86_rule("text.92b99704ac6bafbc"));
                    asmbuf_int(&buf, (int)(v & 0xFFFFFFFF));
                    asmbuf_puts(&buf, xh_x86_rule("text.686bd128725fa74d"));
                    asmbuf_puts(&buf, xh_x86_rule("text.92b99704ac6bafbc"));
                    asmbuf_int(&buf, (int)((v >> 32) & 0xFFFFFFFF));
                    asmbuf_puts(&buf, xh_x86_rule("text.686bd128725fa74d"));
                    break;
                }

                /* IR_64BINARY: 64-bit binary op. aux = TOK_xxx | (0x100 if unsigned).
                 * On x86-32, 64-bit ops work on two dword pairs on the stack. */
                case IR_64BINARY: {
                    int op = I->aux & 0xFF;
                    const char *bop = binop_x(op);
                    int uns = (I->aux & 0x100) != 0;
                    /* A 64-bit value sits on the IR stack with its HIGH dword on
                     * top and `b` above `a`, so the pops are b_hi, b_lo, a_hi,
                     * a_lo.  Every result is pushed back low-first to keep that
                     * invariant. */
                    if (bop) {
                        int is_add = ark_streq(bop, "add");
                        int is_sub = ark_streq(bop, "sub");
                        asmbuf_puts(&buf, xh_x86_rule("text.c1281983882433d5"));
                        asmbuf_puts(&buf, xh_x86_rule("text.1a0f564ddc603945"));
                        asmbuf_puts(&buf, bop);
                        asmbuf_puts(&buf, xh_x86_rule("text.2ce889bddde8feb8"));
                        if (is_add) asmbuf_puts(&buf, xh_x86_rule("text.2825f4be58f40442"));
                        else if (is_sub) asmbuf_puts(&buf, xh_x86_rule("text.0e56e366153b6e70"));
                        else {
                            asmbuf_puts(&buf, bop);
                            asmbuf_puts(&buf, xh_x86_rule("text.44f2696d1d3f8e69"));
                        }
                        asmbuf_puts(&buf, xh_x86_rule("text.0dd72d509b10fd37"));
                    } else if (op == (int)TOK_SHL || op == (int)TOK_SHR) {
                        /* 64-bit shift.  shld/shrd only move bits across the
                         * two dwords for a count below 32; the hardware masks
                         * `cl` to 5 bits, so the n >= 32 half has to be a
                         * separate path or 1ULL << 32 silently answers 1.
                         *   n <  32:  hi = shld(hi,lo,n); lo = lo << n
                         *   n >= 32:  hi = lo << (n-32); lo = 0
                         * (and the mirror for right shifts; for n in 32..63,
                         * n-32 == n & 31). */
                        int is_shr = (op == (int)TOK_SHR);
                        int lhi = ++lbl64, lend = ++lbl64;
                        asmbuf_puts(&buf, xh_x86_rule("text.6406a1641d7fff65"));
                        asmbuf_puts(&buf, xh_x86_rule("text.679314bd2ff12b22"));
                        asmbuf_puts(&buf, xh_x86_rule("text.461f709172b494e2"));
                        x86_elbl(&buf, f->name, "sh", lhi, 0);
                        if (is_shr) {
                            asmbuf_puts(&buf, xh_x86_rule("text.820dbde7748ee897"));
                            asmbuf_puts(&buf, uns ? xh_x86_rule("text.054bf5894365747f") : xh_x86_rule("text.5c8ed08f641c8a16"));
                        } else {
                            asmbuf_puts(&buf, xh_x86_rule("text.821b71e39855d198"));
                        }
                        asmbuf_puts(&buf, xh_x86_rule("text.69256272f003017d"));
                        x86_elbl(&buf, f->name, "sh", lend, 0);
                        x86_elbl(&buf, f->name, "sh", lhi, 1);
                        if (is_shr) {
                            asmbuf_puts(&buf, xh_x86_rule("text.dd90cd92b33d5573"));
                            asmbuf_puts(&buf, uns ? xh_x86_rule("text.6082778eecf4e8dd")
                                                  : xh_x86_rule("text.2dfc378c712e8c2e"));
                        } else {
                            asmbuf_puts(&buf, xh_x86_rule("text.9f5ead3407b2d8f5"));
                        }
                        x86_elbl(&buf, f->name, "sh", lend, 1);
                        asmbuf_puts(&buf, xh_x86_rule("text.0dd72d509b10fd37"));
                    } else if (op == (int)TOK_MUL) {
                        /* 64x64 -> low 64 bits:
                         *   lo = lo(a_lo*b_lo)
                         *   hi = hi32(a_lo*b_lo) + a_hi*b_lo + a_lo*b_hi */
                        asmbuf_puts(&buf, xh_x86_rule("text.c1281983882433d5"));
                        asmbuf_puts(&buf, xh_x86_rule("text.c654558cc5c0ec8b"));
                        asmbuf_puts(&buf, xh_x86_rule("text.ff834093feaebb13"));
                        asmbuf_puts(&buf, xh_x86_rule("text.a60c6b789a6dfebd"));
                        asmbuf_puts(&buf, xh_x86_rule("text.6f6d010f0e5ab3c1"));
                        asmbuf_puts(&buf, xh_x86_rule("text.a7a351cdc205c8ef"));
                    } else if (op == (int)TOK_DIV || op == (int)TOK_MOD) {
                        /* Restoring shift-subtract, 64 rounds.  Registers:
                         *   a=|a| in ebx:eax   d=|b| in [divtmp+4:divtmp+0]
                         *   r in edx:ecx       q in esi:edi
                         * sign bits in [divtmp+12]: bit0 = a<0, bit1 = b<0.
                         * Six registers plus the divisor is all this needs, and
                         * no shld is required -- the 128-bit (r:n) shift is an
                         * add/adc chain.  A zero divisor is undefined in C; it
                         * answers 0 here instead of looping or faulting. */
                        int linit = ++lbl64, lloop = ++lbl64, lsub = ++lbl64;
                        int lnosub = ++lbl64, lqpos = ++lbl64, lrpos = ++lbl64;
                        int lend = ++lbl64;
                        asmbuf_puts(&buf, xh_x86_rule("text.c1281983882433d5"));
                        asmbuf_puts(&buf, xh_x86_rule("text.fa32d915a294d9b9"));
                        asmbuf_int(&buf, divtmp + 12);
                        asmbuf_puts(&buf, xh_x86_rule("text.84ec6f047c3668dc"));
                        if (!uns) {
                            asmbuf_puts(&buf, xh_x86_rule("text.95fe33ef378a8bb4"));
                            asmbuf_puts(&buf, xh_x86_rule("text.fa32d915a294d9b9"));
                            asmbuf_int(&buf, divtmp + 12);
                            asmbuf_puts(&buf, xh_x86_rule("text.47d93a4d497c04b5"));
                            /* esi holds the 0/1 sign BIT here, not the
                             * full high word: `sar esi, 31` on 1 gives 0, so
                             * |a| was never taken and every signed 64-bit
                             * divide with a negative dividend divided the
                             * raw two's-complement pattern.  `neg` turns the
                             * bit into the 0 / -1 mask the xor/sub pair
                             * needs. */
                            asmbuf_puts(&buf, xh_x86_rule("text.3808d8f36f7d5daa"));
                            asmbuf_puts(&buf, xh_x86_rule("text.4b9c82419f56eeb1"));
                            asmbuf_puts(&buf, xh_x86_rule("text.d5a384c1f1fdbf16"));
                            asmbuf_puts(&buf, xh_x86_rule("text.b2214e0a3ab8f00c"));
                            asmbuf_puts(&buf, xh_x86_rule("text.d1ddcffec890c328"));
                            asmbuf_int(&buf, divtmp + 12);
                            asmbuf_puts(&buf, xh_x86_rule("text.9360e8e3ce846e92"));
                            asmbuf_puts(&buf, xh_x86_rule("text.fa32d915a294d9b9"));
                            asmbuf_int(&buf, divtmp + 12);
                            asmbuf_puts(&buf, xh_x86_rule("text.2257aee5f583afe4"));
                            asmbuf_puts(&buf, xh_x86_rule("text.466239f8ebbb4beb"));
                            asmbuf_puts(&buf, xh_x86_rule("text.da0ba7457b56865b"));
                            asmbuf_puts(&buf, xh_x86_rule("text.e5389721d42955df"));
                            asmbuf_puts(&buf, xh_x86_rule("text.c1c3fdf0eab006ff"));
                            asmbuf_puts(&buf, xh_x86_rule("text.fc1d629ae8b91275"));
                            asmbuf_int(&buf, divtmp);
                            asmbuf_puts(&buf, xh_x86_rule("text.5b6b86a4905e6f09"));
                            asmbuf_int(&buf, divtmp + 4);
                            asmbuf_puts(&buf, xh_x86_rule("text.69b61f412b288a0a"));
                        } else {
                            asmbuf_puts(&buf, xh_x86_rule("text.fc1d629ae8b91275"));
                            asmbuf_int(&buf, divtmp);
                            asmbuf_puts(&buf, xh_x86_rule("text.8b3deb472083fd10"));
                            asmbuf_int(&buf, divtmp + 4);
                            asmbuf_puts(&buf, xh_x86_rule("text.69b61f412b288a0a"));
                        }
                        asmbuf_puts(&buf, xh_x86_rule("text.cb41f02a263e8616"));
                        asmbuf_int(&buf, divtmp);
                        asmbuf_puts(&buf, xh_x86_rule("text.2fa01da70f59e71a"));
                        asmbuf_int(&buf, divtmp + 4);
                        asmbuf_puts(&buf, xh_x86_rule("text.9dd9f2a58c42e6d8"));
                        x86_elbl(&buf, f->name, "dv", linit, 0);
                        asmbuf_puts(&buf, xh_x86_rule("text.5bd3862d8a7849ba"));
                        x86_elbl(&buf, f->name, "dv", lend, 0);
                        x86_elbl(&buf, f->name, "dv", linit, 1);
                        asmbuf_puts(&buf, xh_x86_rule("text.b627985a9623b001"));
                        asmbuf_puts(&buf, xh_x86_rule("text.8a70b7986310d9ec"));
                        asmbuf_puts(&buf, xh_x86_rule("text.fa32d915a294d9b9"));
                        asmbuf_int(&buf, divtmp + 8);
                        asmbuf_puts(&buf, xh_x86_rule("text.b3f3de66f4c956d4"));
                        x86_elbl(&buf, f->name, "dv", lloop, 1);
                        /* Shift the 128-bit (r:n) pair left by one the
                         * way the restoring algorithm needs it: the bit
                         * shifted out of n's top goes into r's bottom,
                         * which is what adc (not add) does here.  With
                         * `add` the dividend's high bits were dropped and
                         * every 64-bit division answered 0. */
                        asmbuf_puts(&buf, xh_x86_rule("text.3c2ce573bc6f8bda"));
                        asmbuf_puts(&buf, xh_x86_rule("text.1ac27aeafa036104"));
                        asmbuf_puts(&buf, xh_x86_rule("text.617fb65c4da207c8"));
                        asmbuf_puts(&buf, xh_x86_rule("text.cd95285901a5a647"));
                        asmbuf_int(&buf, divtmp + 4);
                        asmbuf_puts(&buf, xh_x86_rule("text.0a9294436a104c5a"));
                        x86_elbl(&buf, f->name, "dv", lnosub, 0);
                        asmbuf_puts(&buf, xh_x86_rule("text.a66f7e10984df850"));
                        x86_elbl(&buf, f->name, "dv", lsub, 0);
                        asmbuf_puts(&buf, xh_x86_rule("text.596cabe39f20ca06"));
                        asmbuf_int(&buf, divtmp);
                        asmbuf_puts(&buf, xh_x86_rule("text.0a9294436a104c5a"));
                        x86_elbl(&buf, f->name, "dv", lnosub, 0);
                        x86_elbl(&buf, f->name, "dv", lsub, 1);
                        asmbuf_puts(&buf, xh_x86_rule("text.667532ad6b467194"));
                        asmbuf_int(&buf, divtmp);
                        asmbuf_puts(&buf, xh_x86_rule("text.ca9cbd7b617c471b"));
                        asmbuf_int(&buf, divtmp + 4);
                        asmbuf_puts(&buf, xh_x86_rule("text.115bb8c5c2542394"));
                        x86_elbl(&buf, f->name, "dv", lnosub, 1);
                        asmbuf_puts(&buf, xh_x86_rule("text.26fc695c40f28fce"));
                        asmbuf_int(&buf, divtmp + 8);
                        asmbuf_puts(&buf, xh_x86_rule("text.ec8924392ea7f046"));
                        x86_elbl(&buf, f->name, "dv", lloop, 0);
                        if (!uns) {
                            /* Quotient is negative iff exactly one operand was
                             * (sign word 1 or 2); the remainder follows
                             * dividend's sign (bit 0). */
                            asmbuf_puts(&buf, xh_x86_rule("text.6eaf51aaec0d1d8a"));
                            asmbuf_int(&buf, divtmp + 12);
                            asmbuf_puts(&buf, xh_x86_rule("text.3b12671c027655a7"));
                            asmbuf_puts(&buf, xh_x86_rule("text.30dc593803195a56"));
                            x86_elbl(&buf, f->name, "dv", lqpos, 0);
                            asmbuf_puts(&buf, xh_x86_rule("text.800b436f470e1d12"));
                            x86_elbl(&buf, f->name, "dv", lqpos, 0);
                            asmbuf_puts(&buf, xh_x86_rule("text.fd89fd8a166f247a"));
                            asmbuf_puts(&buf, xh_x86_rule("text.0d62446613963db2"));
                            x86_elbl(&buf, f->name, "dv", lqpos, 1);
                            asmbuf_puts(&buf, xh_x86_rule("text.360586a4672c9c1f"));
                            x86_elbl(&buf, f->name, "dv", lrpos, 0);
                            asmbuf_puts(&buf, xh_x86_rule("text.f71fe4d2f1a5f9c1"));
                            asmbuf_puts(&buf, xh_x86_rule("text.1a04608c358721c5"));
                            x86_elbl(&buf, f->name, "dv", lrpos, 1);
                        }
                        if (op == (int)TOK_DIV)
                            asmbuf_puts(&buf, xh_x86_rule("text.ea9aa9b6855ae4fa"));
                        else
                            asmbuf_puts(&buf, xh_x86_rule("text.8ad98fe039692ddc"));
                        x86_elbl(&buf, f->name, "dv", lend, 1);
                    } else {
                        int uns2 = uns;
                        const char *cc = 0;
                        int ld = ++lbl64;
                        switch (op) {
                            case (int)TOK_EQ: cc = "sete";  break;
                            case (int)TOK_NE: cc = "setne"; break;
                            case (int)TOK_LT: cc = uns2 ? "setb" : "setl";  break;
                            case (int)TOK_GT: cc = uns2 ? "seta" : "setg";  break;
                            case (int)TOK_LE: cc = uns2 ? "setbe" : "setle"; break;
                            case (int)TOK_GE: cc = uns2 ? "setae" : "setge"; break;
                            default: cc = "sete"; break;
                        }
                        /* Two-word compare: the high dwords decide unless they
                         * are equal, and then the low dwords decide with the
                         * same condition code.  Comparing only the low words
                         * (what this used to do) answered 0 for every ordering
                         * question on 64-bit values. */
                        asmbuf_puts(&buf, xh_x86_rule("text.c1281983882433d5"));
                        asmbuf_puts(&buf, xh_x86_rule("text.0cf03c3bc7f8e844"));
                        x86_elbl(&buf, f->name, "cm", ld, 0);
                        asmbuf_puts(&buf, xh_x86_rule("text.9e89a698ecb49a06"));
                        x86_elbl(&buf, f->name, "cm", ld, 1);
                        asmbuf_puts(&buf, xh_x86_rule("text.1a0f564ddc603945"));
                        asmbuf_puts(&buf, cc);
                        asmbuf_puts(&buf, xh_x86_rule("text.8d145c1a3edd357d"));
                    }
                    break;
                }

                /* IR_64CMP: 64-bit compare. aux = TOK_xxx | (0x100 if unsigned). */
                case IR_64CMP: {
                    int op = I->aux & 0xFF;
                    int uns = (I->aux & 0x100) != 0;
                    const char *cc;
                    int ld = ++lbl64;
                    switch (op) {
                        case (int)TOK_EQ: cc = "sete";  break;
                        case (int)TOK_NE: cc = "setne"; break;
                        case (int)TOK_LT: cc = uns ? "setb" : "setl";  break;
                        case (int)TOK_GT: cc = uns ? "seta" : "setg";  break;
                        case (int)TOK_LE: cc = uns ? "setbe" : "setle"; break;
                        case (int)TOK_GE: cc = uns ? "setae" : "setge"; break;
                        default: cc = "sete"; break;
                    }
                    /* High dwords first, then the low ones when they tie; the
                     * condition code is the same for both halves. */
                    asmbuf_puts(&buf, xh_x86_rule("text.c1281983882433d5"));
                    asmbuf_puts(&buf, xh_x86_rule("text.0cf03c3bc7f8e844"));
                    x86_elbl(&buf, f->name, "cm", ld, 0);
                    asmbuf_puts(&buf, xh_x86_rule("text.9e89a698ecb49a06"));
                    x86_elbl(&buf, f->name, "cm", ld, 1);
                    asmbuf_puts(&buf, xh_x86_rule("text.1a0f564ddc603945"));
                    asmbuf_puts(&buf, cc);
                    asmbuf_puts(&buf, xh_x86_rule("text.8d145c1a3edd357d"));
                    break;
                }

                /* IR_64F2I / IR_64I2F: 64-bit float <-> 64-bit int.
                 * On x86-32 these operate on 8-byte stack slots. */
                case IR_64F2I: {
                    /* double (or float, aux bit 0x200 clear) -> long long.
                     * aux 0x200 = source is double, 0x400 = destination is
                     * unsigned.  The source sits high-dword-first on the IR
                     * stack, so it has to be flipped before the movq.
                     * Every cvttsd2si below is handed a NON-NEGATIVE operand,
                     * so truncation toward zero is floor and the 2^32 split
                     * carries exactly.  Folding the 2^31 bias in BEFORE the
                     * truncation (as this used to) makes truncation round the
                     * high half up whenever the quotient has a fraction:
                     * `double d = 1e9; unsigned long long u = (unsigned long
                     * long)d;` answered 0x100000000, so u%251 was 123 where
                     * the x86-64 and ARM backends both say 187.  The sign is
                     * now handled by converting the magnitude and negating
                     * afterwards, the same shape as IR_64I2F. */
                    int lp = ++lbl64, ld = ++lbl64, lu = ++lbl64, lv = ++lbl64;
                    int lw = ++lbl64;
                    if (I->aux & 0x200) {
                        x86_pair_swap_at(&buf, 0);
                        asmbuf_puts(&buf, xh_x86_rule("text.c0d9bc14821121d4"));
                    } else {
                        asmbuf_puts(&buf, xh_x86_rule("text.4afef8bb740fd333"));
                        asmbuf_puts(&buf, xh_x86_rule("text.5d9f1cb0416a63f8"));
                    }
                    /* xmm2 = 2^32, xmm3 = 2^31 */
                    asmbuf_puts(&buf, xh_x86_rule("text.6e94a4c258ef9ff0"));
                    asmbuf_puts(&buf, xh_x86_rule("text.a85fde474c053be0"));
                    asmbuf_puts(&buf, xh_x86_rule("text.59ac07dd691e82c6"));
                    if (I->aux & 0x400) {
                        /* Unsigned destination: values at or above 2^63 do not
                         * fit a signed conversion, so they lose 2^63 up front
                         * and get that bit put back at the end. */
                        asmbuf_puts(&buf, xh_x86_rule("text.25c3185bc4002855"));
                        asmbuf_puts(&buf, xh_x86_rule("text.a3318c44ef5a1b4e"));
                        x86_elbl(&buf, f->name, "f2i", lu, 0);
                        asmbuf_puts(&buf, xh_x86_rule("text.4a901af6a9ce5827"));
                        x86_elbl(&buf, f->name, "f2i", lu, 1);
                    } else {
                        /* Signed destination: take the magnitude first, then
                         * re-apply the sign. |x| < 2^63 always fits. */
                        asmbuf_puts(&buf, xh_x86_rule("text.f303df9e4dea9593"));
                        x86_elbl(&buf, f->name, "f2i", lp, 0);
                        asmbuf_puts(&buf, xh_x86_rule("text.b323dcde75821ec9"));
                        x86_elbl(&buf, f->name, "f2i", lp, 1);
                    }
                    /* Shared: xmm0 = |x| in [0, 2^63).  High half = floor(x /
                     * 2^32); the quotient is non-negative so cvttsd2si is a
                     * floor.  The remainder is in [0, 2^32), so it needs the
                     * same 2^31 split, again on a non-negative operand. */
                    asmbuf_puts(&buf, xh_x86_rule("text.18b4bd5cca1366a6"));
                    asmbuf_puts(&buf, xh_x86_rule("text.bf57062e79a57deb"));
                    asmbuf_puts(&buf, xh_x86_rule("text.23c4ded4321ef5c6"));
                    asmbuf_puts(&buf, xh_x86_rule("text.1571b586265e2bfa"));
                    asmbuf_puts(&buf, xh_x86_rule("text.4d294fbe16d34d51"));
                    x86_elbl(&buf, f->name, "f2i", lv, 0);
                    asmbuf_puts(&buf, xh_x86_rule("text.3d2e1813919f5f02"));
                    x86_elbl(&buf, f->name, "f2i", lw, 0);
                    x86_elbl(&buf, f->name, "f2i", lv, 1);
                    asmbuf_puts(&buf, xh_x86_rule("text.8cc0cd8c22cadc72"));
                    x86_elbl(&buf, f->name, "f2i", lw, 1);
                    if (I->aux & 0x400) {
                        asmbuf_puts(&buf, xh_x86_rule("text.bf53b61ffa4748cd"));
                        x86_elbl(&buf, f->name, "f2i", ld, 0);
                        asmbuf_puts(&buf, xh_x86_rule("text.18456b0dc84465a1"));
                        x86_elbl(&buf, f->name, "f2i", ld, 1);
                    } else {
                        asmbuf_puts(&buf, xh_x86_rule("text.bf53b61ffa4748cd"));
                        x86_elbl(&buf, f->name, "f2i", ld, 0);
                        asmbuf_puts(&buf, xh_x86_rule("text.e927fc829b5b2ad2"));
                        x86_elbl(&buf, f->name, "f2i", ld, 1);
                    }
                    asmbuf_puts(&buf, xh_x86_rule("text.a7a351cdc205c8ef"));
                    break;
                }
                case IR_64I2F: {
                    /* long long -> double/float.  aux 0x200 = destination is
                     * double, 0x400 = source is unsigned.  |x| is assembled as
                     * hi*2^32 + lo with each half converted as unsigned
                     * (cvtsi2sd is signed only), then the sign is applied as a
                     * multiply. */
                    asmbuf_puts(&buf, xh_x86_rule("text.6f9c8b71fa5adc5a"));
                    if (!(I->aux & 0x400)) {
                        asmbuf_puts(&buf, xh_x86_rule("text.466239f8ebbb4beb"));
                        asmbuf_puts(&buf, xh_x86_rule("text.24620940121cebad"));
                        asmbuf_puts(&buf, xh_x86_rule("text.c1678ecc06d8c119"));
                    } else {
                        asmbuf_puts(&buf, xh_x86_rule("text.ae4c7e27bb40fc19"));
                    }
                    asmbuf_puts(&buf, xh_x86_rule("text.0e2163b0183a2bf5"));
                    x86_u32_to_xmm(&buf, "xmm1");
                    asmbuf_puts(&buf, xh_x86_rule("text.42a37330107db660"));
                    x86_u32_to_xmm(&buf, "xmm0");
                    asmbuf_puts(&buf, xh_x86_rule("text.6e94a4c258ef9ff0"));
                    asmbuf_puts(&buf, xh_x86_rule("text.c17e0c0d27c3d7ee"));
                    asmbuf_puts(&buf, xh_x86_rule("text.f3204e6546e0591b"));
                    asmbuf_puts(&buf, xh_x86_rule("text.12fd0b0927f0e8a7"));
                    if (I->aux & 0x200) {
                        asmbuf_puts(&buf, xh_x86_rule("text.7674ef19adb6c5e8"));
                        x86_pair_swap_at(&buf, 0);
                    } else {
                        asmbuf_puts(&buf, xh_x86_rule("text.57b0860be9389b8b"));
                        asmbuf_puts(&buf, xh_x86_rule("text.bfbf7eb10d65f0c1"));
                    }
                    break;
                }

                /* IR_STORE_FLD: bitfield store. val=bit_off, aux=bit_width. */
                case IR_STORE_FLD: {
                    int bit_off = I->val;
                    int bw = I->aux;
                    unsigned mask = (bw >= 32) ? 0xFFFFFFFFu : ((1u << bw) - 1u);
                    unsigned field = (unsigned)(mask << bit_off);
                    /* Consume [addr, value] and leave nothing, the contract the
                     * ARM and C backends already keep: the value used to be
                     * pushed back here, which left a stray word for every
                     * enclosing expression.  The merge must also preserve the
                     * neighbouring fields; the old code kept only the bits
                     * below the field and wiped everything above it (measured:
                     * `s.b++` zeroed the upper field `s.c`). */
                    asmbuf_puts(&buf, xh_x86_rule("text.eea0363f8ddf278d"));  /* value, addr */
                    asmbuf_puts(&buf, xh_x86_rule("text.7ef30c4fffaf7a16"));
                    asmbuf_int(&buf, (int)mask);
                    if (bit_off > 0) {
                        asmbuf_puts(&buf, xh_x86_rule("text.8b3cc64fba698ba0"));
                        asmbuf_int(&buf, bit_off);
                    }
                    asmbuf_puts(&buf, xh_x86_rule("text.7fdab8b49978ea66"));
                    asmbuf_int(&buf, (int)(unsigned)(~field));
                    asmbuf_puts(&buf, xh_x86_rule("text.5d13c6795d5acd81"));
                    break;
                }

                /* IR_LOAD_FLD: bitfield load. val=bit_off, aux=bit_width. */
                case IR_LOAD_FLD: {
                    int bit_off = I->val;
                    /* aux low byte = bit width, bit 8 = signed (the same
                     * encoding the ARM and C backends read).  Using aux
                     * directly made every signed field's 0x100 look like a
                     * width over 32, so `int c:6` holding -6 came back as the
                     * whole storage word. */
                    int bw = I->aux & 0xFF;
                    int is_signed = (I->aux & 0x100) != 0;
                    unsigned mask = (bw >= 32) ? 0xFFFFFFFFu : ((1u << bw) - 1u);
                    asmbuf_puts(&buf, xh_x86_rule("text.6f8fe5caa080b1fd"));
                    if (bit_off > 0) {
                        asmbuf_puts(&buf, xh_x86_rule("text.b28953ccda3e8afd"));
                        asmbuf_int(&buf, bit_off);
                        asmbuf_puts(&buf, xh_x86_rule("text.01ba4719c80b6fe9"));
                    }
                    asmbuf_puts(&buf, xh_x86_rule("text.7ef30c4fffaf7a16"));
                    asmbuf_int(&buf, (int)mask);
                    if (is_signed && bw > 0 && bw < 32) {
                        asmbuf_puts(&buf, xh_x86_rule("text.8b3cc64fba698ba0"));
                        asmbuf_int(&buf, 32 - bw);
                        asmbuf_puts(&buf, xh_x86_rule("text.ceda75f1be4f9e1a"));
                        asmbuf_int(&buf, 32 - bw);
                    }
                    asmbuf_puts(&buf, xh_x86_rule("text.686bd128725fa74d"));
                    break;
                }

                /* IR_ASM: inline assembly. The template is written out verbatim.
                 * Extended form ("@template : outs : ins : clobbers") carries the
                 * placeholders but not the operand -> register assignment, so the
                 * x86 backend cannot substitute %N the way the ARM backend does.
                 * Emitting the template anyway produced assembly that no assembler
                 * accepts, so report it as an error instead of shipping garbage. */
                case IR_ASM:
                    if(I->aux&0x40000000){xh_x86_inline_asm(&buf,I,type_ptr_size()==8);break;}
                    if (I->name) {
                        const char *s = I->name;
                        if (s[0] == '@') {
                            s++;
                            while (*s == ' ' || *s == '\t') s++;
                            if (*s == '"') {
                                const char *q = s + 1;
                                while (*q && *q != '"') {
                                    if (*q == '%' && q[1] >= '0' && q[1] <= '9') {
                                        ark_log("[x86] error: inline asm operand %%%c cannot be "
                                                "substituted by the x86 backend\n", q[1]);
                                        x86_emit_errors++;
                                        break;
                                    }
                                    q++;
                                }
                                s++;
                                while (*s && *s != '"') {
                                    asmbuf_putc(&buf, *s);
                                    s++;
                                }
                            }
                        } else {
                            asmbuf_puts(&buf, s);
                        }
                        asmbuf_putc(&buf, '\n');
                    }
                    break;

                /* IR_SVC / IR_SMC / IR_SIMD_*: ARM-specific, emit as comment */
                case IR_SVC:
                case IR_SMC:
                case IR_SIMD_ADD16:
                case IR_SIMD_SUB16:
                case IR_SIMD_ADD8:
                case IR_SIMD_SUB8:
                    asmbuf_puts(&buf, xh_x86_rule("text.ab3f82bd50fcee78"));
                    asmbuf_puts(&buf, ir_op_name(I->op));
                    asmbuf_puts(&buf, xh_x86_rule("text.b1e5c265f2e5c87b"));
                    break;

                default:
                    asmbuf_puts(&buf, xh_x86_rule("text.35815f9c362b56b9"));
                    asmbuf_puts(&buf, ir_op_name(I->op));
                    asmbuf_puts(&buf, xh_x86_rule("text.01ba4719c80b6fe9"));
                    break;
            }
        }
        /* Function-end epilogue (fallthrough return). For sret functions,
         * return the saved sret pointer in eax instead of 0. */
        if (sret_off) {
            asmbuf_puts(&buf, xh_x86_rule("text.6eaf51aaec0d1d8a"));
            asmbuf_int(&buf, sret_off);
            asmbuf_puts(&buf, xh_x86_rule("text.bc8bdd6524c78086"));
        } else {
            asmbuf_puts(&buf, xh_x86_rule("text.72d99589caf320eb"));
        }
    }

    {
        int n = buf.len;
        int i;
        int errs = x86_emit_errors + xh_x86_rules_failed();
        x86_emit_errors = 0;
        if (errs) {
            ark_log("[x86] error: %d unsupported construct(s), no output written\n", errs);
            if (cap > 0) out[0] = 0;
            return -1;
        }
        if (n + 1 > cap) {
            /* A truncated program is an answer nobody asked for:
             * the text would be cut mid-instruction and the
             * assembler would report a nonsense mnemonic (measured:
             * "unknown mnemonic P") instead of "buffer too small".
             * Refuse, so the driver can name ARK_ASM_BUF_SIZE. */
            if (cap > 0) out[0] = 0;
            return -1;
        }
        for (i = 0; i < n; i++) out[i] = buf.buf[i];
        out[n] = 0;
        return n;
    }
}

/* === x86-64 / System V emitter ===========================================
 *
 * The IR word is 32 bits wide, so every 8-byte value travels as TWO words
 * with the HIGH word on top.  A pointer is the one exception: the type model
 * says 8 bytes, but an address is a single object, so irgen marks it
 * IR_AUX_SLOT64 and it occupies ONE machine word.
 *
 * Every machine word is an 8-byte stack slot, and the high 32 bits of a slot
 * that carries a 32-bit payload MUST be zero.  That falls out of the 2-word
 * model: all 32-bit results are produced by a write to the 32-bit register
 * form (which zeroes the upper half of the 64-bit register), and sign
 * extension is never implicit -- irgen spells it out as
 * PUSH 0 / compare / negate, which is why a `long long` built from an `int`
 * is exact here even though nothing in the IR says whether the int was
 * signed.
 *
 * Frame (rbp-relative, and the stack grows towards lower addresses):
 *     [rbp-8]                hidden sret pointer (return buffer)
 *     [rbp-16]               rsp saved across a call
 *     [rbp-24] .. [rbp-48]   scratch
 *     [rbp-FRAMELO + off]    locals,  FRAMELO = 48 + round16(local_size)
 *     [rbp-FRAMELO-8*(k+1)]  64 argument spill slots
 * FRAME is a multiple of 16, so rsp is 16-byte aligned at every `call`.
 *
 * Register use: rax rcx rdx rsi rdi r8-r11 and xmm0-xmm7 -- no callee-saved
 * register is touched, so the prologue has nothing to preserve.  Floating
 * point travels as its bit pattern through the integer stack; a single
 * precision operation is widened to double, computed, and narrowed, because
 * this assembler has no addss/subss/mulss/divss and no movd.
 */

static int g_amd64_errors;

static void amd_unsupported(AsmBuffer *b, const char *what) {
    ark_log("[x86-64] error: %s is not supported by the x86_64 backend\n", what);
    g_amd64_errors++;
    asmbuf_puts(b, xh_x86_rule("text.a4b40e685f7daa97"));
    asmbuf_puts(b, what);
    asmbuf_putc(b, '\n');
}

/* "[rbp+delta]" / "[rbp-delta]" / "[rbp]" */
static void amd_addr(AsmBuffer *b, int delta) {
    asmbuf_puts(b, xh_x86_rule("text.3b0cfa93737d146f"));
    if (delta > 0) {
        asmbuf_putc(b, '+');
        asmbuf_int(b, delta);
    } else if (delta < 0) {
        asmbuf_putc(b, '-');
        asmbuf_int(b, -delta);
    }
    asmbuf_putc(b, ']');
}

/* "    <mn> <reg>, [<size> ptr] [rbp+delta]" */
static void amd_mem_rd(AsmBuffer *b, const char *mn, const char *reg,
                       const char *sz, int delta) {
    asmbuf_puts(b, xh_x86_rule("text.1a0f564ddc603945"));
    asmbuf_puts(b, mn);
    asmbuf_putc(b, ' ');
    asmbuf_puts(b, reg);
    asmbuf_puts(b, xh_x86_rule("text.0a07f659461970d8"));
    if (sz && sz[0]) {
        asmbuf_puts(b, sz);
        asmbuf_puts(b, xh_x86_rule("text.93a01d56bd803c43"));
    }
    amd_addr(b, delta);
    asmbuf_putc(b, '\n');
}

/* "    <mn> [<size> ptr] [rbp+delta], <reg>" */
static void amd_rd_mem(AsmBuffer *b, const char *mn, const char *sz,
                       int delta, const char *reg) {
    asmbuf_puts(b, xh_x86_rule("text.1a0f564ddc603945"));
    asmbuf_puts(b, mn);
    asmbuf_putc(b, ' ');
    if (sz && sz[0]) {
        asmbuf_puts(b, sz);
        asmbuf_puts(b, xh_x86_rule("text.93a01d56bd803c43"));
    }
    amd_addr(b, delta);
    asmbuf_puts(b, xh_x86_rule("text.0a07f659461970d8"));
    asmbuf_puts(b, reg);
    asmbuf_putc(b, '\n');
}

/* Push a 32-bit immediate as one zero-extended 8-byte slot. */
static void amd_push_imm32(AsmBuffer *b, long long v) {
    if (v >= -2147483648LL && v <= 2147483647LL) {
        asmbuf_puts(b, xh_x86_rule("text.92b99704ac6bafbc"));
        asmbuf_int(b, v);
        asmbuf_puts(b, xh_x86_rule("text.475f15810fba1372"));
    } else {
        asmbuf_puts(b, xh_x86_rule("text.ee500dc20d14c1ab"));
        asmbuf_int(b, v);
        asmbuf_puts(b, xh_x86_rule("text.475f15810fba1372"));
    }
}

/* Push a 64-bit constant as TWO words: low first, high on top. */
static void amd_push_imm64(AsmBuffer *b, unsigned long long v) {
    asmbuf_puts(b, xh_x86_rule("text.92b99704ac6bafbc"));
    asmbuf_int(b, (long long)(int)(unsigned int)(v & 0xFFFFFFFFULL));
    asmbuf_puts(b, xh_x86_rule("text.a4cd08031c40ca5f"));
    asmbuf_int(b, (long long)(int)(unsigned int)((v >> 32) & 0xFFFFFFFFULL));
    asmbuf_puts(b, xh_x86_rule("text.475f15810fba1372"));
}

/* [.., lo, hi] -> r64 = (hi << 32) | lo.  r32 is r64's 32-bit name. */
/* Both helpers below need a scratch register for the other half of the value.
 * The scratch used to be hard-wired to rdx, which silently corrupted every
 * call that passed "rdx" as the value register: "mov rdx, rdx" left the value
 * in place but the following "shr rdx, 32" then destroyed it, and the pair
 * pushed onto the stack was (hi, hi) with the low word lost.  Pick a scratch
 * that is not the value register. */
static const char *amd_scratch_for(const char *r64) {
    char key[80];
    snprintf(key,sizeof(key),"abi.pair_scratch.%s",r64);
    return xh_x86_rule(key);
}
static void amd_pop64(AsmBuffer *b,const char *r64,const char *r32) {
    const char *args[3];args[0]=r64;args[1]=r32;args[2]=amd_scratch_for(r64);
    asmbuf_puts(b,xh_x86_expand("sequence.pair.pop64",args,3));
}
static void amd_push64(AsmBuffer *b,const char *r64,const char *r32) {
    const char *args[3];args[0]=r64;args[1]=r32;args[2]=amd_scratch_for(r64);
    asmbuf_puts(b,xh_x86_expand("sequence.pair.push64",args,3));
}

/* Load <size> ptr [rbp+delta] into a 64-bit register, zero-extending a narrow
 * load.  Used for a 1/2/4-byte local whose slot has to become one full word. */
static void amd_load_ext(AsmBuffer *b, int sz, int delta, int is_unsigned) {
    if (sz == 1) {
        asmbuf_puts(b, is_unsigned ? xh_x86_rule("text.d382154c42ac8a78") : xh_x86_rule("text.57f7fa00b5a14b35"));
        amd_addr(b, delta);
        asmbuf_putc(b, '\n');
    } else if (sz == 2) {
        asmbuf_puts(b, is_unsigned ? xh_x86_rule("text.adc6c614f58d2450") : xh_x86_rule("text.96b670cf0b0d1b8b"));
        amd_addr(b, delta);
        asmbuf_putc(b, '\n');
    } else {
        asmbuf_puts(b, xh_x86_rule("text.babe0163fc9bcbdb"));
        amd_addr(b, delta);
        asmbuf_putc(b, '\n');
    }
}

/* The SysV integer argument registers, in order. */
static const char *amd_argreg64(int i) { char key[40]; snprintf(key,sizeof(key),"abi.arg64.%d",i); return xh_x86_rule(key); }
static const char *amd_argreg32(int i) { char key[40]; snprintf(key,sizeof(key),"abi.arg32.%d",i); return xh_x86_rule(key); }

/* The type of a named global, or NULL. */
static Type *amd_global_type(IrProgram *prog, const char *name) {
    int i;
    if (!name) return 0;
    for (i = 0; i < prog->global_count; i++) {
        IrGlobal *g = prog->globals[i];
        if (g && g->name && ark_streq(g->name, name)) return g->type;
    }
    return 0;
}

/* Compare two doubles in xmm0 (a) and xmm1 (b) and push 0/1.  ucomisd sets
 * PF for unordered operands, so ==/!=/<... each need the parity flag folded
 * in; the i386 emitter ignored it and answered "1" for NaN != NaN. */
static void amd_fcmp(AsmBuffer *b, int op) {
    asmbuf_puts(b, xh_x86_rule("text.dffd78210d40c71d"));
    switch (op) {
        case (int)TOK_EQ:
            asmbuf_puts(b, xh_x86_rule("text.6f40fdb1899ad972"));
            break;
        case (int)TOK_NE:
            asmbuf_puts(b, xh_x86_rule("text.dc1ee4fca2c0b953"));
            break;
        case (int)TOK_LT:
            asmbuf_puts(b, xh_x86_rule("text.37bcbbde02db20af"));
            break;
        case (int)TOK_LE:
            asmbuf_puts(b, xh_x86_rule("text.6647cb3551b948a1"));
            break;
        case (int)TOK_GT:
            asmbuf_puts(b, xh_x86_rule("text.c63b3fd74236bee8"));
            break;
        case (int)TOK_GE:
            asmbuf_puts(b, xh_x86_rule("text.f006002c7d0d6e08"));
            break;
        default:
            asmbuf_puts(b, xh_x86_rule("text.6f40fdb1899ad972"));
            break;
    }
    asmbuf_puts(b, xh_x86_rule("text.3e4445ee301b6bc6"));
}

/* Which TOK_* condition a 2-operand compare should use. */
static const char *amd_cc(int op, int is_unsigned) {
    switch (op) {
        case (int)TOK_EQ: return xh_x86_rule("text.7e974de45fbf7089");
        case (int)TOK_NE: return xh_x86_rule("text.2ebfda629113f6bc");
        case (int)TOK_LT: return is_unsigned ? xh_x86_rule("text.45b0a30471aa0a40") : xh_x86_rule("text.c0f9a68a1e1d46ec");
        case (int)TOK_GT: return is_unsigned ? xh_x86_rule("text.8c8978cd7982d691") : xh_x86_rule("text.50c15039fd24925d");
        case (int)TOK_LE: return is_unsigned ? xh_x86_rule("text.52842a6b5054f646") : xh_x86_rule("text.9c24a172eeb3546c");
        case (int)TOK_GE: return is_unsigned ? xh_x86_rule("text.f29a01dcd9f9f0d0") : xh_x86_rule("text.b39541bead413722");
        default: return xh_x86_rule("text.7e974de45fbf7089");
    }
}

/* Emit floating <-> integer conversion result: the value is in xmm0 and the
 * destination is a float (one word) or a double (two words). */
static void amd_store_fp(AsmBuffer *b, int is_double) {
    if (is_double) {
        asmbuf_puts(b, xh_x86_rule("text.71125b5563b4d27f"));
        amd_push64(b, "rax", "eax");
    } else {
        asmbuf_puts(b, xh_x86_rule("text.b50044e3a1414928"));
    }
}

static int amd64_emit_internal(IrProgram *prog, char *out, int cap) {
    if (xh_x86_rules_begin(1)) return -1;
    xh_x86_prog = prog;
    AsmBuffer buf;
    int fi;
    asmbuf_init(&buf);

    asmbuf_puts(&buf, xh_x86_rule("text.7780d9617252be9e"));
    asmbuf_puts(&buf, xh_x86_rule("text.a8e02e4f5a197f3d"));
    x86_emit_data_sections(&buf, prog);
    asmbuf_puts(&buf, xh_x86_rule("text.a6b017eae27bd97f"));

    for (fi = 0; fi < prog->func_count; fi++) {
        IrFunc *f = prog->funcs[fi];
        int i;
        int lsz = x86_abi_align(f->local_size,"abi.frame.alignment");
        int framelo;
        int frame;
        int sret_used = 0;
        int param_idx = 0;
        int body_ok = 1;
        /* Register and stack use of the *named* parameters, kept for va_start:
         * the register save area stores every incoming argument register, and
         * the first variadic argument is the one just past these. */
        int int_used = 0, sse_used = 0, stk = xh_x86_number("abi.stack.argument_base");
        int rs_off = 0;
        /* One result buffer per *call site* that returns a struct/union wider
         * than 8 bytes (IR_CALL bit 36).  SysV hands such a result back through
         * a buffer the caller owns, passed as the hidden first integer
         * argument (rdi); see the pre-scan below. */
        int sret_off_tab[ARK_MAX_SRET_FUNCS];
        int sret_sz_tab[ARK_MAX_SRET_FUNCS];
        int sret_site_n = 0;
        int sret_site_cur = 0;
        int sret_pool = 0;
        int sret_area = 0;
        if (lsz < xh_x86_number("abi.frame.minimum_locals")) lsz = xh_x86_number("abi.frame.minimum_locals");
        framelo = xh_x86_number("abi.frame.locals_gap") + lsz;
        /* Pre-scan the IR for calls that return a struct/union wider than 8
         * bytes (IR_CALL bit 36) and reserve one buffer per call site, right
         * below the 64-word argument spill area.  The size comes from the IR
         * (bits 37..52), i.e. from the static type of the call expression, so
         * a callee that is only declared -- or one reached through a function
         * pointer -- gets exactly the same treatment as one defined in this
         * file; the name tables the arm backend used to consult could see
         * neither.  One buffer per *site*, never one shared scratch area: in
         * `struct S a = f(), b = g();` and in `h(g(), k())` two results are
         * live at the same time.  Overflow is a hard error, not an overlap.
         * Every buffer is 8-byte aligned because the callee may store a
         * double / long long member with a single 8-byte move. */
        for (i = 0; i < f->code_count; i++) {
            IrInst *si = &f->code[i];
            int sz;
            if (si->op != IR_CALL || !((si->val >> 36) & 1)) continue;
            sz = (int)((si->val >> 37) & 0xFFFF);
            sz = x86_abi_align(sz,"abi.aggregate.sret_alignment");
            if (sz <= 0 || sz > 0xFFFF) {
                char b1[12];
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "call site in '%s' returns a composite of %s bytes; "
                          "the x86_64 backend needs 1..65535",
                          f->name, diag_num(b1, sz), 0, 0, 0, 0);
                sz = 8;
            }
            if (sret_site_n >= ARK_MAX_SRET_FUNCS) {
                char b1[12], b2[12];
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "function '%s' has %s call sites returning a struct "
                          "wider than 8 bytes; at most %s are supported; raise "
                          "ARK_MAX_SRET_FUNCS and rebuild arkcc",
                          f->name, diag_num(b1, sret_site_n + 1),
                          diag_num(b2, ARK_MAX_SRET_FUNCS), 0, 0, 0);
                break;
            }
            sret_off_tab[sret_site_n] = sret_pool;
            sret_sz_tab[sret_site_n] = sz;
            sret_pool += sz;
            sret_site_n++;
        }
        /* The buffers sit below everything else the frame keeps (locals, the
         * argument spill area, and the variadic register save area), so
         * framelo and rs_off -- which existing code reads -- are unaffected. */
        sret_area = framelo + xh_x86_number("abi.frame.spill_slots") * xh_x86_number("abi.stack.word") + (f->is_variadic ? xh_x86_number("abi.va.save_bytes") : 0);
        frame = x86_abi_align(sret_area+sret_pool,"abi.frame.alignment");
        /* A variadic definition also owns the ABI's 176-byte register save
         * area: the six integer argument registers (48 bytes) followed by the
         * eight vector ones (16 bytes each).  It sits directly below the
         * argument spill area.  framelo, 64*8 and 176 are all multiples of 16,
         * so the base is 16-byte aligned -- which is what the vector half
         * needs, and it is why the area is allocated at all instead of being
         * read out of the registers on demand. */
        rs_off = framelo + xh_x86_number("abi.frame.spill_slots") * xh_x86_number("abi.stack.word") + xh_x86_number("abi.va.save_bytes");

        if (f->ret_type &&
            (f->ret_type->kind == TYPE_STRUCT || f->ret_type->kind == TYPE_UNION) &&
            type_size(f->ret_type) > xh_x86_number("abi.aggregate.direct_limit"))
            sret_used = 1;

        if (!f->is_static) {
            asmbuf_puts(&buf, xh_x86_rule("text.9bd395d67adffd1c"));
            asmbuf_puts(&buf, f->name);
            asmbuf_putc(&buf, '\n');
        }
        asmbuf_puts(&buf, f->name);
        asmbuf_puts(&buf, xh_x86_rule("text.f3743a0a18e53d13"));
        asmbuf_puts(&buf, xh_x86_rule("text.b886aa64e2df5b28"));
        asmbuf_int(&buf, frame);
        asmbuf_putc(&buf, '\n');
        if (sret_used) {
            asmbuf_puts(&buf, xh_x86_rule("text.33d123687cb27a9e"));
        }

        /* A variadic definition saves every incoming argument register before
         * the body can reuse one: the six INTEGER registers at the base of the
         * save area and the eight VECTOR registers 48 bytes in, one 16-byte
         * slot each (only the low 8 bytes of a double are live).  va_start then
         * points at this area and at the caller's stack block.  This is the
         * System V AMD64 convention spelled out in figure 3.34 of the psABI. */
        if (f->is_variadic) {
            int rj;
            for (rj = 0; rj < xh_x86_number("abi.integer.count"); rj++)
                amd_rd_mem(&buf, "mov", "qword", -rs_off + xh_x86_number("abi.stack.word") * rj,
                           amd_argreg64(rj));
            for (rj = 0; rj < xh_x86_number("abi.vector.count"); rj++) {
                asmbuf_puts(&buf, xh_x86_rule("text.9c2b4f48512425bd"));
                asmbuf_int(&buf, rs_off - xh_x86_number("abi.va.gp_bytes") - xh_x86_number("abi.va.fp_stride") * rj);
                asmbuf_puts(&buf, xh_x86_rule("text.c5a10134862f0f36"));
                asmbuf_int(&buf, rj);
                asmbuf_putc(&buf, '\n');
            }
        }

        /* Spill every incoming argument word into its frame slot first, so an
         * IR_GETARG that copies a large aggregate out of the argument registers
         * cannot clobber a register a later parameter still needs (the copy
         * uses rdi/rsi/rcx). */
        if (body_ok) {
            int k = 0, pi = 0, j;
            int_used = sret_used ? 1 : 0;
            sse_used = 0;
            stk = xh_x86_number("abi.stack.argument_base");
            for (j = 0; j < f->code_count; j++) {
                IrInst *I = &f->code[j];
                Type *pt = 0;
                int is_sse;
                int tmp;
                if (I->op != IR_GETARG || I->val >= 8000) continue;
                if (f->param_types && pi < f->param_count) pt = f->param_types[pi];
                pi++;
                is_sse = (pt && (pt->kind == TYPE_FLOAT || pt->kind == TYPE_DOUBLE));
                tmp = -framelo - xh_x86_number("abi.stack.word") * (k + 1);
                k++;
                if (is_sse) {
                    if (sse_used < xh_x86_number("abi.vector.count")) {
                        asmbuf_puts(&buf, xh_x86_rule("text.38456c9cf2daa5cd"));
                        asmbuf_int(&buf, sse_used);
                        asmbuf_puts(&buf, xh_x86_rule("text.01ba4719c80b6fe9"));
                        amd_rd_mem(&buf, "mov", "qword", tmp, "rax");
                        sse_used++;
                    } else {
                        amd_mem_rd(&buf, "mov", "rax", "qword", stk);
                        amd_rd_mem(&buf, "mov", "qword", tmp, "rax");
                        stk += xh_x86_number("abi.stack.word");
                    }
                } else {
                    if (int_used < xh_x86_number("abi.integer.count")) {
                        amd_rd_mem(&buf, "mov", "qword", tmp, amd_argreg64(int_used));
                        int_used++;
                    } else {
                        amd_mem_rd(&buf, "mov", "rax", "qword", stk);
                        amd_rd_mem(&buf, "mov", "qword", tmp, "rax");
                        stk += xh_x86_number("abi.stack.word");
                    }
                }
            }
        }

        for (i = 0; i < f->code_count && body_ok; i++) {
            IrInst *I = &f->code[i];
            switch (I->op) {
                case IR_FUNC_BEGIN:
                case IR_FUNC_END:
                case IR_NOP:
                case IR_ARG:
                case IR_LOCAL_ALLOC:
                /* The IR spells every widening out itself (PUSH 0 + compare +
                 * negate for signed, PUSH 0 for unsigned) and every narrowing
                 * as an explicit IR_TRUNC, so these three are no-ops -- the
                 * same contract the ARM and RISC-V backends work under.
                 *
                 * One exception: IR_SEXT with IR_AUX_SLOT64.  irgen emits it
                 * for `(long long)p`, where the single word on the stack is a
                 * complete 64-bit address rather than a 32-bit payload, so the
                 * missing high half cannot be a zero -- that would truncate
                 * every pointer above 4 GB.  Split the word instead: the low
                 * word becomes the zero-extended low 32 bits, the high word
                 * the upper 32, matching amd_pop64's (hi << 32) | lo. */
                case IR_SEXT:
                    if (I->aux & IR_AUX_SLOT64) {
                        asmbuf_puts(&buf, xh_x86_rule("text.fcc09ed83b78d922"));
                    }
                    break;
                case IR_ZEXT:
                    /* IR_AUX_SLOT64: the mirror of the case above -- pack the
                     * two-word 64-bit integer (lo, hi-on-top) back into the
                     * single pointer slot, keeping all 64 bits.  irgen emits it
                     * for `(char *)some_long_long`; dropping the high half
                     * instead would keep only the low 32 bits of the address
                     * (measured: `(char *)0x300000000LL != 0` answered 0). */
                    if (I->aux & IR_AUX_SLOT64) {
                        asmbuf_puts(&buf, xh_x86_rule("text.8adea96e84baae87"));
                    }
                    break;
                case IR_CDECL64:
                    break;

                case IR_GETARG: {
                    int sz;
                    int tmp;
                    int dst;
                    int k = param_idx++;
                    if (I->val >= 8000) {
                        amd_unsupported(&buf, "va_arg slot of a variadic function");
                        break;
                    }
                    sz = (int)((I->aux >> 8) & 0xFF);
                    if (sz <= 0) sz = 4;
                    tmp = -framelo - xh_x86_number("abi.stack.word") * (k + 1);
                    dst = (int)I->val - framelo;
                    if (sz > xh_x86_number("abi.aggregate.direct_limit")) {
                        /* The wire carried the aggregate's address. */
                        amd_mem_rd(&buf, "mov", "rsi", "qword", tmp);
                        asmbuf_puts(&buf, xh_x86_rule("text.e8358e3e66032f77"));
                        amd_addr(&buf, dst);
                        asmbuf_putc(&buf, '\n');
                        asmbuf_puts(&buf, xh_x86_rule("text.d392d939b942f07c"));
                        asmbuf_int(&buf, sz);
                        asmbuf_puts(&buf, xh_x86_rule("text.8d3fc2867d2efe58"));
                    } else if (sz > xh_x86_number("abi.ir.word")) {
                        amd_mem_rd(&buf, "mov", "rax", "qword", tmp);
                        amd_rd_mem(&buf, "mov", "qword", dst, "rax");
                    } else if (sz == 4) {
                        amd_mem_rd(&buf, "mov", "eax", "dword", tmp);
                        amd_rd_mem(&buf, "mov", "dword", dst, "eax");
                    } else if (sz == 2) {
                        amd_mem_rd(&buf, "mov", "eax", "dword", tmp);
                        amd_rd_mem(&buf, "mov", "word", dst, "ax");
                    } else {
                        amd_mem_rd(&buf, "mov", "eax", "dword", tmp);
                        amd_rd_mem(&buf, "mov", "byte", dst, "al");
                    }
                    break;
                }

                case IR_PUSH_INT:
                    amd_push_imm32(&buf, I->val);
                    break;

                case IR_PUSH_STR:
                    asmbuf_puts(&buf, xh_x86_rule("text.e9e669e41d52b9b2"));
                    asmbuf_int(&buf, I->val);
                    asmbuf_puts(&buf, xh_x86_rule("text.f4b1b72c766f9a05"));
                    break;

                case IR_PUSH_VAR: {
                    int w = (int)(I->aux & 0xFF);
                    int slot64 = (I->aux & IR_AUX_SLOT64) != 0;
                    int d = (int)I->val - framelo;
                    if (w == 16) {
                        amd_unsupported(&buf, "16-byte local variable");
                        break;
                    }
                    if ((I->aux & IR_AUX_COMPLEX) && w == 8) {
                        amd_unsupported(&buf, "_Complex float local variable");
                        break;
                    }
                    if (slot64) {
                        amd_mem_rd(&buf, "mov", "rax", "qword", d);
                        asmbuf_puts(&buf, xh_x86_rule("text.de1843ef75a9fb24"));
                    } else if (w == 8) {
                        amd_mem_rd(&buf, "mov", "rax", "qword", d);
                        amd_push64(&buf, "rax", "eax");
                    } else {
                        amd_load_ext(&buf, w, d, (I->aux & 0x100) != 0);
                        asmbuf_puts(&buf, xh_x86_rule("text.de1843ef75a9fb24"));
                    }
                    break;
                }

                case IR_PUSH_ADDR: {
                    int d = (int)I->val - framelo;
                    asmbuf_puts(&buf, xh_x86_rule("text.35bbb883dbc56169"));
                    amd_addr(&buf, d);
                    asmbuf_puts(&buf, xh_x86_rule("text.475f15810fba1372"));
                    break;
                }

                case IR_PUSH_GLOBAL: {
                    Type *t = amd_global_type(prog, I->name);
                    /* <stdarg.h>'s System V mode asks for the four numbers
                     * va_start cannot compute itself: how many integer and
                     * vector registers the named parameters used, and the two
                     * frame addresses.  They are per-call-site constants of the
                     * function being compiled, so they cannot live in data. */
                    if (f->is_variadic && I->name) {
                        if (ark_streq(I->name, "_ark_va__gp_offset")) {
                            asmbuf_puts(&buf, xh_x86_rule("text.276c8636c367c6c1"));
                            asmbuf_int(&buf, xh_x86_number("abi.stack.word") * int_used);
                            asmbuf_putc(&buf, '\n');
                            break;
                        }
                        if (ark_streq(I->name, "_ark_va__fp_offset")) {
                            asmbuf_puts(&buf, xh_x86_rule("text.276c8636c367c6c1"));
                            asmbuf_int(&buf, xh_x86_number("abi.va.gp_bytes") + xh_x86_number("abi.va.fp_stride") * sse_used);
                            asmbuf_putc(&buf, '\n');
                            break;
                        }
                        if (ark_streq(I->name, "_ark_va__overflow_area")) {
                            asmbuf_puts(&buf, xh_x86_rule("text.dfc15ab0f0df0a24"));
                            asmbuf_int(&buf, stk);
                            asmbuf_puts(&buf, xh_x86_rule("text.f4b1b72c766f9a05"));
                            break;
                        }
                        if (ark_streq(I->name, "_ark_va__reg_save_area")) {
                            asmbuf_puts(&buf, xh_x86_rule("text.6361c380597f7fe7"));
                            asmbuf_int(&buf, rs_off);
                            asmbuf_puts(&buf, xh_x86_rule("text.f4b1b72c766f9a05"));
                            break;
                        }
                    }
                    int sz = t ? type_size(t) : 4;
                    int uns = t ? t->is_unsigned : 0;
                    if (sz == 16) {
                        amd_unsupported(&buf, "16-byte global variable");
                        break;
                    }
                    if ((I->aux & IR_AUX_COMPLEX) && sz == 8) {
                        amd_unsupported(&buf, "_Complex float global variable");
                        break;
                    }
                    if (t && t->kind == TYPE_PTR) {
                        asmbuf_puts(&buf, xh_x86_rule("text.7dc7b89e01dd0887"));
                        asmbuf_puts(&buf, I->name);
                        asmbuf_puts(&buf, xh_x86_rule("text.f4b1b72c766f9a05"));
                    } else if (sz >= 8) {
                        asmbuf_puts(&buf, xh_x86_rule("text.7dc7b89e01dd0887"));
                        asmbuf_puts(&buf, I->name);
                        asmbuf_puts(&buf, xh_x86_rule("text.f9416e09f78ba316"));
                        amd_push64(&buf, "rax", "eax");
                    } else if (sz == 1) {
                        asmbuf_puts(&buf, uns ? xh_x86_rule("text.6d4d1dcbcda76ecd") : xh_x86_rule("text.719d4e0b279b0046"));
                        asmbuf_puts(&buf, I->name);
                        asmbuf_puts(&buf, xh_x86_rule("text.f4b1b72c766f9a05"));
                    } else if (sz == 2) {
                        asmbuf_puts(&buf, uns ? xh_x86_rule("text.1a1f6255007ca43a") : xh_x86_rule("text.b862f0104b5e524d"));
                        asmbuf_puts(&buf, I->name);
                        asmbuf_puts(&buf, xh_x86_rule("text.f4b1b72c766f9a05"));
                    } else {
                        asmbuf_puts(&buf, xh_x86_rule("text.2fa68fea27ac0180"));
                        asmbuf_puts(&buf, I->name);
                        asmbuf_puts(&buf, xh_x86_rule("text.f4b1b72c766f9a05"));
                    }
                    break;
                }

                case IR_STORE_GLOBAL: {
                    Type *t = amd_global_type(prog, I->name);
                    int sz = t ? type_size(t) : 4;
                    if (sz == 16) {
                        amd_unsupported(&buf, "16-byte global variable");
                        break;
                    }
                    if ((I->aux & IR_AUX_COMPLEX) && sz == 8) {
                        amd_unsupported(&buf, "_Complex float global variable");
                        break;
                    }
                    if (t && t->kind == TYPE_PTR) {
                        asmbuf_puts(&buf, xh_x86_rule("text.9167fb0b226f1420"));
                        asmbuf_puts(&buf, I->name);
                        asmbuf_puts(&buf, xh_x86_rule("text.42ade291e14ed386"));
                    } else if (sz >= 8) {
                        /* mode64 rejects "[sym+disp]", so materialise the
                         * address first and use a register-relative form. */
                        asmbuf_puts(&buf, xh_x86_rule("text.d9fff63b74c3fc83"));
                        asmbuf_puts(&buf, I->name);
                        asmbuf_puts(&buf, xh_x86_rule("text.93336d78c9066192"));
                    } else if (sz == 1) {
                        asmbuf_puts(&buf, xh_x86_rule("text.8dabc6fad806881f"));
                        asmbuf_puts(&buf, I->name);
                        asmbuf_puts(&buf, xh_x86_rule("text.4e43d6d10f959fcb"));
                    } else if (sz == 2) {
                        asmbuf_puts(&buf, xh_x86_rule("text.f59599ca3be9e93a"));
                        asmbuf_puts(&buf, I->name);
                        asmbuf_puts(&buf, xh_x86_rule("text.2dbd1dc71e0212d3"));
                    } else {
                        asmbuf_puts(&buf, xh_x86_rule("text.2ac11892bbd77222"));
                        asmbuf_puts(&buf, I->name);
                        asmbuf_puts(&buf, xh_x86_rule("text.44b5e9f85f2e61a3"));
                    }
                    break;
                }

                case IR_POP:
                    asmbuf_puts(&buf, xh_x86_rule("text.757e527cdcff9451"));
                    break;

                case IR_DUP: {
                    int nw = (int)I->val;
                    int k;
                    if (nw < 1) nw = 1;
                    if (nw > 4) {
                        amd_unsupported(&buf, "duplicate of more than four words");
                        break;
                    }
                    /* Snapshot first: pushing shifts rsp under the reads. */
                    for (k = 0; k < nw; k++) {
                        asmbuf_puts(&buf, xh_x86_rule("text.15ff3752709e02fd"));
                        asmbuf_int(&buf, xh_x86_number("abi.stack.word") * k);
                        asmbuf_puts(&buf, xh_x86_rule("text.f9416e09f78ba316"));
                        amd_rd_mem(&buf, "mov", "qword", -xh_x86_number("abi.frame.multiword_scratch_base") - xh_x86_number("abi.stack.word") * k, "rax");
                    }
                    for (k = nw - 1; k >= 0; k--) {
                        amd_mem_rd(&buf, "mov", "rax", "qword", -xh_x86_number("abi.frame.multiword_scratch_base") - xh_x86_number("abi.stack.word") * k);
                        asmbuf_puts(&buf, xh_x86_rule("text.de1843ef75a9fb24"));
                    }
                    break;
                }

                case IR_SWAP:
                    asmbuf_puts(&buf, xh_x86_rule("text.b2e9d9f67f0ae551"));
                    break;

                case IR_STORE: {
                    int w = (int)(I->aux & ~(IR_AUX_VOLATILE | IR_AUX_COMPLEX | IR_AUX_SLOT64));
                    int slot64 = (I->aux & IR_AUX_SLOT64) != 0;
                    int d = (int)I->val - framelo;
                    if (w == 16) {
                        amd_unsupported(&buf, "16-byte store");
                        break;
                    }
                    if ((I->aux & IR_AUX_COMPLEX) && w == 8) {
                        amd_unsupported(&buf, "_Complex float store");
                        break;
                    }
                    if (slot64) {
                        asmbuf_puts(&buf, xh_x86_rule("text.755680c4ca6d94df"));
                        amd_rd_mem(&buf, "mov", "qword", d, "rax");
                    } else if (w == 8) {
                        asmbuf_puts(&buf, xh_x86_rule("text.c792e5f5e47b5303"));
                        amd_rd_mem(&buf, "mov", "dword", d, "eax");
                        amd_rd_mem(&buf, "mov", "dword", d + 4, "edx");
                    } else if (w == 1) {
                        asmbuf_puts(&buf, xh_x86_rule("text.755680c4ca6d94df"));
                        amd_rd_mem(&buf, "mov", "byte", d, "al");
                    } else if (w == 2) {
                        asmbuf_puts(&buf, xh_x86_rule("text.755680c4ca6d94df"));
                        amd_rd_mem(&buf, "mov", "word", d, "ax");
                    } else {
                        asmbuf_puts(&buf, xh_x86_rule("text.755680c4ca6d94df"));
                        amd_rd_mem(&buf, "mov", "dword", d, "eax");
                    }
                    break;
                }

                case IR_LOAD_IDX: {
                    int es = (int)I->val;
                    int uns = (I->aux & 0x1) != 0;
                    int slot64 = (I->aux & IR_AUX_SLOT64) != 0;
                    if (es == 16) {
                        amd_unsupported(&buf, "16-byte array element load");
                        break;
                    }
                    if (es == 8 && (I->aux & IR_AUX_COMPLEX)) {
                        amd_unsupported(&buf, "_Complex float array element load");
                        break;
                    }
                    asmbuf_puts(&buf, xh_x86_rule("text.10be0a705c528063"));
                    if (es == 2) asmbuf_puts(&buf, xh_x86_rule("text.414872d99b4c53b6"));
                    else if (es == 4) asmbuf_puts(&buf, xh_x86_rule("text.2baa3a2a80560c83"));
                    else if (es == 8) asmbuf_puts(&buf, xh_x86_rule("text.2f51ac6010cd6939"));
                    asmbuf_puts(&buf, xh_x86_rule("text.4d13042fcef9d73c"));
                    if (slot64) {
                        asmbuf_puts(&buf, xh_x86_rule("text.34c0511551df6359"));
                    } else if (es == 1) {
                        asmbuf_puts(&buf, uns ? xh_x86_rule("text.320286e1233ba360")
                                              : xh_x86_rule("text.f43341e99fe36f5d"));
                        asmbuf_puts(&buf, xh_x86_rule("text.de1843ef75a9fb24"));
                    } else if (es == 2) {
                        asmbuf_puts(&buf, uns ? xh_x86_rule("text.d3364f83450cc6cb")
                                              : xh_x86_rule("text.e9559e54b3b4eded"));
                        asmbuf_puts(&buf, xh_x86_rule("text.de1843ef75a9fb24"));
                    } else if (es == 8) {
                        asmbuf_puts(&buf, xh_x86_rule("text.4d067f21ecf956da"));
                        amd_push64(&buf, "rdx", "edx");
                    } else {
                        asmbuf_puts(&buf, xh_x86_rule("text.c8b44b422dea869a"));
                    }
                    break;
                }

                case IR_STORE_IDX: {
                    int es = (int)I->val;
                    int slot64 = (I->aux & IR_AUX_SLOT64) != 0;
                    if (es == 16) {
                        amd_unsupported(&buf, "16-byte array element store");
                        break;
                    }
                    if (es == 8 && (I->aux & IR_AUX_COMPLEX)) {
                        amd_unsupported(&buf, "_Complex float array element store");
                        break;
                    }
                    if (slot64) {
                        asmbuf_puts(&buf, xh_x86_rule("text.3a90bcf5e794e695"));
                    } else if (es == 8) {
                        asmbuf_puts(&buf, xh_x86_rule("text.96c9447543ed5477"));
                    } else if (es == 1) {
                        asmbuf_puts(&buf, xh_x86_rule("text.ec94bfa1fb7aa1bb"));
                    } else if (es == 2) {
                        asmbuf_puts(&buf, xh_x86_rule("text.275a1dac401948e8"));
                    } else {
                        asmbuf_puts(&buf, xh_x86_rule("text.7ceb7f570d0878aa"));
                    }
                    break;
                }

                case IR_BINARY: {
 x86_scalar_rule(&buf,I->aux & 0xFF,(I->aux & 0x100)!=0,(I->aux & IR_AUX_SLOT64)?64:32);
 break;
 }

                case IR_UNARY: {
                    if (I->aux & IR_AUX_COMPLEX) {
                        amd_unsupported(&buf, "conj");
                        break;
                    }
                    asmbuf_puts(&buf, xh_x86_rule("text.755680c4ca6d94df"));
                    if (I->val == 1 && (int)I->aux == (int)TOK_MINUS) {
                        /* float: the only word holds the sign bit; double: the
                         * high word (on top) does.  Either way the top word is
                         * the one to flip. */
                        asmbuf_puts(&buf, xh_x86_rule("text.49eecb5894f48969"));
                    } else if (I->val == 2 &&
                               ((int)I->aux == (int)TOK_MINUS || (int)I->aux == (int)TOK_BNOT)) {
                        asmbuf_puts(&buf, xh_x86_rule("text.b6ac13acdc833d31"));
                        asmbuf_puts(&buf, xh_x86_rule("text.473243e770661c83"));
                        asmbuf_puts(&buf, (int)I->aux == (int)TOK_MINUS ? xh_x86_rule("text.329878c2358781a6")
                                                                       : xh_x86_rule("text.4be3e7f0ae2e026e"));
                        amd_push64(&buf, "rax", "eax");
                    } else {
                        if ((int)I->aux == (int)TOK_MINUS)
                            asmbuf_puts(&buf, xh_x86_rule("text.e29bd740543bbe7f"));
                        else if ((int)I->aux == (int)TOK_BNOT)
                            asmbuf_puts(&buf, xh_x86_rule("text.cd9069b2dc376ed8"));
                        else
                            asmbuf_puts(&buf, xh_x86_rule("text.fecfa68ae8bbca42"));
                        asmbuf_puts(&buf, xh_x86_rule("text.de1843ef75a9fb24"));
                    }
                    break;
                }

                case IR_TRUNC: {
                    asmbuf_puts(&buf, xh_x86_rule("text.755680c4ca6d94df"));
                    if (I->val == 8)
                        asmbuf_puts(&buf, I->aux ? xh_x86_rule("text.344ade76d4acecae") : xh_x86_rule("text.fe88e51f8712bfcd"));
                    else if (I->val == 16)
                        asmbuf_puts(&buf, I->aux ? xh_x86_rule("text.edd53f9a0d5b9f63") : xh_x86_rule("text.8832c5e95cb1e8dd"));
                    asmbuf_puts(&buf, xh_x86_rule("text.de1843ef75a9fb24"));
                    break;
                }

                case IR_LABEL:
                    asmbuf_puts(&buf, xh_x86_rule("text.4b6d79fc377f87b4"));
                    asmbuf_puts(&buf, f->name);
                    asmbuf_putc(&buf, '_');
                    asmbuf_int(&buf, I->val);
                    asmbuf_puts(&buf, xh_x86_rule("text.f3743a0a18e53d13"));
                    break;

                case IR_LABEL_NAMED:
                    asmbuf_puts(&buf, xh_x86_rule("text.be1de5990bdec1cd"));
                    asmbuf_puts(&buf, f->name);
                    asmbuf_putc(&buf, '_');
                    asmbuf_puts(&buf, I->name);
                    asmbuf_puts(&buf, xh_x86_rule("text.f3743a0a18e53d13"));
                    break;

                case IR_JMP:
                    asmbuf_puts(&buf, xh_x86_rule("text.02b129850dd58dc5"));
                    asmbuf_puts(&buf, f->name);
                    asmbuf_putc(&buf, '_');
                    asmbuf_int(&buf, I->val);
                    asmbuf_putc(&buf, '\n');
                    break;

                case IR_JMP_NAMED:
                    asmbuf_puts(&buf, xh_x86_rule("text.66fe547640031826"));
                    asmbuf_puts(&buf, f->name);
                    asmbuf_putc(&buf, '_');
                    asmbuf_puts(&buf, I->name);
                    asmbuf_putc(&buf, '\n');
                    break;

                case IR_JZ:
                case IR_JNZ:
                    asmbuf_puts(&buf, xh_x86_rule("text.e04c6c878bc1cbdf"));
                    asmbuf_puts(&buf, I->op == IR_JZ ? xh_x86_rule("text.7c5dca04b91ccf52") : xh_x86_rule("text.a472f8f0f8e38a6a"));
                    asmbuf_puts(&buf, f->name);
                    asmbuf_putc(&buf, '_');
                    asmbuf_int(&buf, I->val);
                    asmbuf_putc(&buf, '\n');
                    break;

                case IR_CMP_JMP_EQ:
                    asmbuf_puts(&buf, xh_x86_rule("text.611f0f285fc162a0"));
                    asmbuf_int(&buf, I->val);
                    asmbuf_puts(&buf, xh_x86_rule("text.30a89a7de4ba7b85"));
                    asmbuf_puts(&buf, f->name);
                    asmbuf_puts(&buf, xh_x86_rule("text.1ac5c327b8b33635"));
                    asmbuf_int(&buf, i);
                    asmbuf_puts(&buf, xh_x86_rule("text.3fb9af8ed22e7a0c"));
                    asmbuf_puts(&buf, f->name);
                    asmbuf_putc(&buf, '_');
                    asmbuf_int(&buf, I->aux);
                    asmbuf_puts(&buf, xh_x86_rule("text.c54b801b3c7f4a0c"));
                    asmbuf_puts(&buf, f->name);
                    asmbuf_puts(&buf, xh_x86_rule("text.1ac5c327b8b33635"));
                    asmbuf_int(&buf, i);
                    asmbuf_puts(&buf, xh_x86_rule("text.f3743a0a18e53d13"));
                    break;

                case IR_CALL: {
                    int n = (int)I->val;
                    int argmask = (int)I->aux;
                    int is_indirect = (I->name && I->name[0] == '?');
                    const int *wires = (I->aux2 > 0) ? &prog->argbytes[I->aux2 - 1] : 0;
                    int same[64], cls[64], where[64];
                    int int_used = 0, sse_used = 0, nstk = 0;
                    int k;
                    /* A struct/union result wider than 8 bytes is returned
                     * through a buffer this caller owns; the callee receives
                     * its address as the hidden first integer argument, so the
                     * real argument 0 moves to rsi and every register number
                     * shifts by one.  The pre-scan above reserved the buffer,
                     * in code order, so the cursor walks it in the same order
                     * this switch does. */
                    int sret_call = (int)((I->val >> 36) & 1);
                    int sret_buf = 0;
                    if (sret_call) {
                        if (sret_site_cur < sret_site_n) {
                            sret_buf = sret_area + sret_off_tab[sret_site_cur] +
                                       sret_sz_tab[sret_site_cur];
                            sret_site_cur++;
                        } else {
                            amd_unsupported(&buf,
                                "more struct-returning calls than were reserved");
                            sret_call = 0;
                        }
                    }
                    if (sret_call) int_used = 1;

                    if (is_indirect) {
                        asmbuf_puts(&buf, xh_x86_rule("text.ccee9788ee630a91"));
                        n -= 1;
                    }
                    if (n < 0) n = 0;
                    if (n > xh_x86_number("abi.call.max_arguments")) {
                        amd_unsupported(&buf, "call with more than 64 arguments");
                        break;
                    }
                    /* Pop every argument into its frame slot, preserving the
                     * two-word order: high word first (it is on top). */
                    for (k = 0; k < n; k++) {
                        int b, w, slot64, tmp;
                        if (wires) {
                            b = wires[k] & 0xFFFF;
                            slot64 = (wires[k] & 0x20000) != 0;
                        } else {
                            b = (k < 16 && ((argmask >> (2 * k + 1)) & 1)) ? 8 : 4;
                            slot64 = 0;
                        }
                        if (b <= 0) b = 4;
                        w = slot64 ? 1 : (b > 4 ? 2 : 1);
                        tmp = -framelo - xh_x86_number("abi.stack.word") * (k + 1);
                        if (w == 2) {
                            asmbuf_puts(&buf, xh_x86_rule("text.d0aef66727d08b37"));
                            amd_rd_mem(&buf, "mov", "dword", tmp + 4, "edx");
                            asmbuf_puts(&buf, xh_x86_rule("text.755680c4ca6d94df"));
                            amd_rd_mem(&buf, "mov", "dword", tmp, "eax");
                        } else {
                            asmbuf_puts(&buf, xh_x86_rule("text.755680c4ca6d94df"));
                            amd_rd_mem(&buf, "mov", "qword", tmp, "rax");
                        }
                        /* New side tables preserve FP class beyond the legacy
                         * 16-argument bitmask; old serialized IR remains valid. */
                        same[k] = ((wires && (wires[k] & 0x40000)) ||
                                   (k < 16 && ((argmask >> (2 * k)) & 1))) ? 1 : 0;
                    }
                    /* Assign each argument to a register or to a stack slot.
                     * SysV allocates the two register files independently: an
                     * argument goes to an integer register while fewer than 6
                     * are used, or to an SSE register while fewer than 8 are,
                     * and only spills to the stack when its own class is
                     * exhausted.
                     *
                     * cls[k]:  0 = integer register, 1 = SSE register,
                     *          2 = stack slot.  where[k]: register number,
                     *          or the stack slot index.
                     * These used to share one int ("-2 - i" for xmm, anything
                     * non-negative for both registers and stack), which made
                     * "stack" indistinguishable from "integer register": every
                     * stack argument was loaded into rdi/rsi/... and the real
                     * registers were overwritten. */
                    for (k = 0; k < n; k++) {
                        if (same[k] && sse_used < xh_x86_number("abi.vector.count")) {
                            cls[k] = 1; where[k] = sse_used++;
                        } else if (!same[k] && int_used < xh_x86_number("abi.integer.count")) {
                            cls[k] = 0; where[k] = int_used++;
                        } else {
                            cls[k] = 2; where[k] = nstk++;
                        }
                    }
                    asmbuf_puts(&buf, xh_x86_rule("text.d142bbe448005ce7"));
                    if (nstk > 0) {
                        asmbuf_puts(&buf, xh_x86_rule("text.cb03efe8ee360a4b"));
                        asmbuf_int(&buf, x86_abi_align(xh_x86_number("abi.stack.word")*nstk,"abi.frame.alignment"));
                        asmbuf_putc(&buf, '\n');
                    }
                    /* Stack arguments, left to right, each in its own 8-byte slot. */
                    {
                        int next = 0;
                        for (k = 0; k < n; k++) {
                            int b, tmp;
                            if (cls[k] != 2) continue;
                            if (wires) b = wires[k] & 0xFFFF;
                            else b = (k < 16 && ((argmask >> (2 * k + 1)) & 1)) ? 8 : 4;
                            (void)b;
                            tmp = -framelo - xh_x86_number("abi.stack.word") * (k + 1);
                            amd_mem_rd(&buf, "mov", "rax", "qword", tmp);
                            asmbuf_puts(&buf, xh_x86_rule("text.e4109461abaf377f"));
                            asmbuf_int(&buf, xh_x86_number("abi.stack.word") * next);
                            asmbuf_puts(&buf, xh_x86_rule("text.42ade291e14ed386"));
                            next++;
                        }
                    }
                    /* Register arguments. */
                    for (k = 0; k < n; k++) {
                        int b, tmp;
                        if (cls[k] == 2) continue;
                        if (wires) b = wires[k] & 0xFFFF;
                        else b = (k < 16 && ((argmask >> (2 * k + 1)) & 1)) ? 8 : 4;
                        tmp = -framelo - xh_x86_number("abi.stack.word") * (k + 1);
                        if (cls[k] == 1) {
                            asmbuf_puts(&buf, xh_x86_rule("text.cc3836120ed5ecf8"));
                            asmbuf_int(&buf, where[k]);
                            asmbuf_puts(&buf, xh_x86_rule("text.d5ab32bec840f3fe"));
                            amd_addr(&buf, tmp);
                            asmbuf_putc(&buf, '\n');
                        } else {
                            /* Always the 64-bit move.  The frame slot is one
                             * full word -- the value was popped with "pop rax"
                             * and stored with a qword move -- so a 4-byte load
                             * here threw away the top half of an address.
                             * That only showed up on an indirect call, which
                             * has no per-argument width table (prog->argbytes
                             * is filled for named callees), so every pointer
                             * argument of a callback came in truncated.  SysV
                             * leaves the upper half of a register unspecified
                             * for narrower arguments, so widening is legal. */
                            amd_mem_rd(&buf, "mov", amd_argreg64(where[k]), "qword", tmp);
                        }
                    }
                    /* The hidden result buffer goes into rdi last: the
                     * register-argument loop above has just filled
                     * rdi/rsi/... from rsi/rdx/... and would clobber an
                     * earlier write.  rax is free here -- the next use is
                     * `mov al, sse_used`. */
                    if (sret_call) {
                        asmbuf_puts(&buf, xh_x86_rule("text.35bbb883dbc56169"));
                        amd_addr(&buf, -sret_buf);
                        asmbuf_putc(&buf, '\n');
                        asmbuf_puts(&buf, xh_x86_rule("text.d4aa8d11e462616f"));
                    }
                    /* al = number of vector registers used (SysV varargs). */
                    asmbuf_puts(&buf, xh_x86_rule("text.112b8d88b530b012"));
                    asmbuf_int(&buf, sse_used);
                    asmbuf_putc(&buf, '\n');
                    if (is_indirect) {
                        asmbuf_puts(&buf, xh_x86_rule("text.02d54d9705b644c1"));
                    } else {
                        asmbuf_puts(&buf, xh_x86_rule("text.7e873a94c44eeb9e"));
                        asmbuf_puts(&buf, I->name);
                        asmbuf_putc(&buf, '\n');
                    }
                    asmbuf_puts(&buf, xh_x86_rule("text.b0d7482d199fb842"));
                    if (sret_call) {
                        /* The IR value of a struct-returning call is the
                         * address of the result, and the front end copies the
                         * object out of it.  SysV does promise the address
                         * back in rax, but re-deriving it from the frame costs
                         * one lea and removes any dependence on the callee. */
                        asmbuf_puts(&buf, xh_x86_rule("text.35bbb883dbc56169"));
                        amd_addr(&buf, -sret_buf);
                        asmbuf_putc(&buf, '\n');
                    }
                    {
                        int ret2 = (int)((I->val >> 32) & 1);
                        int retvoid = (int)((I->val >> 33) & 1);
                        int retfloat = (int)((I->val >> 53) & 1);
                        if (retvoid) break;
                        if (retfloat) {
                            /* A floating-point result arrives in xmm0, but the
                             * IR stack convention is unchanged: the stack top is
                             * the LOW word, and a double occupies two words while
                             * a float occupies one.  Pushing a single word for a
                             * double (the old code) left the high word missing, so
                             * every call whose result was consumed by IR_FADD etc.
                             * popped the callee's argument half out of the previous
                             * frame slot -- f_add(9.5,5.5), f_mul, g1, g5 and g8 all
                             * read 0 on x86_64 after the bit-53 encoding fix made
                             * this branch reachable for the first time. */
                            asmbuf_puts(&buf, xh_x86_rule("text.71125b5563b4d27f"));
                            if (ret2)
                                amd_push64(&buf, "rax", "eax");
                            else
                                asmbuf_puts(&buf, xh_x86_rule("text.3520170c55ba558b"));
                        } else if (ret2) {
                            /* A two-word result comes back as one full 64-bit
                             * value in rax (see IR_RETURN: amd_pop64 rebuilds
                             * it there).  Splitting it with "mov edx, edx"
                             * only worked while the callee happened to leave
                             * the high word in rdx: amd_pop64 leaves hi<<32
                             * there instead, so the high word read back as 0
                             * and every 64-bit result was truncated to its
                             * low 32 bits.  Split rax explicitly. */
                            asmbuf_puts(&buf, xh_x86_rule("text.5448a6e84572d981"));
                        } else {
                            asmbuf_puts(&buf, xh_x86_rule("text.de1843ef75a9fb24"));
                        }
                    }
                    break;
                }

                case IR_RETURN: {
                    Type *rt = f->ret_type;
                    int two = (rt && (type_is_64bit_integer(rt) || rt->kind == TYPE_DOUBLE)) ||
                              (rt && (rt->kind == TYPE_STRUCT || rt->kind == TYPE_UNION) &&
                               type_size(rt) > xh_x86_number("abi.ir.word") && type_size(rt) <= xh_x86_number("abi.aggregate.direct_limit"));
                    if (sret_used) {
                        /* SysV returns a composite wider than 8 bytes through a
                         * buffer the caller owns.  irgen pushes the *address*
                         * of the returned object as this IR_RETURN's operand,
                         * so the copy into that buffer belongs here -- the arm
                         * and riscv32 backends do the same in their epilogue.
                         * Without it the callee never wrote the caller's buffer
                         * and `struct T3 t = mk3(5)` read uninitialised memory in
                         * every field, while the <= 8-byte register path and the
                         * 16-byte struct pass-by-value path stayed correct.
                         * rep movsb clobbers rdi/rsi/rcx: nothing is live in an
                         * argument register across a statement, and IR_MEMCPY
                         * already uses exactly these three. */
                        int rsz = type_size(rt);
                        if (rsz <= 0) rsz = 8;
                        asmbuf_puts(&buf, xh_x86_rule("text.975d1ac3de00bf49"));
                        asmbuf_int(&buf, rsz);
                        asmbuf_puts(&buf, xh_x86_rule("text.c129ba0807158854"));
                    } else if (two) {
                        amd_pop64(&buf, "rax", "eax");
                    } else {
                        asmbuf_puts(&buf, xh_x86_rule("text.5e083d46fce61f03"));
                    }
                    if (rt && (rt->kind == TYPE_FLOAT || rt->kind == TYPE_DOUBLE))
                        asmbuf_puts(&buf, xh_x86_rule("text.666c9c0127f987ab"));
                    asmbuf_puts(&buf, xh_x86_rule("text.433c6caaac074862"));
                    break;
                }

                case IR_PUSH_FUNC:
                    asmbuf_puts(&buf, xh_x86_rule("text.ba1d0a3c1a91e644"));
                    asmbuf_puts(&buf, I->name);
                    asmbuf_puts(&buf, xh_x86_rule("text.f4b1b72c766f9a05"));
                    break;

                case IR_PUSH_GLOBAL_ADDR:
                    asmbuf_puts(&buf, xh_x86_rule("text.ba1d0a3c1a91e644"));
                    asmbuf_puts(&buf, I->name);
                    asmbuf_puts(&buf, xh_x86_rule("text.f4b1b72c766f9a05"));
                    break;

                case IR_MEMCPY:
                    /* irgen pushes dst, src and then dst again, so the two pops
                     * below are the whole cleanup. */
                    asmbuf_puts(&buf, xh_x86_rule("text.6a557a74cc347725"));
                    asmbuf_int(&buf, I->val);
                    asmbuf_puts(&buf, xh_x86_rule("text.8d3fc2867d2efe58"));
                    break;

                case IR_ALLOCA:
                    asmbuf_puts(&buf, xh_x86_rule("text.e0a9440bd43c1f8a"));
                    break;

                case IR_DIV:
                case IR_MOD:
                    asmbuf_puts(&buf, xh_x86_rule("text.96f5981a878f5fa4"));
                    if (I->aux & 1)
                        asmbuf_puts(&buf, xh_x86_rule("text.ad142bfd1df4af66"));
                    else
                        asmbuf_puts(&buf, xh_x86_rule("text.767d3f8a4f58efa7"));
                    asmbuf_puts(&buf, I->op == IR_DIV ? xh_x86_rule("text.de1843ef75a9fb24") : xh_x86_rule("text.d030bef8d2dfffbe"));
                    break;

                case IR_PUSH_FLOAT: {
                    int is_dbl = (I->aux & 0x200) != 0;
                    if (I->aux & IR_AUX_COMPLEX) {
                        amd_unsupported(&buf, "_Complex constant");
                        break;
                    }
                    if (is_dbl) {
                        double v = (I->val >= 0 && I->val < prog->float_count) ?
                                   prog->floats[I->val] : 0.0;
                        union { double d; unsigned long long i; } u;
                        u.d = v;
                        amd_push_imm64(&buf, u.i);
                    } else {
                        float fv = (I->val >= 0 && I->val < prog->float_count) ?
                                   (float)prog->floats[I->val] : 0.0f;
                        union { float f; unsigned int i; } u;
                        u.f = fv;
                        amd_push_imm32(&buf, (long long)u.i);
                    }
                    break;
                }

                case IR_FADD:
                case IR_FSUB:
                case IR_FMUL:
                case IR_FDIV: {
                    const char *op_s = "addsd";
                    int is_dbl;
                    if ((I->aux & 0xF00) == 0x900) {
                        amd_unsupported(&buf, "cexp");
                        break;
                    }
                    if (I->aux & IR_AUX_COMPLEX) {
                        amd_unsupported(&buf, "_Complex arithmetic");
                        break;
                    }
                    switch (I->op) {
                        case IR_FADD: op_s = "addsd"; break;
                        case IR_FSUB: op_s = "subsd"; break;
                        case IR_FMUL: op_s = "mulsd"; break;
                        case IR_FDIV: op_s = "divsd"; break;
                        default: break;
                    }
                    is_dbl = (I->aux & 0x200) != 0;
                    if (is_dbl) {
                        amd_pop64(&buf, "rax", "eax");   /* b */
                        amd_pop64(&buf, "rcx", "ecx");   /* a */
                        asmbuf_puts(&buf, xh_x86_rule("text.fdf5cf8fa9ab9307"));
                        asmbuf_puts(&buf, op_s);
                        asmbuf_puts(&buf, xh_x86_rule("text.90bc70c67f005071"));
                        amd_push64(&buf, "rax", "eax");
                    } else {
                        /* No single-precision ALU in this assembler: widen both
                         * operands, compute in double, narrow back.  Rounding
                         * twice is exact for + - * / because 53 >= 2*24 + 2. */
                        asmbuf_puts(&buf, xh_x86_rule("text.4cc8939188beca23"));
                        asmbuf_puts(&buf, op_s);
                        asmbuf_puts(&buf, xh_x86_rule("text.78576d88a1d6d704"));
                        amd_store_fp(&buf, 0);
                    }
                    break;
                }

                case IR_FCMP: {
                    int is_dbl = (I->aux & 0x200) != 0;
                    int op = (int)(I->aux & 0xFF);
                    if (I->aux & IR_AUX_COMPLEX) {
                        amd_unsupported(&buf, "_Complex comparison");
                        break;
                    }
                    if (is_dbl) {
                        amd_pop64(&buf, "rax", "eax");
                        amd_pop64(&buf, "rcx", "ecx");
                        asmbuf_puts(&buf, xh_x86_rule("text.af40109a3e4c5b8d"));
                    } else {
                        asmbuf_puts(&buf, xh_x86_rule("text.bf90b28855981548"));
                    }
                    amd_fcmp(&buf, op);
                    break;
                }

                case IR_F2I: {
                    int src_dbl = (I->aux & 0x200) != 0;
                    int dst_uns = (I->aux & 0x400) != 0;
                    if (I->aux & IR_AUX_COMPLEX) {
                        amd_unsupported(&buf, "creal/cimag/cabs/carg");
                        break;
                    }
                    if (src_dbl) {
                        amd_pop64(&buf, "rax", "eax");
                        asmbuf_puts(&buf, xh_x86_rule("text.666c9c0127f987ab"));
                    } else {
                        asmbuf_puts(&buf, xh_x86_rule("text.efbd6e805ebcde33"));
                    }
                    if (dst_uns) {
                        /* The 64-bit form covers the whole unsigned 32-bit
                         * range; the low half is then the answer. */
                        asmbuf_puts(&buf, xh_x86_rule("text.b984325137f3b5f0"));
                    } else {
                        asmbuf_puts(&buf, xh_x86_rule("text.8cc0cd8c22cadc72"));
                    }
                    asmbuf_puts(&buf, xh_x86_rule("text.de1843ef75a9fb24"));
                    break;
                }

                case IR_I2F: {
                    int dst_dbl = (I->aux & 0x200) != 0;
                    int src_uns = (I->aux & 0x400) != 0;
                    if (I->aux & IR_AUX_COMPLEX) {
                        amd_unsupported(&buf, "real to complex conversion");
                        break;
                    }
                    asmbuf_puts(&buf, xh_x86_rule("text.755680c4ca6d94df"));
                    if (src_uns)
                        asmbuf_puts(&buf, xh_x86_rule("text.5829b49189e5d8da"));
                    else
                        asmbuf_puts(&buf, xh_x86_rule("text.806e40da505153d5"));
                    asmbuf_puts(&buf, xh_x86_rule("text.e90ca0e5f84313f1"));
                    amd_store_fp(&buf, dst_dbl);
                    break;
                }

                case IR_F2F: {
                    if (I->aux & 0x200) {
                        /* double -> float */
                        amd_pop64(&buf, "rax", "eax");
                        asmbuf_puts(&buf, xh_x86_rule("text.666c9c0127f987ab"));
                        amd_store_fp(&buf, 0);
                    } else if (I->aux & 0x100) {
                        /* float -> double */
                        asmbuf_puts(&buf, xh_x86_rule("text.2a7b9abdcdd1df24"));
                        amd_push64(&buf, "rax", "eax");
                    } else {
                        /* Same width: irgen does not emit it, but keep the
                         * value on the stack if it ever does. */
                    }
                    break;
                }

                case IR_64PUSH:
                    amd_push_imm64(&buf,
                                   ((unsigned long long)(unsigned int)(int)I->val << 32) |
                                   (unsigned int)I->aux);
                    break;

                case IR_64BINARY: {
                    int op = (int)(I->aux & 0xFF);
                    int uns = (I->aux & 0x100) != 0;
                    const char *bop = binop_x(op);
                    amd_pop64(&buf, "rcx", "ecx");   /* b */
                    amd_pop64(&buf, "rax", "eax");   /* a */
                    if (bop) {
                        asmbuf_puts(&buf, xh_x86_rule("text.1a0f564ddc603945"));
                        asmbuf_puts(&buf, bop);
                        asmbuf_puts(&buf, xh_x86_rule("text.9bb2ec8aa477608c"));
                        amd_push64(&buf, "rax", "eax");
                    } else if (op == (int)TOK_MUL || op == (int)TOK_DIV ||
                               op == (int)TOK_MOD || op == (int)TOK_SHL ||
                               op == (int)TOK_SHR) {
                        if (op == (int)TOK_MUL) {
                            asmbuf_puts(&buf, xh_x86_rule("text.dd74d87037e1c8a8"));
                        } else if (op == (int)TOK_DIV || op == (int)TOK_MOD) {
                            asmbuf_puts(&buf, uns ? xh_x86_rule("text.8bda95a54d415775")
                                                  : xh_x86_rule("text.bc370da3c4dbbf24"));
                            if (op == (int)TOK_MOD)
                                asmbuf_puts(&buf, xh_x86_rule("text.d729da48110992c8"));
                        } else if (op == (int)TOK_SHL) {
                            asmbuf_puts(&buf, xh_x86_rule("text.3425d160c827d5dc"));
                        } else {
                            asmbuf_puts(&buf, uns ? xh_x86_rule("text.b0d97196713fb33b") : xh_x86_rule("text.b3ad277704da6177"));
                        }
                        amd_push64(&buf, "rax", "eax");
                    } else {
                        const char *cc = amd_cc(op, uns);
                        asmbuf_puts(&buf, xh_x86_rule("text.5bd62f2b9392f0ab"));
                        asmbuf_puts(&buf, cc);
                        asmbuf_puts(&buf, xh_x86_rule("text.bd3376dd310a01b5"));
                    }
                    break;
                }

                case IR_64CMP: {
                    int op = (int)(I->aux & 0xFF);
                    int uns = (I->aux & 0x100) != 0;
                    const char *cc = amd_cc(op, uns);
                    amd_pop64(&buf, "rcx", "ecx");
                    amd_pop64(&buf, "rax", "eax");
                    asmbuf_puts(&buf, xh_x86_rule("text.5bd62f2b9392f0ab"));
                    asmbuf_puts(&buf, cc);
                    asmbuf_puts(&buf, xh_x86_rule("text.bd3376dd310a01b5"));
                    break;
                }

                case IR_64F2I: {
                    int src_dbl = (I->aux & 0x200) != 0;
                    int dst_uns = (I->aux & 0x400) != 0;
                    if (src_dbl) {
                        amd_pop64(&buf, "rax", "eax");
                        asmbuf_puts(&buf, xh_x86_rule("text.666c9c0127f987ab"));
                    } else {
                        asmbuf_puts(&buf, xh_x86_rule("text.efbd6e805ebcde33"));
                    }
                    if (dst_uns) {
                        asmbuf_puts(&buf, xh_x86_rule("text.207af587925b69e5"));
                        asmbuf_puts(&buf, f->name);
                        asmbuf_puts(&buf, xh_x86_rule("text.35961bcff24ffbb8"));
                        asmbuf_int(&buf, i);
                        asmbuf_puts(&buf, xh_x86_rule("text.9ed4773e60445886"));
                        asmbuf_puts(&buf, f->name);
                        asmbuf_puts(&buf, xh_x86_rule("text.f93897535644328c"));
                        asmbuf_int(&buf, i);
                        asmbuf_puts(&buf, xh_x86_rule("text.c54b801b3c7f4a0c"));
                        asmbuf_puts(&buf, f->name);
                        asmbuf_puts(&buf, xh_x86_rule("text.35961bcff24ffbb8"));
                        asmbuf_int(&buf, i);
                        asmbuf_puts(&buf, xh_x86_rule("text.c78cc2578921b416"));
                        asmbuf_puts(&buf, f->name);
                        asmbuf_puts(&buf, xh_x86_rule("text.f93897535644328c"));
                        asmbuf_int(&buf, i);
                        asmbuf_puts(&buf, xh_x86_rule("text.f3743a0a18e53d13"));
                    } else {
                        asmbuf_puts(&buf, xh_x86_rule("text.83267ea010e4594c"));
                    }
                    amd_push64(&buf, "rax", "eax");
                    break;
                }

                case IR_64I2F: {
                    int dst_dbl = (I->aux & 0x200) != 0;
                    int src_uns = (I->aux & 0x400) != 0;
                    amd_pop64(&buf, "rax", "eax");
                    if (src_uns) {
                        asmbuf_puts(&buf, xh_x86_rule("text.403bf3de62279b59"));
                        asmbuf_puts(&buf, f->name);
                        asmbuf_puts(&buf, xh_x86_rule("text.cb0a2fa0d24b4edc"));
                        asmbuf_int(&buf, i);
                        asmbuf_puts(&buf, xh_x86_rule("text.195beb5ae1d61c37"));
                        asmbuf_puts(&buf, f->name);
                        asmbuf_puts(&buf, xh_x86_rule("text.80656b170c4f469a"));
                        asmbuf_int(&buf, i);
                        asmbuf_puts(&buf, xh_x86_rule("text.c54b801b3c7f4a0c"));
                        asmbuf_puts(&buf, f->name);
                        asmbuf_puts(&buf, xh_x86_rule("text.cb0a2fa0d24b4edc"));
                        asmbuf_int(&buf, i);
                        asmbuf_puts(&buf, xh_x86_rule("text.5c67d0d3b542f472"));
                        asmbuf_puts(&buf, f->name);
                        asmbuf_puts(&buf, xh_x86_rule("text.80656b170c4f469a"));
                        asmbuf_int(&buf, i);
                        asmbuf_puts(&buf, xh_x86_rule("text.f3743a0a18e53d13"));
                    } else {
                        asmbuf_puts(&buf, xh_x86_rule("text.e90ca0e5f84313f1"));
                    }
                    amd_store_fp(&buf, dst_dbl);
                    break;
                }

                case IR_STORE_FLD: {
                    int bit_off = (int)I->val;
                    int bw = (int)I->aux;
                    unsigned mask = (bw >= 32) ? 0xFFFFFFFFu : ((1u << bw) - 1u);
                    unsigned field = (unsigned)(mask << bit_off);
                    /* Same contract as the emitter above: consume
                     * [addr, value], push nothing, keep the neighbouring bits. */
                    asmbuf_puts(&buf, xh_x86_rule("text.d42526e17a4315a9"));
                    asmbuf_int(&buf, (long long)mask);
                    if (bit_off > 0) {
                        asmbuf_puts(&buf, xh_x86_rule("text.8b3cc64fba698ba0"));
                        asmbuf_int(&buf, bit_off);
                    }
                    asmbuf_puts(&buf, xh_x86_rule("text.f6e886e4901ed44b"));
                    asmbuf_int(&buf, (long long)(unsigned int)(~field));
                    asmbuf_puts(&buf, xh_x86_rule("text.f7ba77dc30202797"));
                    break;
                }

                case IR_LOAD_FLD: {
                    int bit_off = (int)I->val;
                    int bw = (int)I->aux & 0xFF;
                    int is_signed = ((int)I->aux & 0x100) != 0;
                    unsigned mask = (bw >= 32) ? 0xFFFFFFFFu : ((1u << bw) - 1u);
                    asmbuf_puts(&buf, xh_x86_rule("text.6d0b3fef17dcd889"));
                    if (bit_off > 0) {
                        asmbuf_puts(&buf, xh_x86_rule("text.b28953ccda3e8afd"));
                        asmbuf_int(&buf, bit_off);
                        asmbuf_putc(&buf, '\n');
                    }
                    asmbuf_puts(&buf, xh_x86_rule("text.7ef30c4fffaf7a16"));
                    asmbuf_int(&buf, (long long)mask);
                    if (is_signed && bw > 0 && bw < 32) {
                        asmbuf_puts(&buf, xh_x86_rule("text.8b3cc64fba698ba0"));
                        asmbuf_int(&buf, 32 - bw);
                        asmbuf_puts(&buf, xh_x86_rule("text.ceda75f1be4f9e1a"));
                        asmbuf_int(&buf, 32 - bw);
                    }
                    asmbuf_puts(&buf, xh_x86_rule("text.475f15810fba1372"));
                    break;
                }

                case IR_ASM:
                    if(I->aux&0x40000000){xh_x86_inline_asm(&buf,I,type_ptr_size()==8);break;}
                    if (I->name) {
                        const char *s = I->name;
                        if (s[0] == '@') {
                            s++;
                            while (*s == ' ' || *s == '\t') s++;
                            if (*s == '"') {
                                const char *q = s + 1;
                                while (*q && *q != '"') {
                                    if (*q == '%' && q[1] >= '0' && q[1] <= '9') {
                                        ark_log("[x86-64] error: inline asm operand %%%c cannot be "
                                                "substituted by the x86_64 backend\n", q[1]);
                                        g_amd64_errors++;
                                        break;
                                    }
                                    q++;
                                }
                                s++;
                                while (*s && *s != '"') {
                                    asmbuf_putc(&buf, *s);
                                    s++;
                                }
                            }
                        } else {
                            asmbuf_puts(&buf, s);
                        }
                        asmbuf_putc(&buf, '\n');
                    }
                    break;

                case IR_SVC:
                case IR_SMC:
                case IR_SIMD_ADD16:
                case IR_SIMD_SUB16:
                case IR_SIMD_ADD8:
                case IR_SIMD_SUB8:
                    asmbuf_puts(&buf, xh_x86_rule("text.ab3f82bd50fcee78"));
                    asmbuf_puts(&buf, ir_op_name(I->op));
                    asmbuf_puts(&buf, xh_x86_rule("text.0ab8a5dbb336271c"));
                    break;

                default:
                    asmbuf_puts(&buf, xh_x86_rule("text.35815f9c362b56b9"));
                    asmbuf_puts(&buf, ir_op_name(I->op));
                    asmbuf_putc(&buf, '\n');
                    break;
            }
        }
        if (body_ok) {
            if (sret_used)
                asmbuf_puts(&buf, xh_x86_rule("text.e888ee0df925654a"));
            else
                asmbuf_puts(&buf, xh_x86_rule("text.e57fab16113b0a0c"));
            asmbuf_puts(&buf, xh_x86_rule("text.43cbd64c703a3e68"));
        } else {
            asmbuf_puts(&buf, xh_x86_rule("text.43cbd64c703a3e68"));
        }
    }

    {
        int n = buf.len;
        int i;
        int errs = g_amd64_errors + xh_x86_rules_failed();
        g_amd64_errors = 0;
        if (errs) {
            ark_log("[x86-64] error: %d unsupported construct(s), no output written\n", errs);
            if (cap > 0) out[0] = 0;
            return -1;
        }
        if (n + 1 > cap) {
            /* A truncated program is an answer nobody asked for:
             * the text would be cut mid-instruction and the
             * assembler would report a nonsense mnemonic (measured:
             * "unknown mnemonic P") instead of "buffer too small".
             * Refuse, so the driver can name ARK_ASM_BUF_SIZE. */
            if (cap > 0) out[0] = 0;
            return -1;
        }
        for (i = 0; i < n; i++) out[i] = buf.buf[i];
        out[n] = 0;
        return n;
    }
}

static int amd64_emit_sysv(IrProgram *prog, char *out, int cap) {
    return amd64_emit_internal(prog, out, cap);
}

/* === Built-in x86 32-bit assembler ===
 *
 * Encodes Intel-syntax (.intel_syntax noprefix) assembly text into x86
 * machine code. Two-pass design: pass1 computes instruction sizes and
 * collects label addresses; pass2 emits bytes and records forward refs;
 * resolve_refs patches the refs. All jumps use near (4-byte disp32) form
 * so pass1 sizes are fixed regardless of branch distance.
 *
 * Relocation types (for cross-object refs, consumed by the ARKOBJ2 linker):
 *   4 = absolute 32-bit address (R_386_32, same encoding as ARM R_ARM_ABS32)
 *   5 = PC-relative 32-bit disp  (R_386_PC32, for jmp/call/jcc disp32) */

/* is_global: the assembly said ".globl <name>" for this label.  Only those
 * labels are exported to the ARKOBJ2 symbol table.  Before this the exporter
 * treated "does not start with .L" as "is global", so every `static` function
 * in a header -- stdarg.h's ark_va__gp / ark_va__fp / ark_va__mem /
 * ark_va__gp_or_mem are exactly that -- became a strong global in every object
 * that included it, and a 25-object link printed
 * "duplicate strong symbol ark_va__gp, using first definition" three times
 * over and bound every TU's calls to the first object's copy.  The .globl in
 * the emitted text was already there and was being ignored. */
typedef struct { char *name; int addr; int is_weak; int is_global; } X86AsmLabel;
/* addend: the `+disp` of a `[symbol+disp]` operand.  Relocation types 8/9
 * carry it in the site's disp32 field; for a label this module defines the
 * addend is folded in here instead.  Always 0 for plain [symbol]. */
typedef struct { int pc; char *name; int type; int addend; int width; } X86AsmRef;

typedef struct {
    const char    *src;
    unsigned char *out;
    int            pc;
    int            out_cap;
    X86AsmLabel   *labels;
    int            label_count, label_cap;
    X86AsmRef     *refs;
    int            ref_count, ref_cap;
    int            error_count;
    int            cur_section;  /* 0=.text, 1=.data, 2=.rodata */
    int            pending_weak;
    /* Names the assembly declared with .globl/.global.  Kept as its own list so
     * a .globl that appears before or after the label definition both work. */
    char         **globals;
    int            global_count, global_cap;
    int            dry_run;      /* 1=pass1 (count only), 0=pass2 (emit) */
    int            mode64;       /* 1=AMD64 encoding (REX + rip-relative) */
    int            obj;          /* 1=-c object: own labels get relocated */
} X86Asm;

/* Set while xa_process_line runs, so the operand parsers can accept the
 * AMD64-only register names without threading X86Asm through every call. */
static int g_xa64 = 0;

/* Operand kinds */
enum { XOP_NONE, XOP_REG, XOP_IMM, XOP_MEM };

/* Longest label / symbol name this assembler can carry.  It used to be 32 for
 * the mnemonic buffer and 64 for operand symbols, and both truncations failed
 * silently: a 32-char label such as `ark_sl_ark_get_date_time_mon_name:` was
 * cut to 31 chars, the ':' after it was no longer at the cursor, so the line
 * fell through to the instruction decoder and reported
 * `unknown instruction "ark_sl_..."`.  Static locals are built as
 * ark_sl_<function>_<variable>, i.e. exactly the names that get long, and
 * port/host/platform_host.c and backend_arm.c both hit it.  The line reader
 * now also refuses a name that does not fit instead of silently continuing. */
#define XA_SYM_MAX 512

typedef struct {
    int  kind;
    int  reg;        /* register code 0-7 (XOP_REG) */
    int  reg_size;   /* 1=byte, 2=word, 4=dword (XOP_REG) */
    long long imm;   /* immediate value (XOP_IMM) */
    /* memory: [base + index*scale + disp] or [symbol] */
    int  base;       /* base register code, -1=none (XOP_MEM) */
    int  index;      /* index register code, -1=none (XOP_MEM) */
    int  scale;      /* 1,2,4,8 (XOP_MEM) */
    int  disp;       /* displacement (XOP_MEM) */
    int  has_sym;    /* 1 if [symbol] (XOP_MEM) */
    char sym[XA_SYM_MAX];    /* symbol name (XOP_MEM) */
    int  mem_size;   /* 0=auto, 1=byte, 2=word, 4=dword (from size prefix) */
} X86Operand;

/* --- character / token helpers --- */

static int xa_is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.';
}
static int xa_is_alnum(char c) {
    return xa_is_alpha(c) || (c >= '0' && c <= '9');
}
static int xa_is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static int xa_toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c; }
static int xa_strieq(const char *a, const char *b) {
    while (*a && *b) { if (xa_toupper(*a) != xa_toupper(*b)) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}
/* Case-insensitive prefix check: does *p start with prefix (followed by space
 * or '[')?  Needed because xa_strieq requires exact match but "dword ptr [...]"
 * has trailing content after "dword". */
static int xa_starts_with(const char *p, const char *prefix) {
    while (*prefix) {
        if (xa_toupper(*p) != xa_toupper(*prefix)) return 0;
        p++; prefix++;
    }
    return 1;
}
static void xa_skip_space(const char **p) { while (xa_is_space(**p)) (*p)++; }
static void xa_skip_to_eol(const char **p) { while (**p && **p != '\n') (*p)++; }

/* Read a word (mnemonic, directive, or label name). */
static int xa_read_word(const char **p, char *out, int cap) {
    int n = 0;
    xa_skip_space(p);
    while (**p && xa_is_alnum(**p) && n < cap - 1) { out[n++] = **p; (*p)++; }
    out[n] = 0;
    return n;
}

/* --- label / ref management --- */
static int  xa_is_declared_global(X86Asm *a, const char *name);
static void xa_declare_global(X86Asm *a, const char *name);
static void xa_add_label(X86Asm *a, const char *name, int addr) {
    int i;
    /* pass1 and pass2 walk the same text, so without a name check every label
     * is registered twice.  That doubled the object's symbol table, and -link
     * then reported "duplicate strong symbol <fn>, using first definition" for
     * every function in every file (and, on a two-object link, for symbols the
     * other object only *referenced*).  Update the address that pass2 computed
     * instead of appending. */
    for (i = 0; i < a->label_count; i++) {
        if (ark_streq(a->labels[i].name, name)) {
            a->labels[i].addr = addr;
            if (a->pending_weak) a->labels[i].is_weak = 1;
            a->pending_weak = 0;
            return;
        }
    }
    if (a->label_count >= a->label_cap) {
        int nc = a->label_cap == 0 ? 64 : a->label_cap * 2;
        a->labels = (X86AsmLabel *)mem_realloc(a->labels,
            a->label_cap * (int)sizeof(X86AsmLabel), nc * (int)sizeof(X86AsmLabel));
        a->label_cap = nc;
    }
    a->labels[a->label_count].name = mem_strdup(name);
    a->labels[a->label_count].addr = addr;
    a->labels[a->label_count].is_weak = a->pending_weak;
    a->labels[a->label_count].is_global = xa_is_declared_global(a, name);
    a->pending_weak = 0;
    a->label_count++;
}

/* Is this name one the assembly exported with .globl?  Exported symbols are
 * the only ones the linker may bind across objects; everything else (static
 * functions, string literals, function-scope statics) belongs to this object
 * alone. */
static int xa_is_declared_global(X86Asm *a, const char *name) {
    int i;
    for (i = 0; i < a->global_count; i++)
        if (ark_streq(a->globals[i], name)) return 1;
    return 0;
}

static void xa_declare_global(X86Asm *a, const char *name) {
    int i;
    if (!name || !name[0]) return;
    for (i = 0; i < a->global_count; i++)
        if (ark_streq(a->globals[i], name)) return;
    if (a->global_count >= a->global_cap) {
        int nc = a->global_cap == 0 ? 64 : a->global_cap * 2;
        /* mem_realloc takes (ptr, old_size, new_size) -- the old size is what
         * the bump allocator compares against to extend in place.  Calling it
         * with two arguments silently passes the new size as old_size, and the
         * crash landed in the *next* line of assembly: `.globl f` alone
         * segfaulted the i386 arm of the x86-64 gate. */
        a->globals = (char **)mem_realloc(a->globals,
            a->global_cap * (int)sizeof(char *), nc * (int)sizeof(char *));
        a->global_cap = nc;
    }
    a->globals[a->global_count++] = mem_strdup(name);
    /* The definition may already have gone by (".globl" after the label). */
    for (i = 0; i < a->label_count; i++)
        if (ark_streq(a->labels[i].name, name)) a->labels[i].is_global = 1;
}
static int xa_find_label(X86Asm *a, const char *name) {
    int i;
    for (i = 0; i < a->label_count; i++)
        if (ark_streq(a->labels[i].name, name)) return a->labels[i].addr;
    return -1;
}
static void xa_add_ref_a(X86Asm *a, int pc, const char *name, int type,
                         int addend) {
    if (a->ref_count >= a->ref_cap) {
        int nc = a->ref_cap == 0 ? 64 : a->ref_cap * 2;
        a->refs = (X86AsmRef *)mem_realloc(a->refs,
            a->ref_cap * (int)sizeof(X86AsmRef), nc * (int)sizeof(X86AsmRef));
        a->ref_cap = nc;
    }
    a->refs[a->ref_count].pc = pc;
    a->refs[a->ref_count].name = mem_strdup(name);
    a->refs[a->ref_count].type = type;
    a->refs[a->ref_count].addend = addend;
    a->refs[a->ref_count].width = 4;
    a->ref_count++;
}
static void xa_add_ref(X86Asm *a, int pc, const char *name, int type) {
    xa_add_ref_a(a, pc, name, type, 0);
}

/* --- byte emission (respects dry_run) --- */
static void xa_emit(X86Asm *a, unsigned char b) {
    if (!a->dry_run && a->pc < a->out_cap) a->out[a->pc] = b;
    a->pc++;
}
static void xa_emit16(X86Asm *a, int v) {
    xa_emit(a, (unsigned char)(v & 0xFF));
    xa_emit(a, (unsigned char)((v >> 8) & 0xFF));
}
static void xa_emit32(X86Asm *a, int v) {
    xa_emit(a, (unsigned char)(v & 0xFF));
    xa_emit(a, (unsigned char)((v >> 8) & 0xFF));
    xa_emit(a, (unsigned char)((v >> 16) & 0xFF));
    xa_emit(a, (unsigned char)((v >> 24) & 0xFF));
}
static void xa_emit64(X86Asm *a, long long v) {
    xa_emit32(a, (int)(v & 0xFFFFFFFFLL));
    xa_emit32(a, (int)((v >> 32) & 0xFFFFFFFFLL));
}

/* Shared bounded encoder supplies every x86 instruction field layout. */
static void xa_field_byte(X86Asm *a,const char *rule,int a0,int a1,int a2,int a3,int count){
    int args[4];unsigned byte;
    args[0]=a0;args[1]=a1;args[2]=a2;args[3]=a3;
    if(xh_encoding_encode(a->mode64?"x86_64":"x86",rule,args,count,&byte) || byte>255){a->error_count++;return;}
    xa_emit(a,(unsigned char)byte);
}
static void xa_rex(X86Asm *a,int w,int regf,int base,int index){
    int extended=xh_x86_number("encoding.rex.extended_start");
    if(!a->mode64)return;
    if(!w && regf<extended && base<extended && index<extended)return;
    xa_field_byte(a,"REX",w,regf<0?0:regf,base<0?0:base,index<0?0:index,4);
}
/* REX for an instruction whose rm operand is a memory operand (or a reg). */
static void xa_rex_rm(X86Asm *a, int w, int regf, X86Operand *rm) {
    int base = -1, index = -1;
    if (rm->kind == XOP_MEM) { base = rm->base; index = rm->index; }
    else if (rm->kind == XOP_REG) { base = rm->reg; }
    xa_rex(a, w, regf, base, index);
}
/* Operand width in bytes (0 = unknown). */
static int xa_opsize(X86Operand *op) {
    if (op->kind == XOP_REG) return op->reg_size;
    if (op->kind == XOP_MEM) return op->mem_size;
    return 0;
}
/* 1 if either operand is a 64-bit GPR / 8-byte memory operand. */
static int xa_is64(X86Operand *x, X86Operand *y) {
    return xa_opsize(x) == 8 || xa_opsize(y) == 8;
}

/* --- register parsing ---
 * Returns register code (0-7), sets *size (1=byte,2=word,4=dword), or -1.
 * 32-bit: eax=0,ecx=1,edx=2,ebx=3,esp=4,ebp=5,esi=6,edi=7
 * 16-bit: ax=0,cx=1,dx=2,bx=3,sp=4,bp=5,si=6,di=7
 * 8-bit:  al=0,cl=1,dl=2,bl=3,ah=4,ch=5,dh=6,bh=7 */
static int xa_parse_reg(const char **p, int *size) {
    char w[8];
    int n = 0;
    const char *s = *p;
    xa_skip_space(p);
    s = *p;
    while (xa_is_alnum(**p) && n < (int)sizeof(w) - 1) { w[n++] = **p; (*p)++; }
    w[n] = 0;
    if (n == 0) return -1;
    { int code=xh_x86_register(w,size); if(code>=0)return code; }
    /* Not a register - restore position */
    *p = s;
    return -1;
}

/* --- immediate parsing ---
 * Handles decimal, hex (0x), and leading '-' / '+'. */
static int xa_parse_imm(const char **p, long long *out) {
    long long v = 0;
    int neg = 0;
    xa_skip_space(p);
    if (**p == '+') (*p)++;
    if (**p == '-') { neg = 1; (*p)++; }
    if (**p == '0' && ((*p)[1] == 'x' || (*p)[1] == 'X')) {
        (*p) += 2;
        if (!( (**p >= '0' && **p <= '9') ||
               (**p >= 'a' && **p <= 'f') ||
               (**p >= 'A' && **p <= 'F') )) return 0;
        while ((**p >= '0' && **p <= '9') ||
               (**p >= 'a' && **p <= 'f') ||
               (**p >= 'A' && **p <= 'F')) {
            char c = **p;
            int dv = (c >= '0' && c <= '9') ? c - '0' :
                     (c >= 'a' && c <= 'f') ? c - 'a' + 10 : c - 'A' + 10;
            v = v * 16 + dv;
            (*p)++;
        }
    } else if (**p >= '0' && **p <= '9') {
        while (**p >= '0' && **p <= '9') { v = v * 10 + (**p - '0'); (*p)++; }
    } else {
        return 0;
    }
    /* skip suffixes (u, l, L) */
    while (**p == 'u' || **p == 'U' || **p == 'l' || **p == 'L') (*p)++;
    *out = neg ? -v : v;
    return 1;
}

/* Parse a register-or-symbol name (for labels in jmp/call). */
static int xa_read_ident(const char **p, char *out, int cap) {
    int n = 0;
    xa_skip_space(p);
    if (**p == '.') {
        while (xa_is_alnum(**p) && n < cap - 1) { out[n++] = **p; (*p)++; }
    } else if (xa_is_alpha(**p)) {
        while (xa_is_alnum(**p) && n < cap - 1) { out[n++] = **p; (*p)++; }
    }
    out[n] = 0;
    return n;
}

/* --- memory operand parsing ---
 * Parses [base+index*scale+disp] or [symbol]. Handles optional
 * size prefix (dword ptr / byte ptr / word ptr) before the bracket. */
static int xa_parse_mem(const char **p, X86Operand *op) {
    xa_skip_space(p);
    op->mem_size = 0;
    /* Check for size prefix: "dword ptr" / "byte ptr" / "word ptr" */
    {
        const char *save = *p;
        if (xa_starts_with(*p, "dword")) {
            *p += 5; xa_skip_space(p);
            if (xa_starts_with(*p, "ptr")) { *p += 3; xa_skip_space(p); op->mem_size = 4; }
            else { *p = save; }
        } else if (xa_starts_with(*p, "byte")) {
            *p += 4; xa_skip_space(p);
            if (xa_starts_with(*p, "ptr")) { *p += 3; xa_skip_space(p); op->mem_size = 1; }
            else { *p = save; }
        } else if (xa_starts_with(*p, "word")) {
            *p += 4; xa_skip_space(p);
            if (xa_starts_with(*p, "ptr")) { *p += 3; xa_skip_space(p); op->mem_size = 2; }
            else { *p = save; }
        } else if (g_xa64 && xa_starts_with(*p, "qword")) {
            *p += 5; xa_skip_space(p);
            if (xa_starts_with(*p, "ptr")) { *p += 3; xa_skip_space(p); op->mem_size = 8; }
            else { *p = save; }
        } else if (g_xa64 && xa_starts_with(*p, "xmmword")) {
            *p += 7; xa_skip_space(p);
            if (xa_starts_with(*p, "ptr")) { *p += 3; xa_skip_space(p); op->mem_size = 16; }
            else { *p = save; }
        }
    }
    if (**p != '[') return 0;
    (*p)++;
    op->kind = XOP_MEM;
    op->base = -1;
    op->index = -1;
    op->scale = 1;
    op->disp = 0;
    op->has_sym = 0;
    op->sym[0] = 0;

    /* Parse interior: sequence of terms separated by + or - */
    {
        int sign = 1;  /* +1 for '+' prefix, -1 for '-' */
        int got_term = 0;
        for (;;) {
            xa_skip_space(p);
            if (**p == ']') { (*p)++; break; }
            if (**p == '+') { sign = 1; (*p)++; xa_skip_space(p); continue; }
            if (**p == '-') { sign = -1; (*p)++; xa_skip_space(p); continue; }
            /* Try register */
            {
                int rsize;
                const char *save = *p;
                int rc = xa_parse_reg(p, &rsize);
                if (rc >= 0) {
                    /* Check for *scale */
                    xa_skip_space(p);
                    if (**p == '*') {
                        (*p)++;
                        xa_skip_space(p);
                        {
                            long long sc;
                            if (!xa_parse_imm(p, &sc)) sc = 1;
                            op->index = rc;
                            op->scale = (int)sc;
                        }
                    } else {
                        if (op->base < 0 && sign > 0) op->base = rc;
                        else if (op->index < 0) { op->index = rc; op->scale = 1; }
                        else { *p = save; break; } /* too many regs */
                    }
                    sign = 1;
                    got_term = 1;
                    continue;
                }
            }
            /* Try immediate (displacement) */
            {
                long long v;
                const char *save = *p;
                if (xa_parse_imm(p, &v)) {
                    op->disp += (int)(sign * v);
                    sign = 1;
                    got_term = 1;
                    continue;
                }
                /* Try symbol name */
                {
                    char name[XA_SYM_MAX];
                    const char *sv2 = *p;
                    if (xa_read_ident(p, name, (int)sizeof(name)) && name[0]) {
                        /* If this is the first term and no base/index, it's [symbol] */
                        if (!got_term && op->base < 0 && op->index < 0) {
                            /* Check if next char is ']' or '+' or '-' */
                            xa_skip_space(p);
                            if (**p == ']' || **p == '+' || **p == '-') {
                                op->has_sym = 1;
                                /* copy symbol name */
                                {
                                    int k;
                                    for (k = 0; name[k] && k < (int)sizeof(op->sym) - 1; k++)
                                        op->sym[k] = name[k];
                                    op->sym[k] = 0;
                                }
                                sign = 1;
                                got_term = 1;
                                continue;
                            }
                        }
                        /* symbol + disp: treat symbol as disp32 reference */
                        op->has_sym = 1;
                        {
                            int k;
                            for (k = 0; name[k] && k < (int)sizeof(op->sym) - 1; k++)
                                op->sym[k] = name[k];
                            op->sym[k] = 0;
                        }
                        sign = 1;
                        got_term = 1;
                        continue;
                    }
                    *p = sv2;
                }
                *p = save;
            }
            break;  /* unrecognized token */
        }
    }
    return 1;
}

/* Parse one operand (register, immediate, or memory). */
static int xa_parse_operand(const char **p, X86Operand *op) {
    xa_skip_space(p);
    if (**p == 0 || **p == '\n' || **p == ',') return 0;
    /* Try memory operand (with optional size prefix) */
    if (**p == '[' || xa_starts_with(*p, "dword") || xa_starts_with(*p, "byte") ||
        xa_starts_with(*p, "word") || (g_xa64 && xa_starts_with(*p, "qword")) ||
        (g_xa64 && xa_starts_with(*p, "xmmword"))) {
        return xa_parse_mem(p, op);
    }
    /* Try register */
    {
        int rsize;
        const char *save = *p;
        int rc = xa_parse_reg(p, &rsize);
        if (rc >= 0) {
            op->kind = XOP_REG;
            op->reg = rc;
            op->reg_size = rsize;
            return 1;
        }
        *p = save;
    }
    /* Try immediate */
    {
        long long v;
        if (xa_parse_imm(p, &v)) {
            op->kind = XOP_IMM;
            op->imm = v;
            return 1;
        }
    }
    /* Try label/symbol (for jmp/call targets) */
    {
        char name[XA_SYM_MAX];
        if (xa_read_ident(p, name, (int)sizeof(name)) && name[0]) {
            op->kind = XOP_MEM;
            op->base = -1;
            op->index = -1;
            op->scale = 1;
            op->disp = 0;
            op->has_sym = 1;
            {
                int k;
                for (k = 0; name[k] && k < (int)sizeof(op->sym) - 1; k++)
                    op->sym[k] = name[k];
                op->sym[k] = 0;
            }
            op->mem_size = 0;
            return 1;
        }
    }
    return 0;
}

/* --- ModR/M encoding --- */
/* Emit ModR/M for reg-reg (mod=11). */
static void xa_modrm_reg(X86Asm *a, int reg, int rm) {
    xa_field_byte(a,"MODRM",xh_x86_number("encoding.mod.register"),reg,rm,0,3);
}

/* Emit ModR/M + optional SIB + displacement for a memory operand.
 * reg_field is the /r field (register or opcode extension). */
static void xa_modrm_mem(X86Asm *a, int reg_field, X86Operand *mem) {
    int base = mem->base;
    int index = mem->index;
    int disp = mem->disp;
    /* rsp/r12 as base, or rbp/r13 as base with mod==0, need the (base&7) test. */
    int needs_sib = (base >= 0 && (base & xh_x86_number("encoding.address.index_mask")) == xh_x86_number("encoding.address.sib_rm")) || (index >= 0);

    /* [symbol] with no base/index: mod=00, rm=101, disp32 = symbol addr.
     * In 32-bit mode that encoding is an absolute address, so `+disp` is a
     * plain addend on the same relocation; in 64-bit mode it is RIP-relative
     * and the addend has to move along with the displacement.  Both used to
     * be refused outright, which made every global 64-bit object unusable on
     * i386: `double g;` reads the high word as `mov eax, [g+4]`. */
    if (mem->has_sym && base < 0 && index < 0) {
        int rtype;
        if (!a->mode64) {
            rtype = (disp != 0) ? X86_RELOC_ABS32_ADDEND : 4;
        } else {
            rtype = (disp != 0) ? X86_RELOC_PC32_ADDEND : 5;
        }
        /* In 64-bit mode mod=00 rm=101 means RIP-relative: the disp32 field is
         * patched by the same reloc type 5 (target-(pc+4)) the linker already
         * implements, which is exactly the RIP-relative semantic.  The field
         * starts out holding the addend: resolve_refs folds it in when the
         * label is local (byte-identical for addend 0), and types 8/9 tell
         * the linker to add it for a cross-object symbol. */
        xa_field_byte(a,"MODRM",xh_x86_number("encoding.mod.indirect"),reg_field,xh_x86_number("encoding.address.no_base"),0,3);
        xa_add_ref_a(a, a->pc, mem->sym, rtype, disp);
        xa_emit32(a, disp);
        return;
    }
    /* [symbol + base]: symbol contributes to disp32 (absolute reloc). */
    if (mem->has_sym && base >= 0) {
        xa_field_byte(a,"MODRM",xh_x86_number("encoding.mod.disp32"),reg_field,needs_sib?xh_x86_number("encoding.address.sib_rm"):base,0,3);
        if (needs_sib) {
            int sb = (base >= 0) ? base : xh_x86_number("encoding.address.no_base");
            int si = (index >= 0) ? index : xh_x86_number("encoding.address.no_index");
            int sc = (mem->scale == xh_x86_number("encoding.scale.two")) ? xh_x86_number("encoding.scale.code_two") : (mem->scale == xh_x86_number("encoding.scale.four")) ? xh_x86_number("encoding.scale.code_four") : (mem->scale == xh_x86_number("encoding.scale.eight")) ? xh_x86_number("encoding.scale.code_eight") : xh_x86_number("encoding.scale.code_one");
            xa_field_byte(a,"SIB",sc,si,sb,0,3);
        }
        xa_add_ref_a(a, a->pc, mem->sym, 4, disp);
        xa_emit32(a, disp);
        return;
    }

    /* No base means a mandatory disp32. In AMD64 a plain rm=5 would
     * mean RIP-relative, so an absolute numeric address uses a no-base SIB.
     * The same SIB form handles an index-only address in both modes. */
    if(base<0){
        int sc=(mem->scale==xh_x86_number("encoding.scale.two"))?xh_x86_number("encoding.scale.code_two"):
               (mem->scale==xh_x86_number("encoding.scale.four"))?xh_x86_number("encoding.scale.code_four"):
               (mem->scale==xh_x86_number("encoding.scale.eight"))?xh_x86_number("encoding.scale.code_eight"):xh_x86_number("encoding.scale.code_one");
        xa_field_byte(a,"MODRM",xh_x86_number("encoding.mod.indirect"),reg_field,xh_x86_number("encoding.address.sib_rm"),0,3);
        xa_field_byte(a,"SIB",sc,index<0?xh_x86_number("encoding.address.no_index"):index,xh_x86_number("encoding.address.no_base"),0,3);
        if(mem->has_sym)xa_add_ref(a,a->pc,mem->sym,4);
        xa_emit32(a,disp);
        return;
    }
    /* Determine mod field */
    {
        int mod;
        if (disp == 0 && !(base >= 0 && (base & xh_x86_number("encoding.address.index_mask")) == xh_x86_number("encoding.address.no_base"))) {
            mod = xh_x86_number("encoding.mod.indirect");
        } else if (disp >= xh_x86_number("encoding.displacement.short_min") && disp <= xh_x86_number("encoding.displacement.short_max")) {
            mod = xh_x86_number("encoding.mod.disp8");
        } else {
            mod = xh_x86_number("encoding.mod.disp32");
        }
        /* ebp/r13 as base needs at least mod=01 (mod=00 rm=101 = [disp32]) */
        if (base >= 0 && (base & xh_x86_number("encoding.address.index_mask")) == xh_x86_number("encoding.address.no_base") && mod == xh_x86_number("encoding.mod.indirect")) mod = xh_x86_number("encoding.mod.disp8");

        if (needs_sib) {
            int sb = (base >= 0) ? base : xh_x86_number("encoding.address.no_base");  /* 5 in mod=00 means no base (disp32) */
            int si = (index >= 0) ? index : xh_x86_number("encoding.address.no_index");  /* 4 = no index */
            int sc = (mem->scale == xh_x86_number("encoding.scale.two")) ? xh_x86_number("encoding.scale.code_two") : (mem->scale == xh_x86_number("encoding.scale.four")) ? xh_x86_number("encoding.scale.code_four") : (mem->scale == xh_x86_number("encoding.scale.eight")) ? xh_x86_number("encoding.scale.code_eight") : xh_x86_number("encoding.scale.code_one");
            xa_field_byte(a,"MODRM",mod,reg_field,xh_x86_number("encoding.address.sib_rm"),0,3);
            xa_field_byte(a,"SIB",sc,si,sb,0,3);
        } else {
            xa_field_byte(a,"MODRM",mod,reg_field,base,0,3);
        }
        if (mod == xh_x86_number("encoding.mod.disp8")) {
            xa_emit(a, (unsigned char)(disp & 0xFF));
        } else if (mod == xh_x86_number("encoding.mod.disp32")) {
            xa_emit32(a, disp);
        }
    }
}

/* --- ALU instruction emitter (add/sub/cmp/and/or/xor/adc/sbb) ---
 * op_rm_r:  r/m, r form opcode (e.g. 0x01 for add)
 * op_r_rm:  r, r/m form opcode (e.g. 0x03 for add)
 * op_eax:   eax, imm32 special opcode (e.g. 0x05 for add)
 * sub_op:   /n extension (0=add, 1=or, 4=and, 5=sub, 6=xor, 7=cmp) */
static int xa_imm_fits32(long long v) {
    return v >= -2147483648LL && v <= 2147483647LL;
}

/* 0x81/0x83 sign-extend their imm32 only when the operand is 64-bit.  With a
 * 32-bit operand the field is a raw bit pattern, so 0x80000000..0xFFFFFFFF is
 * perfectly encodable -- and is what the front end actually emits to flip a
 * float sign bit (`xor eax, 0x80000000`).  Treating that as "out of range"
 * made src/lexer.c and src/sema.c impossible to assemble at all, even though
 * every one of those instructions was valid. */
static int xa_alu_imm_fits(long long v, int w) {
    if (w) return xa_imm_fits32(v);
    return v >= -2147483648LL && v <= 4294967295LL;
}

static void xa_alu(X86Asm *a, int op_rm_r, int op_r_rm, int op_eax, int sub_op,
                   X86Operand *dst, X86Operand *src) {
    int w = xa_is64(dst, src) ? 1 : 0;
    if (dst->kind == XOP_REG && src->kind == XOP_REG) {
        xa_rex(a, w, src->reg, dst->reg, -1);
        xa_emit(a, (unsigned char)op_rm_r);
        xa_modrm_reg(a, src->reg, dst->reg);
    } else if (dst->kind == XOP_REG && src->kind == XOP_IMM) {
        long long imm = src->imm;
        if (!xa_alu_imm_fits(imm, w)) {
            /* 0x81 takes an imm32 that is sign-extended when the operand is
             * 64-bit; silently truncating a wider constant would produce a
             * wrong answer, so refuse.  A 32-bit operand has no such hazard
             * (the field is a bit pattern), see xa_alu_imm_fits. */
            a->error_count++;
            diag_tool(DIAG_SEV_ERROR, "arkcc", "immediate out of 32-bit range for alu op", 0,0,0,0,0,0);
            return;
        }
        if (imm >= -128 && imm <= 127) {
            xa_rex(a, w, sub_op, dst->reg, -1);
            xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.83"));
            xa_modrm_reg(a, sub_op, dst->reg);
            xa_emit(a, (unsigned char)(imm & 0xFF));
        } else if ((dst->reg & 7) == 0) {  /* eax/rax */
            xa_rex(a, w, 0, dst->reg, -1);
            xa_emit(a, (unsigned char)op_eax);
            xa_emit32(a, (int)imm);
        } else {
            xa_rex(a, w, sub_op, dst->reg, -1);
            xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.81"));
            xa_modrm_reg(a, sub_op, dst->reg);
            xa_emit32(a, (int)imm);
        }
    } else if (dst->kind == XOP_REG && src->kind == XOP_MEM) {
        xa_rex_rm(a, w, dst->reg, src);
        xa_emit(a, (unsigned char)op_r_rm);
        xa_modrm_mem(a, dst->reg, src);
    } else if (dst->kind == XOP_MEM && src->kind == XOP_REG) {
        xa_rex_rm(a, w, src->reg, dst);
        xa_emit(a, (unsigned char)op_rm_r);
        xa_modrm_mem(a, src->reg, dst);
    } else if (dst->kind == XOP_MEM && src->kind == XOP_IMM) {
        long long imm = src->imm;
        if (!xa_alu_imm_fits(imm, w)) {
            a->error_count++;
            diag_tool(DIAG_SEV_ERROR, "arkcc", "immediate out of 32-bit range for alu op", 0,0,0,0,0,0);
            return;
        }
        if (imm >= -128 && imm <= 127) {
            xa_rex_rm(a, w, sub_op, dst);
            xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.83"));
            xa_modrm_mem(a, sub_op, dst);
            xa_emit(a, (unsigned char)(imm & 0xFF));
        } else {
            xa_rex_rm(a, w, 0, dst);
            xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.c7"));
            xa_modrm_mem(a, 0, dst);
            xa_emit32(a, (int)imm);
        }
    } else {
        a->error_count++;
    }
}

/* Emit a branch (jmp/call/jcc) with a label operand.
 * opcode2 is 0 for single-byte opcodes (E8/E9), or the second byte for 0F-prefixed jcc. */
static void xa_branch(X86Asm *a, unsigned char op1, unsigned char op2, const char *label) {
    int disp_pc;
    if (op2) {
        xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.0f"));
        xa_emit(a, op2);
    } else {
        xa_emit(a, op1);
    }
    disp_pc = a->pc;
    xa_add_ref(a, disp_pc, label, 5);  /* type 5: PC-relative disp32 */
    xa_emit32(a, 0);  /* placeholder */
}

/* Parse a string literal ".asciz" / ".ascii" content and emit bytes. */
static void xa_emit_string(X86Asm *a, const char **p, int with_nul) {
    xa_skip_space(p);
    if (**p != '"') return;
    (*p)++;
    while (**p && **p != '"' && **p != '\n') {
        if (**p == '\\') {
            (*p)++;
            switch (**p) {
                case 'n':  xa_emit(a, '\n'); (*p)++; break;
                case 't':  xa_emit(a, '\t'); (*p)++; break;
                case 'r':  xa_emit(a, '\r'); (*p)++; break;
                case '0':  xa_emit(a, 0);    (*p)++; break;
                case '\\': xa_emit(a, '\\'); (*p)++; break;
                case '"':  xa_emit(a, '"');  (*p)++; break;
                case 'x': {
                    int v = 0; (*p)++;
                    while (**p) {
                        int dv;
                        if (**p >= '0' && **p <= '9') dv = **p - '0';
                        else if (**p >= 'a' && **p <= 'f') dv = **p - 'a' + 10;
                        else if (**p >= 'A' && **p <= 'F') dv = **p - 'A' + 10;
                        else break;
                        v = v * 16 + dv; (*p)++;
                    }
                    xa_emit(a, (unsigned char)v);
                    break;
                }
                default: xa_emit(a, (unsigned char)**p); (*p)++; break;
            }
        } else {
            xa_emit(a, (unsigned char)**p);
            (*p)++;
        }
    }
    if (**p == '"') (*p)++;
    if (with_nul) xa_emit(a, 0);
}

/* Emit a .byte/.word/.long/.quad value (numeric or symbol reference). */
static void xa_emit_data(X86Asm *a, const char **p, int size) {
    xa_skip_space(p);
    {
        long long v;
        const char *save = *p;
        if (xa_parse_imm(p, &v)) {
            if (size == 1) xa_emit(a, (unsigned char)(v & 0xFF));
            else if (size == 2) xa_emit16(a, (int)v);
            else if (size == 4) xa_emit32(a, (int)v);
            /* .quad must carry the high half too: writing zero there turned
             * every 64-bit initialiser whose value did not fit in 32 bits
             * into its low word plus a hole, so "int g[4] = {1,2,3,4}" -- which
             * the aggregate emitter packs two ints per .quad -- linked as
             * {1,0,3,0} and every 64-bit global constant read half-zero. */
            else { xa_emit32(a, (int)(v & 0xFFFFFFFFLL));
                   xa_emit32(a, (int)((v >> 32) & 0xFFFFFFFFLL)); }
        } else {
            /* Symbol reference (for .long symbol etc.) */
            char name[XA_SYM_MAX];
            *p = save;
            if (xa_read_ident(p, name, (int)sizeof(name)) && name[0]) {
                int add=0;xa_skip_space(p);if(**p=='+'||**p=='-'){int neg=**p=='-';long long z;(*p)++;if(xa_parse_imm(p,&z))add=(int)(neg?-z:z);else a->error_count++;}
                if (size == 4) {
                    xa_add_ref_a(a, a->pc, name, 4, add);
                    xa_emit32(a, 0);
                } else if (size == 8) {
                    xa_add_ref_a(a, a->pc, name, 4, add);
                    a->refs[a->ref_count-1].width = 8;
                    xa_emit32(a, 0);
                    xa_emit32(a, 0);
                } else {
                    xa_emit(a, 0);
                }
            }
        }
    }
}

/* ============================ AMD64 emission helpers ======================
 * These exist so the instruction dispatch chain below stays readable while
 * still handling both the i386 (mode64==0) and the AMD64 (mode64==1) forms.
 * Every REX emission is a no-op in 32-bit mode, so the i386 output stays
 * byte-for-byte identical to what this assembler produced before. */

/* movq: xmm <-> gpr64, xmm <-> m64, xmm <-> xmm. */
static void xa_emit_movq(X86Asm *a, X86Operand *d, X86Operand *s) {
    int d_xmm = (d->kind == XOP_REG && d->reg_size == 16);
    int s_xmm = (s->kind == XOP_REG && s->reg_size == 16);
    if (d_xmm && s_xmm) {
        /* F3 0F 7E /r : movq xmm, xmm */
        xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.f3"));
        xa_rex(a, 0, d->reg, s->reg, -1);
        xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.0f")); xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.7e"));
        xa_modrm_reg(a, d->reg, s->reg);
    } else if (d_xmm && s->kind == XOP_REG) {
        /* 66 REX.W 0F 6E /r : movq xmm, r64 */
        xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.66"));
        xa_rex(a, 1, d->reg, s->reg, -1);
        xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.0f")); xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.6e"));
        xa_modrm_reg(a, d->reg, s->reg);
    } else if (d_xmm && s->kind == XOP_MEM) {
        /* F3 0F 7E /r : movq xmm, m64 */
        xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.f3"));
        xa_rex_rm(a, 0, d->reg, s);
        xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.0f")); xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.7e"));
        xa_modrm_mem(a, d->reg, s);
    } else if (d->kind == XOP_REG && s_xmm) {
        /* 66 REX.W 0F 7E /r : movq r64, xmm */
        xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.66"));
        xa_rex(a, 1, s->reg, d->reg, -1);
        xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.0f")); xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.7e"));
        xa_modrm_reg(a, s->reg, d->reg);
    } else if (d->kind == XOP_MEM && s_xmm) {
        /* 66 0F D6 /r : movq m64, xmm */
        xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.66"));
        xa_rex_rm(a, 0, s->reg, d);
        xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.0f")); xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.d6"));
        xa_modrm_mem(a, s->reg, d);
    } else {
        a->error_count++;
        diag_tool(DIAG_SEV_ERROR, "arkcc", "movq needs an xmm operand", 0,0,0,0,0,0);
    }
}

/* SSE/SSE2 two-operand instruction: legacy prefix + optional REX + 0F op2. */
static void xa_sse(X86Asm *a, int pfx, int op2, X86Operand *d, X86Operand *s) {
    int w = xa_is64(d, s) ? 1 : 0;
    int regf = (d->kind == XOP_REG) ? d->reg : 0;
    if (pfx) xa_emit(a, (unsigned char)pfx);
    xa_rex_rm(a, w, regf, s);
    xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.0f"));
    xa_emit(a, (unsigned char)op2);
    if (s->kind == XOP_REG) xa_modrm_reg(a, regf, s->reg);
    else xa_modrm_mem(a, regf, s);
}

/* Shift / rotate group 2: ext=4 shl, 5 shr, 7 sar. */
static void xa_shift(X86Asm *a, int ext, X86Operand *d, X86Operand *s) {
    int w = (xa_opsize(d) == 8) ? 1 : 0;
    int by_cl = (s->kind == XOP_REG && s->reg_size == 1 && (s->reg & 7) == 1);
    if (by_cl) {
        xa_rex_rm(a, w, ext, d);
        xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.d3"));
    } else if (s->kind == XOP_IMM) {
        xa_rex_rm(a, w, ext, d);
        xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.c1"));
    } else {
        a->error_count++;
        diag_tool(DIAG_SEV_ERROR, "arkcc", "shift needs cl or an immediate", 0,0,0,0,0,0);
        return;
    }
    if (d->kind == XOP_REG) xa_modrm_reg(a, ext, d->reg);
    else xa_modrm_mem(a, ext, d);
    if (!by_cl) xa_emit(a, (unsigned char)(s->imm & 0xFF));
}

/* Double-precision shifts: shld (0F A4 /r ib, 0F A5 /r) and shrd
 * (0F AC /r ib, 0F AD /r).  Three operands: destination, source, count. */
static void xa_shxd(X86Asm *a, int op_imm, int op_cl, X86Operand *d,
                    X86Operand *s, X86Operand *c) {
    int w = (xa_opsize(d) == 8) ? 1 : 0;
    int by_cl = (c->kind == XOP_REG && c->reg_size == 1 && (c->reg & 7) == 1);
    if (!by_cl && c->kind != XOP_IMM) {
        a->error_count++;
        diag_tool(DIAG_SEV_ERROR, "arkcc", "shld/shrd needs cl or an immediate", 0,0,0,0,0,0);
        return;
    }
    xa_rex_rm(a, w, s->reg, d);
    xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.0f"));
    xa_emit(a, (unsigned char)(by_cl ? op_cl : op_imm));
    if (d->kind == XOP_REG) xa_modrm_reg(a, s->reg, d->reg);
    else xa_modrm_mem(a, s->reg, d);
    if (!by_cl) xa_emit(a, (unsigned char)(c->imm & 0xFF));
}

/* Group 3 unary: ext=2 not, 3 neg, 4 mul, 6 div, 7 idiv (F7 /ext). */
static void xa_grp3(X86Asm *a, int ext, X86Operand *d) {
    int w = (xa_opsize(d) == 8) ? 1 : 0;
    xa_rex_rm(a, w, ext, d);
    xa_emit(a, (unsigned char)((xa_opsize(d) == 1) ? 0xF6 : 0xF7));
    if (d->kind == XOP_REG) xa_modrm_reg(a, ext, d->reg);
    else xa_modrm_mem(a, ext, d);
}

/* setcc: accepts an explicit 8-bit destination (defaults to al). */
static void xa_setcc(X86Asm *a, int op2, X86Operand *d) {
    int r = (d->kind == XOP_REG) ? d->reg : 0;
    xa_rex(a, 0, 0, r, -1);
    xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.0f"));
    xa_emit(a, (unsigned char)op2);
    xa_modrm_reg(a, 0, r);
}

/* mov with full 64-bit support; xmm operands are routed to xa_emit_movq. */
static void xa_emit_mov(X86Asm *a, X86Operand *d, X86Operand *s) {
    int sz = xa_opsize(d) ? xa_opsize(d) : xa_opsize(s);
    int w = (sz == 8) ? 1 : 0;
    if ((d->kind == XOP_REG && d->reg_size == 16) ||
        (s->kind == XOP_REG && s->reg_size == 16)) {
        xa_emit_movq(a, d, s);
        return;
    }
    if (d->kind == XOP_REG && s->kind == XOP_REG) {
        xa_rex(a, w, s->reg, d->reg, -1);
        xa_emit(a, (unsigned char)((sz == 1) ? 0x88 : 0x89));
        xa_modrm_reg(a, s->reg, d->reg);
    } else if (d->kind == XOP_REG && s->kind == XOP_IMM) {
        long long imm = s->imm;
        if (sz == 1) {
            xa_rex(a, 0, 0, d->reg, -1);
            xa_field_byte(a,"REGOP",xh_x86_number("encoding.register_opcode.b0"),d->reg,0,0,2);
            xa_emit(a, (unsigned char)(imm & 0xFF));
        } else if (sz == 2) {
            xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.66"));
            xa_rex(a, 0, 0, d->reg, -1);
            xa_field_byte(a,"REGOP",xh_x86_number("encoding.register_opcode.b8"),d->reg,0,0,2);
            xa_emit16(a, (int)(imm & 0xFFFF));
        } else if (w) {
            if (xa_imm_fits32(imm)) {
                xa_rex(a, 1, 0, d->reg, -1);
                xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.c7"));
                xa_modrm_reg(a, 0, d->reg);
                xa_emit32(a, (int)imm);
            } else {
                xa_rex(a, 1, 0, d->reg, -1);
                xa_field_byte(a,"REGOP",xh_x86_number("encoding.register_opcode.b8"),d->reg,0,0,2);
                xa_emit64(a, imm);
            }
        } else {
            if (!xa_imm_fits32(imm)) {
                a->error_count++;
                diag_tool(DIAG_SEV_ERROR, "arkcc", "mov reg, imm32 out of range", 0,0,0,0,0,0);
                return;
            }
            xa_field_byte(a,"REGOP",xh_x86_number("encoding.register_opcode.b8"),d->reg,0,0,2);
            xa_emit32(a, (int)imm);
        }
    } else if (d->kind == XOP_REG && s->kind == XOP_MEM) {
        int byte_load = (sz == 1 && a->mode64);
        xa_rex_rm(a, w, d->reg, s);
        xa_emit(a, (unsigned char)(byte_load ? 0x8A : 0x8B));
        xa_modrm_mem(a, d->reg, s);
    } else if (d->kind == XOP_MEM && s->kind == XOP_REG) {
        xa_rex_rm(a, w, s->reg, d);
        xa_emit(a, (unsigned char)((sz == 1) ? 0x88 : 0x89));
        xa_modrm_mem(a, s->reg, d);
    } else if (d->kind == XOP_MEM && s->kind == XOP_IMM) {
        long long imm = s->imm;
        int msz = d->mem_size;
        if (!xa_imm_fits32(imm)) {
            a->error_count++;
            diag_tool(DIAG_SEV_ERROR, "arkcc", "mov [mem], wide immediate needs a register", 0,0,0,0,0,0);
            return;
        }
        if (msz == 1) {
            xa_rex_rm(a, 0, 0, d);
            xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.c6"));
            xa_modrm_mem(a, 0, d);
            xa_emit(a, (unsigned char)(imm & 0xFF));
        } else if (msz == 2) {
            xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.66"));
            xa_rex_rm(a, 0, 0, d);
            xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.c7"));
            xa_modrm_mem(a, 0, d);
            xa_emit16(a, (int)(imm & 0xFFFF));
        } else {
            xa_rex_rm(a, (msz == 8) ? 1 : 0, 0, d);
            xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.c7"));
            xa_modrm_mem(a, 0, d);
            xa_emit32(a, (int)imm);
        }
    } else {
        a->error_count++;
        diag_tool(DIAG_SEV_ERROR, "arkcc", "unsupported mov operand combination", 0,0,0,0,0,0);
    }
}

/* --- process one line of assembly ---
 * Shared by pass1 (dry_run=1) and pass2 (dry_run=0). */
static void xa_process_line(X86Asm *a, const char *line) {
    const char *p = line;
    g_xa64 = a->mode64;
    char mn[XA_SYM_MAX];
    /* Three operands, not two: shld/shrd take dest, source, count.  A
     * directive re-parses from opstart, so the extra slot is harmless. */
    X86Operand ops[3];
    int nop = 0;

    xa_skip_space(&p);
    if (*p == '\n' || *p == 0 || *p == '#') return;

    /* Read mnemonic or directive */
    if (xa_read_word(&p, mn, (int)sizeof(mn)) == 0) return;
    if (xa_is_alnum(*p)) {
        /* The word did not fit in mn: report it instead of quietly splitting a
         * long label into (truncated name) + (garbage instruction). */
        a->error_count++;
        diag_tool(DIAG_SEV_ERROR, "arkcc",
                  "assembly label or instruction name is longer than the %s-character limit",
                  diag_num((char *)0, XA_SYM_MAX - 1), 0,0,0,0,0);
        return;
    }
    xa_skip_space(&p);

    /* Label definition: "name:" */
    if (*p == ':') {
        xa_add_label(a, mn, a->pc);
        p++;
        xa_skip_space(&p);
        if (*p == '\n' || *p == 0 || *p == '#') return;
        /* Read the actual mnemonic after the label */
        if (xa_read_word(&p, mn, (int)sizeof(mn)) == 0) return;
        xa_skip_space(&p);
    }

    /* Where the operand text starts.  "rep movsb" is two mnemonics on one
     * line, and the generic scan below eats the second one as if it were an
     * operand, so the rep branch rewinds to here to read it. */
    const char *opstart = p;

    /* Parse operands (up to 2, comma-separated) */
    {
        int i;
        for (i = 0; i < 3; i++) {
            ops[i].kind = XOP_NONE;
            if (xa_parse_operand(&p, &ops[i])) {
                nop++;
                xa_skip_space(&p);
                if (*p == ',') { p++; continue; }
                break;
            }
            break;
        }
    }

    /* --- Directives --- */
    /* The operand pre-parse above has already consumed the line, so a
     * directive that reads its own operands (.long / .quad / .byte / .word /
     * .zero / .asciz ...) used to parse from the END of the directive and
     * emit nothing at all: 'int g = 7;' got a .data label at the right place
     * with zero bytes behind it, so every initialised global in an x86 / x86_64
     * image read as 0 and every later symbol collapsed onto one offset.
     * Rewind to where the operand text starts. */
    p = opstart;
    if (mn[0] == '.') {
        if (xa_strieq(mn, ".intel_syntax")) { xa_skip_to_eol(&p); return; }
        if (xa_strieq(mn, ".text"))    { a->cur_section = 0; return; }
        if (xa_strieq(mn, ".data"))    { a->cur_section = 1; return; }
        if (xa_strieq(mn, ".section")) {
            xa_skip_space(&p);
            if (xa_starts_with(p, ".rodata")) a->cur_section = 2;
            else if (xa_starts_with(p, ".data")) a->cur_section = 1;
            else if (xa_starts_with(p, ".text")) a->cur_section = 0;
            else a->cur_section = 0;
            xa_skip_to_eol(&p);
            return;
        }
        if (xa_strieq(mn, ".globl") || xa_strieq(mn, ".global")) {
            /* This is what decides whether the link may bind the symbol from
             * another object -- see xa_is_declared_global. */
            char gname[XA_SYM_MAX];
            xa_skip_space(&p);
            xa_read_word(&p, gname, (int)sizeof(gname));
            xa_declare_global(a, gname);
            xa_skip_to_eol(&p);
            return;
        }
        if (xa_strieq(mn, ".local")) { return; }
        if (xa_strieq(mn, ".weak")) { a->pending_weak = 1; return; }
        if (xa_strieq(mn, ".align") || xa_strieq(mn, ".balign") || xa_strieq(mn, ".p2align")) {
            long long align = 0;
            xa_parse_imm(&p, &align);
            if (align > 1) {
                int rem = a->pc % (int)align;
                if (rem) { int pad; for (pad = 0; pad < (int)align - rem; pad++) xa_emit(a, 0); }
            }
            return;
        }
        if (xa_strieq(mn, ".asciz") || xa_strieq(mn, ".ascii")) {
            xa_emit_string(a, &p, xa_strieq(mn, ".asciz"));
            return;
        }
        if (xa_strieq(mn, ".byte"))  { xa_emit_data(a, &p, 1); return; }
        if (xa_strieq(mn, ".word") || xa_strieq(mn, ".short")) { xa_emit_data(a, &p, 2); return; }
        if (xa_strieq(mn, ".long") || xa_strieq(mn, ".int")) { xa_emit_data(a, &p, 4); return; }
        if (xa_strieq(mn, ".quad")) { xa_emit_data(a, &p, 8); return; }
        if (xa_strieq(mn, ".zero") || xa_strieq(mn, ".skip") || xa_strieq(mn, ".space")) {
            long long n = 0;
            xa_parse_imm(&p, &n);
            { int k; for (k = 0; k < n; k++) xa_emit(a, 0); }
            return;
        }
        if (xa_strieq(mn, ".set") || xa_strieq(mn, ".equ")) { return; }
        /* Unknown directive: skip */
        return;
    }

    /* --- Instructions --- */
    if (xa_strieq(mn, "mov")) {
        xa_emit_mov(a, &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "movq")) {
        xa_emit_movq(a, &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "movsxd")) {
        xa_rex_rm(a, 1, ops[0].reg, &ops[1]);
        xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.63"));
        if (ops[1].kind == XOP_REG) xa_modrm_reg(a, ops[0].reg, ops[1].reg);
        else xa_modrm_mem(a, ops[0].reg, &ops[1]);
    } else if (xa_strieq(mn, "add")) {
        xa_alu(a, xh_x86_number("encoding.xa_alu.1.3.5.0.0"), xh_x86_number("encoding.xa_alu.1.3.5.0.1"), xh_x86_number("encoding.xa_alu.1.3.5.0.2"), xh_x86_number("encoding.xa_alu.1.3.5.0.3"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "sub")) {
        xa_alu(a, xh_x86_number("encoding.xa_alu.41.43.45.5.0"), xh_x86_number("encoding.xa_alu.41.43.45.5.1"), xh_x86_number("encoding.xa_alu.41.43.45.5.2"), xh_x86_number("encoding.xa_alu.41.43.45.5.3"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "cmp")) {
        xa_alu(a, xh_x86_number("encoding.xa_alu.57.59.61.7.0"), xh_x86_number("encoding.xa_alu.57.59.61.7.1"), xh_x86_number("encoding.xa_alu.57.59.61.7.2"), xh_x86_number("encoding.xa_alu.57.59.61.7.3"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "xor")) {
        xa_alu(a, xh_x86_number("encoding.xa_alu.49.51.53.6.0"), xh_x86_number("encoding.xa_alu.49.51.53.6.1"), xh_x86_number("encoding.xa_alu.49.51.53.6.2"), xh_x86_number("encoding.xa_alu.49.51.53.6.3"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "and")) {
        xa_alu(a, xh_x86_number("encoding.xa_alu.33.35.37.4.0"), xh_x86_number("encoding.xa_alu.33.35.37.4.1"), xh_x86_number("encoding.xa_alu.33.35.37.4.2"), xh_x86_number("encoding.xa_alu.33.35.37.4.3"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "or")) {
        xa_alu(a, xh_x86_number("encoding.xa_alu.9.11.13.1.0"), xh_x86_number("encoding.xa_alu.9.11.13.1.1"), xh_x86_number("encoding.xa_alu.9.11.13.1.2"), xh_x86_number("encoding.xa_alu.9.11.13.1.3"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "adc")) {
        xa_alu(a, xh_x86_number("encoding.xa_alu.17.19.21.2.0"), xh_x86_number("encoding.xa_alu.17.19.21.2.1"), xh_x86_number("encoding.xa_alu.17.19.21.2.2"), xh_x86_number("encoding.xa_alu.17.19.21.2.3"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "sbb")) {
        xa_alu(a, xh_x86_number("encoding.xa_alu.25.27.29.3.0"), xh_x86_number("encoding.xa_alu.25.27.29.3.1"), xh_x86_number("encoding.xa_alu.25.27.29.3.2"), xh_x86_number("encoding.xa_alu.25.27.29.3.3"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "push")) {
        if (ops[0].kind == XOP_REG) {
            xa_rex(a, 0, 0, ops[0].reg, -1);
            xa_field_byte(a,"REGOP",xh_x86_number("encoding.register_opcode.50"),ops[0].reg,0,0,2);
        } else if (ops[0].kind == XOP_IMM) {
            int imm = (int)ops[0].imm;
            if (imm >= -128 && imm <= 127) {
                xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.6a"));
                xa_emit(a, (unsigned char)(imm & 0xFF));
            } else {
                xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.68"));
                xa_emit32(a, imm);
            }
        } else if (ops[0].kind == XOP_MEM) {
            xa_rex_rm(a, 0, 6, &ops[0]);
            xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.ff"));
            xa_modrm_mem(a, 6, &ops[0]);
        } else {
            a->error_count++;
        }
    } else if (xa_strieq(mn, "pop")) {
        if (ops[0].kind == XOP_REG) {
            xa_rex(a, 0, 0, ops[0].reg, -1);
            xa_field_byte(a,"REGOP",xh_x86_number("encoding.register_opcode.58"),ops[0].reg,0,0,2);
        } else if (ops[0].kind == XOP_MEM) {
            xa_rex_rm(a, 0, 0, &ops[0]);
            xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.8f"));
            xa_modrm_mem(a, 0, &ops[0]);
        } else {
            a->error_count++;
        }
    } else if (xa_strieq(mn, "lea")) {
        if (ops[0].kind == XOP_REG && ops[1].kind == XOP_MEM) {
            xa_rex_rm(a, (xa_opsize(&ops[0]) == 8) ? 1 : 0, ops[0].reg, &ops[1]);
            xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.8d"));
            xa_modrm_mem(a, ops[0].reg, &ops[1]);
        } else {
            a->error_count++;
            diag_tool(DIAG_SEV_ERROR, "arkcc", "lea needs reg, mem", 0,0,0,0,0,0);
        }
    } else if (xa_strieq(mn, "not")) {
        xa_grp3(a, xh_x86_number("encoding.xa_grp3.2.0"), &ops[0]);
    } else if (xa_strieq(mn, "neg")) {
        xa_grp3(a, xh_x86_number("encoding.xa_grp3.3.0"), &ops[0]);
    } else if (xa_strieq(mn, "test")) {
        int w = xa_is64(&ops[0], &ops[1]) ? 1 : 0;
        if (ops[0].kind == XOP_REG && ops[1].kind == XOP_REG) {
            xa_rex(a, w, ops[1].reg, ops[0].reg, -1);
            xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.85"));
            xa_modrm_reg(a, ops[1].reg, ops[0].reg);
        } else if (ops[0].kind == XOP_REG && ops[1].kind == XOP_IMM) {
            long long imm = ops[1].imm;
            if (!xa_imm_fits32(imm)) { a->error_count++; diag_tool(DIAG_SEV_ERROR, "arkcc", "test immediate out of range", 0,0,0,0,0,0); return; }
            if ((ops[0].reg & 7) == 0 && !w) {  /* eax */
                xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.a9"));
                xa_emit32(a, (int)imm);
            } else {
                xa_rex_rm(a, w, 0, &ops[0]);
                xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.f7"));
                xa_modrm_reg(a, 0, ops[0].reg);
                xa_emit32(a, (int)imm);
            }
        } else if (ops[0].kind == XOP_MEM && ops[1].kind == XOP_IMM) {
            long long imm = ops[1].imm;
            if (!xa_imm_fits32(imm)) { a->error_count++; diag_tool(DIAG_SEV_ERROR, "arkcc", "test immediate out of range", 0,0,0,0,0,0); return; }
            xa_rex_rm(a, w, 0, &ops[0]);
            xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.f7"));
            xa_modrm_mem(a, 0, &ops[0]);
            xa_emit32(a, (int)imm);
        } else {
            a->error_count++;
            diag_tool(DIAG_SEV_ERROR, "arkcc", "unsupported test form", 0,0,0,0,0,0);
        }
    } else if (xa_strieq(mn, "imul")) {
        int w = xa_is64(&ops[0], &ops[1]) ? 1 : 0;
        if (ops[0].kind == XOP_REG && ops[1].kind == XOP_REG) {
            xa_rex(a, w, ops[0].reg, ops[1].reg, -1);
            xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.0f")); xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.af"));
            xa_modrm_reg(a, ops[0].reg, ops[1].reg);
        } else if (ops[0].kind == XOP_REG && ops[1].kind == XOP_IMM) {
            long long imm = ops[1].imm;
            if (!xa_imm_fits32(imm)) { a->error_count++; diag_tool(DIAG_SEV_ERROR, "arkcc", "imul immediate out of range", 0,0,0,0,0,0); return; }
            xa_rex(a, w, ops[0].reg, ops[0].reg, -1);
            xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.69"));
            xa_modrm_reg(a, ops[0].reg, ops[0].reg);
            xa_emit32(a, (int)imm);
        } else if (ops[0].kind == XOP_REG && ops[1].kind == XOP_MEM) {
            xa_rex_rm(a, w, ops[0].reg, &ops[1]);
            xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.0f")); xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.af"));
            xa_modrm_mem(a, ops[0].reg, &ops[1]);
        } else {
            a->error_count++;
            diag_tool(DIAG_SEV_ERROR, "arkcc", "unsupported imul form", 0,0,0,0,0,0);
        }
    } else if (xa_strieq(mn, "mul")) {
        xa_grp3(a, xh_x86_number("encoding.xa_grp3.4.0"), &ops[0]);
    } else if (xa_strieq(mn, "shld")) {
        xa_shxd(a, 0xA4, 0xA5, &ops[0], &ops[1], &ops[2]);
    } else if (xa_strieq(mn, "shrd")) {
        xa_shxd(a, 0xAC, 0xAD, &ops[0], &ops[1], &ops[2]);
    } else if (xa_strieq(mn, "idiv")) {
        xa_grp3(a, xh_x86_number("encoding.xa_grp3.7.0"), &ops[0]);
    } else if (xa_strieq(mn, "div")) {
        xa_grp3(a, xh_x86_number("encoding.xa_grp3.6.0"), &ops[0]);
    } else if (xa_strieq(mn, "cdq")) {
        xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.99"));
    } else if (xa_strieq(mn, "cqo")) {
        xa_rex(a, 1, -1, -1, -1);
        xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.99"));
    } else if (xa_strieq(mn, "cdqe")) {
        xa_rex(a, 1, -1, -1, -1);
        xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.98"));
    } else if (xa_strieq(mn, "cbw")) {
        xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.66")); xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.98"));
    } else if (xa_strieq(mn, "cwd") || xa_strieq(mn, "cwde")) {
        xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.98"));
    } else if (xa_strieq(mn, "shl") || xa_strieq(mn, "sal")) {
        xa_shift(a, xh_x86_number("encoding.xa_shift.4.0"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "shr")) {
        xa_shift(a, xh_x86_number("encoding.xa_shift.5.0"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "sar")) {
        xa_shift(a, xh_x86_number("encoding.xa_shift.7.0"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "rol")) {
        xa_shift(a, xh_x86_number("encoding.xa_shift.0.0"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "ror")) {
        xa_shift(a, xh_x86_number("encoding.xa_shift.1.0"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "movzx")) {
        int w = (xa_opsize(&ops[0]) == 8) ? 1 : 0;
        unsigned char op2 = (xa_opsize(&ops[1]) == 2) ? 0xB7 : 0xB6;
        xa_rex_rm(a, w, ops[0].reg, &ops[1]);
        xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.0f")); xa_emit(a, op2);
        if (ops[1].kind == XOP_REG) xa_modrm_reg(a, ops[0].reg, ops[1].reg);
        else xa_modrm_mem(a, ops[0].reg, &ops[1]);
    } else if (xa_strieq(mn, "movsx") || xa_strieq(mn, "movsxd")) {
        int dsz = xa_opsize(&ops[0]);
        int ssz = xa_opsize(&ops[1]);
        int w = (dsz == 8) ? 1 : 0;
        unsigned char op2;
        if (dsz == 8 && ssz != 2 && ssz != 1 && ssz != 8) {
            /* movsxd: 63 /r */
            xa_rex_rm(a, 1, ops[0].reg, &ops[1]);
            xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.63"));
            if (ops[1].kind == XOP_REG) xa_modrm_reg(a, ops[0].reg, ops[1].reg);
            else xa_modrm_mem(a, ops[0].reg, &ops[1]);
            return;
        }
        op2 = (ssz == 2) ? 0xBF : 0xBE;
        xa_rex_rm(a, w, ops[0].reg, &ops[1]);
        xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.0f")); xa_emit(a, op2);
        if (ops[1].kind == XOP_REG) xa_modrm_reg(a, ops[0].reg, ops[1].reg);
        else xa_modrm_mem(a, ops[0].reg, &ops[1]);
    } else if (xa_strieq(mn, "jmp")) {
        if (ops[0].kind == XOP_REG) {
            xa_rex(a, 0, 0, ops[0].reg, -1);
            xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.ff")); xa_modrm_reg(a, 4, ops[0].reg);
        } else if (ops[0].has_sym) {
            xa_branch(a, xh_x86_number("encoding.xa_branch.233.0.0"), xh_x86_number("encoding.xa_branch.233.0.1"), ops[0].sym);
        } else { a->error_count++; }
    } else if (xa_strieq(mn, "je") || xa_strieq(mn, "jz")) {
        xa_branch(a, xh_x86_number("encoding.xa_branch.0.132.0"), xh_x86_number("encoding.xa_branch.0.132.1"), ops[0].sym);
    } else if (xa_strieq(mn, "jne") || xa_strieq(mn, "jnz")) {
        xa_branch(a, xh_x86_number("encoding.xa_branch.0.133.0"), xh_x86_number("encoding.xa_branch.0.133.1"), ops[0].sym);
    } else if (xa_strieq(mn, "jl")) {
        xa_branch(a, xh_x86_number("encoding.xa_branch.0.140.0"), xh_x86_number("encoding.xa_branch.0.140.1"), ops[0].sym);
    } else if (xa_strieq(mn, "jge")) {
        xa_branch(a, xh_x86_number("encoding.xa_branch.0.141.0"), xh_x86_number("encoding.xa_branch.0.141.1"), ops[0].sym);
    } else if (xa_strieq(mn, "jle")) {
        xa_branch(a, xh_x86_number("encoding.xa_branch.0.142.0"), xh_x86_number("encoding.xa_branch.0.142.1"), ops[0].sym);
    } else if (xa_strieq(mn, "jg")) {
        xa_branch(a, xh_x86_number("encoding.xa_branch.0.143.0"), xh_x86_number("encoding.xa_branch.0.143.1"), ops[0].sym);
    } else if (xa_strieq(mn, "jb") || xa_strieq(mn, "jc")) {
        xa_branch(a, xh_x86_number("encoding.xa_branch.0.130.0"), xh_x86_number("encoding.xa_branch.0.130.1"), ops[0].sym);
    } else if (xa_strieq(mn, "jae") || xa_strieq(mn, "jnc")) {
        xa_branch(a, xh_x86_number("encoding.xa_branch.0.131.0"), xh_x86_number("encoding.xa_branch.0.131.1"), ops[0].sym);
    } else if (xa_strieq(mn, "jbe")) {
        xa_branch(a, xh_x86_number("encoding.xa_branch.0.134.0"), xh_x86_number("encoding.xa_branch.0.134.1"), ops[0].sym);
    } else if (xa_strieq(mn, "ja")) {
        xa_branch(a, xh_x86_number("encoding.xa_branch.0.135.0"), xh_x86_number("encoding.xa_branch.0.135.1"), ops[0].sym);
    } else if (xa_strieq(mn, "js")) {
        xa_branch(a, xh_x86_number("encoding.xa_branch.0.136.0"), xh_x86_number("encoding.xa_branch.0.136.1"), ops[0].sym);
    } else if (xa_strieq(mn, "jns")) {
        xa_branch(a, xh_x86_number("encoding.xa_branch.0.137.0"), xh_x86_number("encoding.xa_branch.0.137.1"), ops[0].sym);
    } else if (xa_strieq(mn, "jp")) {
        xa_branch(a, xh_x86_number("encoding.xa_branch.0.138.0"), xh_x86_number("encoding.xa_branch.0.138.1"), ops[0].sym);
    } else if (xa_strieq(mn, "jnp")) {
        xa_branch(a, xh_x86_number("encoding.xa_branch.0.139.0"), xh_x86_number("encoding.xa_branch.0.139.1"), ops[0].sym);
    } else if (xa_strieq(mn, "sete") || xa_strieq(mn, "setz")) {
        xa_setcc(a, xh_x86_number("encoding.xa_setcc.148.0"), &ops[0]);
    } else if (xa_strieq(mn, "setne") || xa_strieq(mn, "setnz")) {
        xa_setcc(a, xh_x86_number("encoding.xa_setcc.149.0"), &ops[0]);
    } else if (xa_strieq(mn, "setl")) {
        xa_setcc(a, xh_x86_number("encoding.xa_setcc.156.0"), &ops[0]);
    } else if (xa_strieq(mn, "setge")) {
        xa_setcc(a, xh_x86_number("encoding.xa_setcc.157.0"), &ops[0]);
    } else if (xa_strieq(mn, "setle")) {
        xa_setcc(a, xh_x86_number("encoding.xa_setcc.158.0"), &ops[0]);
    } else if (xa_strieq(mn, "setg")) {
        xa_setcc(a, xh_x86_number("encoding.xa_setcc.159.0"), &ops[0]);
    } else if (xa_strieq(mn, "setb") || xa_strieq(mn, "setc")) {
        xa_setcc(a, xh_x86_number("encoding.xa_setcc.146.0"), &ops[0]);
    } else if (xa_strieq(mn, "setae") || xa_strieq(mn, "setnc")) {
        xa_setcc(a, xh_x86_number("encoding.xa_setcc.147.0"), &ops[0]);
    } else if (xa_strieq(mn, "setbe")) {
        xa_setcc(a, xh_x86_number("encoding.xa_setcc.150.0"), &ops[0]);
    } else if (xa_strieq(mn, "seta")) {
        xa_setcc(a, xh_x86_number("encoding.xa_setcc.151.0"), &ops[0]);
    } else if (xa_strieq(mn, "setp")) {
        xa_setcc(a, xh_x86_number("encoding.xa_setcc.154.0"), &ops[0]);
    } else if (xa_strieq(mn, "setnp")) {
        xa_setcc(a, xh_x86_number("encoding.xa_setcc.155.0"), &ops[0]);
    } else if (xa_strieq(mn, "call")) {
        if (ops[0].kind == XOP_REG) {
            xa_rex(a, 0, 0, ops[0].reg, -1);
            xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.ff")); xa_modrm_reg(a, 2, ops[0].reg);
        } else if (ops[0].has_sym) {
            xa_branch(a, xh_x86_number("encoding.xa_branch.232.0.0"), xh_x86_number("encoding.xa_branch.232.0.1"), ops[0].sym);
        } else { a->error_count++; }
    } else if (xa_strieq(mn, "ret")) {
        xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.c3"));
    } else if (xa_strieq(mn, "nop")) {
        xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.90"));
    } else if (xa_strieq(mn, "addsd")) {
        xa_sse(a, xh_x86_number("encoding.xa_sse.242.88.0"), xh_x86_number("encoding.xa_sse.242.88.1"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "subsd")) {
        xa_sse(a, xh_x86_number("encoding.xa_sse.242.92.0"), xh_x86_number("encoding.xa_sse.242.92.1"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "mulsd")) {
        xa_sse(a, xh_x86_number("encoding.xa_sse.242.89.0"), xh_x86_number("encoding.xa_sse.242.89.1"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "divsd")) {
        xa_sse(a, xh_x86_number("encoding.xa_sse.242.94.0"), xh_x86_number("encoding.xa_sse.242.94.1"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "sqrtsd")) {
        xa_sse(a, xh_x86_number("encoding.xa_sse.242.81.0"), xh_x86_number("encoding.xa_sse.242.81.1"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "addss")) {
        xa_sse(a, xh_x86_number("encoding.xa_sse.243.88.0"), xh_x86_number("encoding.xa_sse.243.88.1"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "subss")) {
        xa_sse(a, xh_x86_number("encoding.xa_sse.243.92.0"), xh_x86_number("encoding.xa_sse.243.92.1"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "mulss")) {
        xa_sse(a, xh_x86_number("encoding.xa_sse.243.89.0"), xh_x86_number("encoding.xa_sse.243.89.1"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "divss")) {
        xa_sse(a, xh_x86_number("encoding.xa_sse.243.94.0"), xh_x86_number("encoding.xa_sse.243.94.1"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "sqrtss")) {
        xa_sse(a, xh_x86_number("encoding.xa_sse.243.81.0"), xh_x86_number("encoding.xa_sse.243.81.1"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "movss") && nop == 2) {
        /* F3 0F 10 /r (load) or F3 0F 11 /r (store).  The 32-bit i386
         * emitter stores a float as one word, so every single-precision
         * load/store needs this: without it the assembler rejected the
         * instruction ("unknown instruction \"movss\"") and a float
         * local, float argument or float return could not be assembled at
         * all -- f_float, g1 and g2 failed to build while the double forms
         * worked. */
        if (ops[0].kind == XOP_MEM) xa_sse(a, xh_x86_number("encoding.xa_sse.243.17.0"), xh_x86_number("encoding.xa_sse.243.17.1"), &ops[1], &ops[0]);
        else xa_sse(a, xh_x86_number("encoding.xa_sse.243.16.0"), xh_x86_number("encoding.xa_sse.243.16.1"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "ucomiss")) {
        xa_sse(a, xh_x86_number("encoding.xa_sse.0.46.0"), xh_x86_number("encoding.xa_sse.0.46.1"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "comiss")) {
        xa_sse(a, xh_x86_number("encoding.xa_sse.0.47.0"), xh_x86_number("encoding.xa_sse.0.47.1"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "cvttss2si")) {
        xa_sse(a, xh_x86_number("encoding.xa_sse.243.44.0"), xh_x86_number("encoding.xa_sse.243.44.1"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "cvtsi2ss")) {
        xa_sse(a, xh_x86_number("encoding.xa_sse.243.42.0"), xh_x86_number("encoding.xa_sse.243.42.1"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "ucomisd")) {
        xa_sse(a, xh_x86_number("encoding.xa_sse.102.46.0"), xh_x86_number("encoding.xa_sse.102.46.1"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "comisd")) {
        xa_sse(a, xh_x86_number("encoding.xa_sse.102.47.0"), xh_x86_number("encoding.xa_sse.102.47.1"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "cvttsd2si")) {
        xa_sse(a, xh_x86_number("encoding.xa_sse.242.44.0"), xh_x86_number("encoding.xa_sse.242.44.1"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "cvtsi2sd")) {
        xa_sse(a, xh_x86_number("encoding.xa_sse.242.42.0"), xh_x86_number("encoding.xa_sse.242.42.1"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "cvtsd2ss")) {
        xa_sse(a, xh_x86_number("encoding.xa_sse.242.90.0"), xh_x86_number("encoding.xa_sse.242.90.1"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "cvtss2sd")) {
        xa_sse(a, xh_x86_number("encoding.xa_sse.243.90.0"), xh_x86_number("encoding.xa_sse.243.90.1"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "xorpd") || xa_strieq(mn, "pxor")) {
        xa_sse(a, xh_x86_number("encoding.constant.66"), xa_strieq(mn, "pxor") ? xh_x86_number("encoding.constant.ef") : xh_x86_number("encoding.constant.57"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "andpd")) {
        xa_sse(a, xh_x86_number("encoding.xa_sse.102.84.0"), xh_x86_number("encoding.xa_sse.102.84.1"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "orpd")) {
        xa_sse(a, xh_x86_number("encoding.xa_sse.102.86.0"), xh_x86_number("encoding.xa_sse.102.86.1"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "movapd")) {
        xa_sse(a, xh_x86_number("encoding.xa_sse.102.40.0"), xh_x86_number("encoding.xa_sse.102.40.1"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "movups") || xa_strieq(mn, "movaps") ||
               xa_strieq(mn, "movdqu") || xa_strieq(mn, "movdqa")) {
        /* 128-bit moves: 0F 10/0F 11 (ups), 0F 28/0F 29 (aps),
         * F3 or 66 0F 6F/0F 7F (dqu/dqa).  The variadic prologue saves
         * xmm0..xmm7 with "movups [mem], xmm" and this encoder had no
         * 16-byte form at all, so the flat-image build printed
         * "unknown instruction" for every save -- and still produced an
         * image, i.e. the register save area silently held nothing. */
        int pfx = 0, ld = xh_x86_number("encoding.constant.10"), st = xh_x86_number("encoding.constant.11");
        if (xa_strieq(mn, "movaps")) { ld = xh_x86_number("encoding.constant.28"); st = xh_x86_number("encoding.constant.29"); }
        else if (xa_strieq(mn, "movdqu")) { pfx = xh_x86_number("encoding.constant.f3"); ld = xh_x86_number("encoding.constant.6f"); st = xh_x86_number("encoding.constant.7f"); }
        else if (xa_strieq(mn, "movdqa")) { pfx = xh_x86_number("encoding.constant.66"); ld = xh_x86_number("encoding.constant.6f"); st = xh_x86_number("encoding.constant.7f"); }
        if (nop != 2) {
            a->error_count++;
        } else if (ops[0].kind == XOP_MEM) {
            xa_sse(a, pfx, st, &ops[1], &ops[0]);
        } else {
            xa_sse(a, pfx, ld, &ops[0], &ops[1]);
        }
    } else if (xa_strieq(mn, "movsd") && nop == 2) {
        /* F2 0F 10 /r (load) or F2 0F 11 /r (store) */
        if (ops[0].kind == XOP_MEM) xa_sse(a, xh_x86_number("encoding.xa_sse.242.17.0"), xh_x86_number("encoding.xa_sse.242.17.1"), &ops[1], &ops[0]);
        else xa_sse(a, xh_x86_number("encoding.xa_sse.242.16.0"), xh_x86_number("encoding.xa_sse.242.16.1"), &ops[0], &ops[1]);
    } else if (xa_strieq(mn, "rep") || xa_strieq(mn, "repe") || xa_strieq(mn, "repz")) {
        /* rep movsb / rep stosb / etc.  Two mnemonics share the line and the
         * operand scan above already consumed the second one, so start over
         * from opstart.  Before this the prefix byte 0xF3 was emitted on its
         * own and the string instruction vanished: "rep movsb" assembled to a
         * single stray prefix, so IR_MEMCPY copied nothing and every struct
         * assignment silently produced the destination unchanged. */
        struct { const char *name; int prefix; unsigned char op; } xa_reps[16];
        int rep_count=xh_x86_number("encoding.rep.count"), rep_i;
        char rep_key[64];
        if(rep_count<0 || rep_count>16){a->error_count++;return;}
        for(rep_i=0;rep_i<rep_count;rep_i++){
            snprintf(rep_key,sizeof(rep_key),"encoding.rep.%d.name",rep_i);
            xa_reps[rep_i].name=xh_x86_rule(rep_key);
            snprintf(rep_key,sizeof(rep_key),"encoding.rep.%d.prefix",rep_i);
            xa_reps[rep_i].prefix=xh_x86_number(rep_key);
            snprintf(rep_key,sizeof(rep_key),"encoding.rep.%d.op",rep_i);
            xa_reps[rep_i].op=(unsigned char)xh_x86_number(rep_key);
        }
        char sub[16];
        int k, hit = -1;
        sub[0] = 0;
        p = opstart;
        xa_skip_space(&p);
        if (xa_read_word(&p, sub, (int)sizeof(sub))) {
            for (k = 0; k < rep_count; k++) {
                if (xa_strieq(sub, xa_reps[k].name)) { hit = k; break; }
            }
        }
        if (hit < 0) {
            /* Never emit a bare prefix: that turns a missing instruction into
             * a silently valid but wrong encoding. */
            a->error_count++;
            diag_tool(DIAG_SEV_ERROR, "arkcc", "unsupported rep-prefixed instruction",
                      0,0,0,0,0,0);
        } else {
            xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.f3"));
            if (xa_reps[hit].prefix == 64) xa_rex(a, 1, -1, -1, -1);
            else if (xa_reps[hit].prefix != 0) xa_emit(a, (unsigned char)xa_reps[hit].prefix);
            xa_emit(a, xa_reps[hit].op);
        }
    } else if (xa_strieq(mn, "movsb")) { xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.a4")); }
    else if (xa_strieq(mn, "movsd")) { xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.a5")); }
    else if (xa_strieq(mn, "stosb")) { xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.aa")); }
    else if (xa_strieq(mn, "stosd")) { xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.ab")); }
    else if (xa_strieq(mn, "leave")) { xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.c9")); }
    else if (xa_strieq(mn, "int")) {
        if (ops[0].kind == XOP_IMM) { xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.cd")); xa_emit(a, (unsigned char)(ops[0].imm & 0xFF)); }
    } else if (xa_strieq(mn, "xchg")) {
        int w = xa_is64(&ops[0], &ops[1]) ? 1 : 0;
        if (ops[0].kind == XOP_REG && ops[1].kind == XOP_REG) {
            if ((ops[0].reg & 7) == 0) {
                xa_rex(a, w, 0, ops[1].reg, -1);
                xa_field_byte(a,"REGOP",xh_x86_number("encoding.constant.90"),ops[1].reg,0,0,2);
            } else if ((ops[1].reg & 7) == 0) {
                xa_rex(a, w, 0, ops[0].reg, -1);
                xa_field_byte(a,"REGOP",xh_x86_number("encoding.constant.90"),ops[0].reg,0,0,2);
            } else {
                xa_rex(a, w, ops[0].reg, ops[1].reg, -1);
                xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.87")); xa_modrm_reg(a, ops[0].reg, ops[1].reg);
            }
        } else { a->error_count++; }
    } else if (xa_strieq(mn, "inc")) {
        if (ops[0].kind == XOP_REG) { xa_rex(a, (ops[0].reg_size == 8) ? 1 : 0, 0, ops[0].reg, -1); xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.ff")); xa_modrm_reg(a, 0, ops[0].reg); }
        else { xa_rex_rm(a, (ops[0].mem_size == 8) ? 1 : 0, 0, &ops[0]); xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.ff")); xa_modrm_mem(a, 0, &ops[0]); }
    } else if (xa_strieq(mn, "dec")) {
        if (ops[0].kind == XOP_REG) { xa_rex(a, (ops[0].reg_size == 8) ? 1 : 0, 0, ops[0].reg, -1); xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.ff")); xa_modrm_reg(a, 1, ops[0].reg); }
        else { xa_rex_rm(a, (ops[0].mem_size == 8) ? 1 : 0, 0, &ops[0]); xa_emit(a, (unsigned char)xh_x86_number("encoding.byte.ff")); xa_modrm_mem(a, 1, &ops[0]); }
    } else {
        /* Unknown instruction: emit nop to keep pc advancing */
        a->error_count++;
        diag_tool(DIAG_SEV_ERROR, "arkcc", "unknown instruction %sq", mn,
                  0,0,0,0,0);
    }
    (void)nop;
}

/* --- pass1: compute sizes, collect labels --- */
static void pass1_x(X86Asm *a) {
    const char *p = a->src;
    a->dry_run = 1;
    a->pc = 0;
    a->label_count = 0;
    a->ref_count = 0;
    while (*p) {
        const char *line = p;
        /* Find end of line */
        while (*p && *p != '\n') p++;
        xa_process_line(a, line);
        if (*p == '\n') p++;
    }
}

/* --- pass2: emit machine code, record refs --- */
static void pass2_x(X86Asm *a) {
    const char *p = a->src;
    a->dry_run = 0;
    a->pc = 0;
    a->ref_count = 0;  /* reset refs for pass2 */
    while (*p) {
        const char *line = p;
        while (*p && *p != '\n') p++;
        xa_process_line(a, line);
        if (*p == '\n') p++;
    }
}

/* --- resolve_refs: patch forward references --- */
static void resolve_refs_x(X86Asm *a) {
    int i;
    /* Native ELF relocations retain symbolic local references as well; its
     * writer records the full explicit addend and clears each placeholder. */
    if(a->obj && g_xhcc_elf_mode)return;
    for(i=0;i<a->ref_count;i++){
        X86AsmRef *r=&a->refs[i];
        int addr=xa_find_label(a,r->name),pc=r->type==5 || r->type==X86_RELOC_PC32_ADDEND;
        XhRelocContext context;
        const char *rule;
        if(addr<0){
            if(r->addend)r->type=pc?X86_RELOC_PC32_ADDEND:X86_RELOC_ABS32_ADDEND;
            continue;
        }
        context.symbol=(uint64_t)(unsigned)addr;
        context.addend=(uint64_t)(int64_t)r->addend;
        context.place=(uint64_t)(unsigned)r->pc;
        context.image_base=a->mode64?(uint64_t)X86_IMAGE_BASE:0;
        rule=pc?(r->width==8?"PC64":"PC32"):(r->width==8?"LOCAL_ABS64":"LOCAL_ABS32");
        if(!a->dry_run && xh_relocation_apply(a->mode64?"x86_64":"x86",rule,a->out,(size_t)a->pc,(size_t)r->pc,&context)){
            a->error_count++;continue;
        }
        if(!pc && a->obj)r->type=X86_RELOC_SELF_BASE;
    }
}

/* --- symbol/reloc export for the linker --- */
typedef struct { char *name; int offset; int type; int is_weak; } X86Sym;
typedef struct { char *name; int offset; int type; } X86Reloc;

static X86Sym   g_x86_syms[ARM_MAX_SYMBOLS]   __attribute__((section(".ark_buf")));
static int      g_x86_sym_count = 0;
static X86Reloc g_x86_relocs[ARM_MAX_RELOCS]  __attribute__((section(".ark_buf")));
static int      g_x86_reloc_count = 0;

static void x86_save_syms(X86Asm *a) {
    int i;
    g_x86_sym_count = 0;
    for (i = 0; i < a->label_count && g_x86_sym_count < ARM_MAX_SYMBOLS; i++) {
        /* Only .globl names are linker-visible.  Exporting everything else is
         * what made two TUs with the same static helper (and two TUs with the
         * same string literal) look like duplicate strong definitions. */
        if (!a->labels[i].is_global) continue;
        g_x86_syms[g_x86_sym_count].name    = a->labels[i].name;
        g_x86_syms[g_x86_sym_count].offset   = a->labels[i].addr;
        g_x86_syms[g_x86_sym_count].type     = 0;
        g_x86_syms[g_x86_sym_count].is_weak  = a->labels[i].is_weak;
        g_x86_sym_count++;
    }
}

static void x86_save_relocs(X86Asm *a) {
    int i;
    g_x86_reloc_count = 0;
    for (i = 0; i < a->ref_count && g_x86_reloc_count < ARM_MAX_RELOCS; i++) {
        X86AsmRef *r = &a->refs[i];
        /* A self-base site names a label this object defines, so the lookup
         * below would call it resolved and drop it.  Keep it: the linker is
         * the only side that knows the base it placed this object at. */
        if (r->type == X86_RELOC_SELF_BASE || xa_find_label(a, r->name) < 0) {
            g_x86_relocs[g_x86_reloc_count].name   = r->name;
            g_x86_relocs[g_x86_reloc_count].offset  = r->pc;
            g_x86_relocs[g_x86_reloc_count].type    = r->type;
            g_x86_reloc_count++;
        }
    }
}

int x86_get_symbol_count(void) { return g_x86_sym_count; }
const char *x86_get_symbol_name(int idx) { return g_x86_syms[idx].name; }
int x86_get_symbol_offset(int idx) { return g_x86_syms[idx].offset; }
int x86_get_symbol_type(int idx) { return g_x86_syms[idx].type; }
int x86_get_symbol_weak(int idx) { return g_x86_syms[idx].is_weak; }

int x86_get_reloc_count(void) { return g_x86_reloc_count; }
const char *x86_get_reloc_name(int idx) { return g_x86_relocs[idx].name; }
int x86_get_reloc_offset(int idx) { return g_x86_relocs[idx].offset; }
int x86_get_reloc_type(int idx) { return g_x86_relocs[idx].type; }

/* x86_assemble: encode Intel-syntax assembly text into x86 machine code.
 * Two-pass design: pass1 computes sizes + labels; pass2 emits bytes + refs;
 * resolve_refs patches forward references. All jumps use near (disp32) form
 * so pass1 sizes are fixed. Exports symbol/reloc tables for the ARKOBJ2 linker. */
/* Set by the driver for -c: intra-object absolute addresses then stay
 * relocations instead of being frozen at their object-relative offset.  A
 * flat image has no linker to apply them, so it keeps the old behaviour. */
int g_x86_obj_mode = 0;

static X86Asm xh_x86_asm;
#include "../../../object/target_elf.inc"
int xhcc_x86_write_elf(const char*path,unsigned char*code,int len){
 X86Asm*a=&xh_x86_asm;int i,n=0,nr=0,cap=a->label_count+a->ref_count+8;TeSym*s;TeRel*r;
 if(techeck(path,xh_x86_prog))return ARK_ERR_CODEGEN;s=mem_pool_alloc(cap*(int)sizeof(*s));r=mem_pool_alloc((a->ref_count+1)*(int)sizeof(*r));if(!s||!r)return ARK_ERR_CODEGEN;
 for(i=0;i<a->label_count;i++){X86AsmLabel*l=&a->labels[i];int k=tesym(s,&n,cap,l->name);if(k<0)return ARK_ERR_CODEGEN;s[k].defined=1;s[k].value=(unsigned)l->addr;s[k].bind=l->is_global?(l->is_weak?2:1):0;}
 for(i=0;i<a->ref_count;i++){X86AsmRef*q=&a->refs[i];int k=tesym(s,&n,cap,q->name),pc=q->type==5||q->type==9;int j,tp=pc?2:(a->mode64?(q->width==8?1:10):1);if(k<0||q->pc<0||q->pc+q->width>len)return ARK_ERR_CODEGEN;
 /* Relocate local calls too: global definitions remain interposable at link. */
 if(terel(r,&nr,a->ref_count+1,(unsigned)q->pc,k,tp,q->addend-(pc?4:0)))return ARK_ERR_CODEGEN;for(j=0;j<q->width;j++)code[q->pc+j]=0;
 }
 temeta(s,n,xh_x86_prog);return tewrite(path,code,len,a->mode64?62:3,a->mode64,s,n,r,nr);
}
static int x86_assemble_mode(const char *asm_text, unsigned char *out, int cap, int mode64) {
    if (xh_x86_rules_begin(mode64) || xh_encoding_load(mode64?"x86_64":"x86")) return -1;
    X86Asm a;
    a.src = asm_text;
    a.out = out;
    a.pc = 0;
    a.out_cap = cap;
    a.labels = 0; a.label_count = 0; a.label_cap = 0;
    /* The globals list is what xa_add_label() consults for every label it
     * registers, so an uninitialised pointer here is read on the very first
     * label.  These three fields were simply missing from this reset block,
     * and the two entry paths hid it from each other: the C front end reaches
     * the assembler after the pool and the stack have already been touched, so
     * the stack slot held a zero and the object came out fine, while a plain
     * .s input (which resets the pool right before assembling) left garbage in
     * it and segfaulted on `f:` -- with no diagnostic at all, since the crash
     * beats every error path.  Initialise all three; the arrays are grown
     * lazily by xa_declare_global(). */
    a.globals = 0; a.global_count = 0; a.global_cap = 0;
    a.refs = 0; a.ref_count = 0; a.ref_cap = 0;
    a.error_count = 0;
    a.cur_section = 0;
    a.pending_weak = 0;
    a.dry_run = 0;
    a.mode64 = mode64;
    a.obj = g_x86_obj_mode ? 1 : 0;
    pass1_x(&a);
    pass2_x(&a);
    resolve_refs_x(&a);
    x86_save_syms(&a);
#if ARKCC_PROFILE != ARKCC_PROFILE_STM32_MIN
    /* ARKCC_DUMP_SYMS=1 prints the label map.  A flat image carries no symbol
     * table, so this is the only way a host-side test can find `main` in it --
     * the same contract the riscv32 backend publishes as RV_SYM. */
    if (getenv("ARKCC_DUMP_SYMS")) {
        int li;
        for (li = 0; li < a.label_count; li++) {
            if (a.labels[li].name[0] == '.') continue;
            ark_log("X86_SYM %s 0x%X\n", a.labels[li].name, a.labels[li].addr);
        }
    }
#endif
    x86_save_relocs(&a);
    xh_x86_asm = a;
    a.mode64 = 0;
    g_xa64 = 0;
    /* An encoder error used to leave the offending instruction out and still
     * return a byte count, so the caller wrote an image whose instruction was
     * a lone nop -- a wrong image out of a build that only printed a
     * diagnostic.  Every caller already treats a negative return as
     * "assembly failed", so refuse here instead. */
    if (a.error_count > 0 || xh_x86_rules_failed()) return -1;
    return a.pc;
}

static int x86_assemble(const char *asm_text, unsigned char *out, int cap) {
    return x86_assemble_mode(asm_text, out, cap, 0);
}

static int x86_assemble64(const char *asm_text, unsigned char *out, int cap) {
    return x86_assemble_mode(asm_text, out, cap, 1);
}

/* No-op link: cross-object linking is handled by compiler.c's -link mode
 * (ARKOBJ2 binary linker with symbol resolution + relocation patching). */
static int x86_link(unsigned char *c, int s, struct LinkContext *lc) {
    (void)c; (void)s; (void)lc;
    return 0;
}

static int x86_emit_sysv(IrProgram *prog, char *out, int cap) {
    return x86_emit_internal(prog, out, cap, ARK_ABI_SYSV);
}

static const ArkBackend g_x86_backend = {
    "x86",
    "i386-pc-linux-cdecl",
    4,
    x86_emit_sysv,
    x86_assemble,
    x86_link,
    ARK_ABI_SYSV
};

/* The real 64-bit backend: SysV x86-64 (AMD64) in place of i386 cdecl.
 * pointer_size 8 is what makes type.c lay out pointers, longs and aggregates
 * the way the ABI does; amd64_emit_internal() keeps the compiler's 4-byte
 * stack-word model on top of that, so a pointer is still one "word" but its
 * slot holds the full 64-bit value (IR_AUX_SLOT64). */
static const ArkBackend g_amd64_backend = {
    "x86_64",
    "x86_64-unknown-linux-gnu",
    8,
    amd64_emit_sysv,
    x86_assemble64,
    x86_link,
    ARK_ABI_SYSV
};

/* Kept under its original name because registry.c and compiler.c call it: it
 * is the 32-bit i386 backend. ark_backend_i386() is the name that says so. */
const ArkBackend *ark_backend_x86_64(void) {
    return &g_x86_backend;
}

const ArkBackend *ark_backend_i386(void) {
    return &g_x86_backend;
}

const ArkBackend *ark_backend_amd64(void) {
    return &g_amd64_backend;
}

/* Stub retained for registry.c compatibility (MSVC variant removed).
 * Returns the main x86 backend; -target x86_64-msvc is no longer distinct. */
const ArkBackend *ark_backend_x86_64_msvc(void) {
    return &g_x86_backend;
}
