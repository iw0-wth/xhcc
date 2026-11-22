/* Ark C Compiler - C source backend
 * Emits IR instructions as C source text that the host compiler (gcc) compiles
 * to a native executable.
 *
 * Design overview:
 *   - Each arkcc function is emitted as `long long ark_<name>(long long a0, ...)`.
 *   - The runtime stack is `long long stk[256]` with an `int sp` index; the IR
 *     push/pop model maps directly onto this stack.
 *   - Locals/parameters live in a `char frame[]` array addressed by offset.
 *   - IR_CALL dispatches to either the host libc (for is_libc_func entries) or
 *     to user-defined `ark_<name>` functions; unused argument slots default to 0.
 *   - String literals are emitted as `const char *ark_str_<id>`.
 *   - The C `main` entry is generated separately and calls `ark_main(0,0,...)`.
 *   - Functions with no forward declaration in the IR are assumed to be host
 *     libc functions (printf/putchar/puts/strlen/strcmp/strcpy/memcpy/memset/
 *     abort); their prototypes are emitted before use so the host libc links.
 */
#include "backend.h"
#include "codegen.h"
#include "memory.h"
#include "platform.h"
#include "common.h"
#include "config.h"
#include "arkcc_config.h"
#include "xh_c_rules.h"
#include <stdio.h>   /* snprintf used for float constant formatting */

#define MAX_CALL_ARGS (xh_c_abi()->max_call_args)

static void emit_str_literal(AsmBuffer *b, const char *data, int len) {
    int i;
    asmbuf_putc(b, '"');
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        switch (c) {
            case '\n': asmbuf_puts(b, xh_c_rule_text("escape_newline")); break;
            case '\t': asmbuf_puts(b, xh_c_rule_text("escape_tab")); break;
            case '\r': asmbuf_puts(b, xh_c_rule_text("escape_return")); break;
            case '\\': asmbuf_puts(b, xh_c_rule_text("escape_backslash")); break;
            case '"':  asmbuf_puts(b, xh_c_rule_text("escape_quote")); break;
            default:
                if (c < 0x20 || c >= 0x7F) {
                    char buf[8];
                    int n = 0;
                    buf[n++] = '\\';
                    buf[n++] = 'x';
                    {
                        const char *hex = xh_c_rule_text("escape_hex_digits");
                        buf[n++] = hex[(c >> 4) & 0xF];
                        buf[n++] = hex[c & 0xF];
                    }
                    buf[n] = 0;
                    asmbuf_puts(b, buf);
                    /* A C hexadecimal escape consumes every following hex
                     * digit. End this token before a literal hex character. */
                    if (i + 1 < len && ((data[i+1] >= '0' && data[i+1] <= '9') ||
                        (data[i+1] >= 'a' && data[i+1] <= 'f') ||
                        (data[i+1] >= 'A' && data[i+1] <= 'F')))
                        asmbuf_puts(b, xh_c_rule_text("escape_literal_boundary"));
                } else {
                    asmbuf_putc(b, (char)c);
                }
                break;
        }
    }
    asmbuf_putc(b, '"');
}

/* Emit a binary operator for the C backend.
 * The aux flag bit 0x100 marks an unsigned operation; because C99 integer
 * promotion rules make `int/unsigned` already unsigned, we only need an
 * explicit cast for unsigned int (32-bit) but not for unsigned long long (64-bit).
 * For the 32-bit path (IR_BINARY), we cast to (unsigned int); for the 64-bit
 * path (IR_64BINARY), 32-bit cast is unnecessary.
 * This matters for shifts and comparisons, e.g. int -1 vs unsigned 0xFFFFFFFF. */
static void emit_binop(AsmBuffer *b, int op, int is64) {
    int is_unsigned = (op & 0x100) != 0;
    int base_op = op & ~0x100;
    /* lhs op rhs form; operands are loaded into ark_a/ark_c before emission. */
    switch (base_op) {
        case (int)TOK_PLUS:  asmbuf_puts(b, xh_c_rule_text("tok_plus")); break;
        case (int)TOK_MINUS: asmbuf_puts(b, xh_c_rule_text("tok_minus")); break;
        case (int)TOK_MUL:   asmbuf_puts(b, xh_c_rule_text("tok_mul")); break;
        case (int)TOK_DIV:   asmbuf_puts(b, xh_c_rule_text("tok_div")); break;
        case (int)TOK_MOD:   asmbuf_puts(b, xh_c_rule_text("tok_mod")); break;
        case (int)TOK_BAND:  asmbuf_puts(b, xh_c_rule_text("tok_band")); break;
        case (int)TOK_BOR:   asmbuf_puts(b, xh_c_rule_text("tok_bor")); break;
        case (int)TOK_BXOR:  asmbuf_puts(b, xh_c_rule_text("tok_bxor")); break;
        case (int)TOK_SHL:   asmbuf_puts(b, xh_c_rule_text("tok_shl")); break;
        case (int)TOK_SHR:
            /* The cast must match the *operand* width: a 32-bit unsigned right
             * shift has to clear the high bits of a 32-bit value, a 64-bit one
             * must keep all 64.  ark_a is a long long in both cases, so without
             * this the 64-bit form was truncated to 32 bits. */
            if (is_unsigned) asmbuf_puts(b, is64 ? xh_c_rule_text("tok_shr")
                                                : xh_c_rule_text("tok_shr_2"));
            else             asmbuf_puts(b, xh_c_rule_text("tok_shr_3"));
            break;
        case (int)TOK_EQ:    asmbuf_puts(b, xh_c_rule_text("tok_eq")); break;
        case (int)TOK_NE:    asmbuf_puts(b, xh_c_rule_text("tok_ne")); break;
        case (int)TOK_LT:
            if (is_unsigned) asmbuf_puts(b, xh_c_rule_text("tok_lt"));
            else             asmbuf_puts(b, xh_c_rule_text("tok_lt_2"));
            break;
        case (int)TOK_GT:
            if (is_unsigned) asmbuf_puts(b, xh_c_rule_text("tok_gt"));
            else             asmbuf_puts(b, xh_c_rule_text("tok_gt_2"));
            break;
        case (int)TOK_LE:
            if (is_unsigned) asmbuf_puts(b, xh_c_rule_text("tok_le"));
            else             asmbuf_puts(b, xh_c_rule_text("tok_le_2"));
            break;
        case (int)TOK_GE:
            if (is_unsigned) asmbuf_puts(b, xh_c_rule_text("tok_ge"));
            else             asmbuf_puts(b, xh_c_rule_text("abi_tok_ge_2"));
            break;
        default:             xh_c_rules_error("unsupported C binary operation"); break;
    }
}

static void emit_func_proto(AsmBuffer *b, IrFunc *f) {
    int i;
    /* Emit only the prototype; static linkage keeps it file-local.
     * __naked is a GCC attribute; __irq is emitted as a comment because host
     * builds do not handle interrupt vectoring.
     * __weak is dropped on host targets: mingw-gcc turns a weak definition into
     * an unresolved weak external, so the call jumps to a bogus alias and
     * faults at run time; weak linkage is not observable in a
     * single-translation-unit host build. */
    if (f->is_weak)   asmbuf_puts(b, xh_c_rule_text("attribute_tok_ge_4"));
    if (f->is_naked || f->is_asm_func) asmbuf_puts(b, xh_c_rule_text("attribute_tok_ge_5"));
    if (f->is_used)   asmbuf_puts(b, xh_c_rule_text("attribute_tok_ge_6"));
    if (f->is_unused) asmbuf_puts(b, xh_c_rule_text("attribute_tok_ge_7"));
    if (f->forceinline) asmbuf_puts(b, xh_c_rule_text("attribute_tok_ge_8"));
    /* constructor/destructor emit into gcc's .init_array/.fini_array sections;
     * deprecated triggers a gcc warning at the use site. */
    if (f->is_constructor) asmbuf_puts(b, xh_c_rule_text("attribute_tok_ge_9"));
    if (f->is_destructor)  asmbuf_puts(b, xh_c_rule_text("attribute_tok_ge_10"));
    if (f->is_deprecated)  asmbuf_puts(b, xh_c_rule_text("attribute_tok_ge_11"));
#if ARKCC_FEATURE_ARMCC_KEYWORDS
    /* armcc 5 keywords mapped to the closest gcc equivalent:
     *   __pure            -> __attribute__((pure))
     *   __attribute__((const)) -> __attribute__((const))
     *   __value_in_regs   -> no gcc equivalent; ignored (struct return ABI differs)
     *   __interwork       -> ARM/Thumb interworking; ignored on host builds
     *   __smc(n)          -> secure monitor call; ignored on host builds */
    if (f->is_pure)       asmbuf_puts(b, xh_c_rule_text("attribute_tok_ge_12"));
    if (f->is_const_attr) asmbuf_puts(b, xh_c_rule_text("attribute_tok_ge_13"));
#endif
    if (f->alias_target) {
        /* alias attribute: __attribute__((alias("ark_xxx"))) */
        asmbuf_puts(b, xh_c_rule_text("attribute_tok_ge_14"));
        asmbuf_puts(b, f->alias_target);
        asmbuf_puts(b, xh_c_rule_text("abi_tok_ge_15"));
    }
    asmbuf_puts(b, f->is_static ? xh_c_rule_text("abi_tok_ge_16") : xh_c_rule_text("abi_tok_ge_17"));
    asmbuf_puts(b, f->name);
    asmbuf_puts(b, xh_c_rule_text("abi_tok_ge_18"));
    for (i = 0; i < MAX_CALL_ARGS; i++) {
        if (i) asmbuf_puts(b, xh_c_rule_text("abi_tok_ge_19"));
        asmbuf_puts(b, xh_c_rule_text("abi_tok_ge_20"));
        asmbuf_int(b, i);
    }
    asmbuf_puts(b, xh_c_rule_text("abi_tok_ge_21"));
    if (f->is_irq) asmbuf_puts(b, xh_c_rule_text("abi_tok_ge_22"));
#if ARKCC_FEATURE_ARMCC_KEYWORDS
    if (f->is_value_in_regs) asmbuf_puts(b, xh_c_rule_text("abi_tok_ge_23"));
    if (f->is_interwork)     asmbuf_puts(b, xh_c_rule_text("abi_tok_ge_24"));
    if (f->smc_num > 0) {
        xh_c_rule_ints(b, "abi_tok_ge_value", f->smc_num, 0);
    }
    if (f->global_reg >= 0) {
        xh_c_rule_ints(b, "abi_tok_ge_value_2", f->global_reg, 0);
    }
#endif
    asmbuf_putc(b, '\n');
}

/* Decide whether a function calls directly into host libc; such functions do
 * not have an ark_ wrapper and are referenced via extern declarations. */
static int is_libc_func(const char *name) {
    return xh_c_rule_member("abi_libcs", name);
}

/* Decide whether a libc function returns void. Such functions never push a
 * result onto the stack; the IR_CALL path invokes them without synthesizing
 * a return value (irgen skips IR_POP for direct void calls). User-defined
 * void functions are detected separately via prog->funcs in the IR_CALL
 * handler. */
static int is_void_func(const char *name) {
    return xh_c_rule_member("abi_voids", name);
}

/* Decide whether a function returns a struct. These functions must not be
 * cast to (long long); the result is stored via the struct-return slot and
 * the quot/rem members need correct typing. */
static int is_struct_ret_func(const char *name) {
    return xh_c_rule_member("abi_structs", name);
}

static int is_complex_ret_func(const char *name) {
    return xh_c_rule_member("abi_cplx", name);
}

static const char *struct_ret_type(const char *name) {
    return xh_c_rule_map("abi_struct_return_types", name);
}

/* Decide whether a function has an ark_ wrapper. These are static inline
 * helpers that take long long slots and bridge to the host libc with the
 * correct double/pointer casts; IR_CALL routes ark_func(...) through them.
 * Wrapper returns are long long, so the caller uses host libc types. */
static int is_wrapped_func(const char *name) {
    return xh_c_rule_member("abi_wrapped", name);
}

/* Decide whether a name refers to a host libc variable; the C backend emits
 * extern declarations for these so the host linker can resolve them.
 * Variables like stdin/stdout/stderr/errno/FE_DFL_ENV are types defined in
 * the host's <stdio.h>/<errno.h>/<fenv.h>; arkcc declares them through the
 * standard headers and the IR sees them as already extern. */
static int is_libc_var(const char *name) {
    return xh_c_rule_member("abi_vars", name);
}

/* Emit an extern prototype so undefined references in this TU link correctly. */
static void emit_extern_proto(AsmBuffer *b, const char *name) {
    int i;
    asmbuf_puts(b, xh_c_rule_text("abi_tok_ge_33"));
    asmbuf_puts(b, name);
    asmbuf_puts(b, xh_c_rule_text("abi_tok_ge_18"));
    for (i = 0; i < MAX_CALL_ARGS; i++) {
        if (i) asmbuf_puts(b, xh_c_rule_text("abi_tok_ge_19"));
        asmbuf_puts(b, xh_c_rule_text("abi_tok_ge_20"));
        asmbuf_int(b, i);
    }
    asmbuf_puts(b, xh_c_rule_text("abi_tok_ge_34"));
}

/* Emit a C type with declarator syntax. Follows the standard layout:
 *   - scalar types print as "<base> <name>"
 *   - pointers recurse with base, name gets a '*' prefix
 *   - arrays recurse with base+name, then append "[size]" ("[]" when size==0)
 *   - struct/union emit either an inline definition or a tag reference.
 * Storage class is added by the caller (e.g., const/static/section). */
static void emit_c_type(AsmBuffer *b, Type *t, const char *name) {
    if (!t) { asmbuf_puts(b, xh_c_rule_text("abi_tok_ge_35")); asmbuf_puts(b, name); return; }
    /* F2: Emit volatile/const qualifiers for non-pointer types.
     * For pointer types, qualifiers on the pointer itself are not emitted here
     * (would require placement after '*'); base-type qualifiers are correctly
     * emitted through recursion into t->base. This covers the common HAL patterns:
     *   volatile uint32_t x  ->  volatile unsigned int x
     *   volatile uint32_t *p ->  volatile unsigned int *p  (volatile on base) */
    if (t->kind != TYPE_PTR) {
        if (t->is_volatile) asmbuf_puts(b, xh_c_rule_text("abi_tok_ge_36"));
        if (t->is_const) asmbuf_puts(b, xh_c_rule_text("abi_tok_ge_37"));
    }
    switch (t->kind) {
        case TYPE_VOID:   asmbuf_puts(b, xh_c_rule_text("type_void")); break;
        case TYPE_CHAR:   asmbuf_puts(b, xh_c_rule_text("type_char")); break;
        case TYPE_SHORT:  asmbuf_puts(b, xh_c_rule_text("type_short")); break;
        case TYPE_INT:    asmbuf_puts(b, xh_c_rule_text("type_int")); break;
        case TYPE_LONG:   asmbuf_puts(b, xh_c_rule_text("type_long")); break;
        case TYPE_LLONG:  asmbuf_puts(b, xh_c_rule_text("abi_tok_ge_35")); break;
        case TYPE_BOOL:   asmbuf_puts(b, xh_c_rule_text("type_bool")); break;
        case TYPE_FLOAT:  asmbuf_puts(b, xh_c_rule_text("type_float")); break;
        case TYPE_DOUBLE: asmbuf_puts(b, xh_c_rule_text("type_double")); break;
        case TYPE_ENUM:   asmbuf_puts(b, xh_c_rule_text("type_int")); break;
        case TYPE_WCHAR:  asmbuf_puts(b, xh_c_rule_text("type_wchar")); break;  /* B2: matches gcc wchar_t */
        case TYPE_COMPLEX: /* B4: _Complex float/double supported by gcc */
            asmbuf_puts(b, xh_c_rule_text("type_complex"));
            if (t->base && t->base->kind == TYPE_FLOAT) asmbuf_puts(b, xh_c_rule_text("type_float"));
            else asmbuf_puts(b, xh_c_rule_text("type_double"));
            break;
        case TYPE_PTR: {
            /* If name starts with '*', prepend another '*' to the base. */
            char namebuf[256];
            int i = 0;
            namebuf[i++] = '*';
            if (name) {
                const char *p = name;
                while (*p && i < (int)sizeof(namebuf) - 1) namebuf[i++] = *p++;
                if (*p) { xh_c_rules_error("C declarator name exceeds capacity"); return; }
            }
            namebuf[i] = 0;
            /* For pointer to struct/union, emit "struct Tag *name" without recursing
             * into the struct body. This prevents infinite recursion on self-referential
             * types (e.g. struct Type { StructField *fields; } where StructField has
             * a Type* field). Anonymous struct pointers use "void *" as fallback. */
            if (t->base && (t->base->kind == TYPE_STRUCT || t->base->kind == TYPE_UNION)) {
                if (t->base->tag) {
                    asmbuf_puts(b, t->base->kind == TYPE_UNION ? xh_c_rule_text("type_ptr") : xh_c_rule_text("type_ptr_2"));
                    asmbuf_puts(b, t->base->tag);
                    asmbuf_puts(b, xh_c_rule_text("type_ptr_3"));
                    asmbuf_puts(b, namebuf);
                    return;
                } else {
                    asmbuf_puts(b, xh_c_rule_text("type_void"));
                    asmbuf_puts(b, namebuf);
                    return;
                }
            }
            /* Function pointer: emit "<ret> (*name)(<params>)".
             * Without this, function pointer fields in structs (e.g. the
             * ArkBackend vtable) would be emitted as "long long *name",
             * losing the function-pointer type and breaking initializers
             * that reference function symbols. */
            if (t->base && t->base->kind == TYPE_FUNC) {
                Type *func = t->base;
                int j;
                emit_c_type(b, func->base, xh_c_rule_text("type_ptr_4"));
                asmbuf_puts(b, xh_c_rule_text("type_ptr_5"));
                if (name) asmbuf_puts(b, name);
                asmbuf_puts(b, xh_c_rule_text("type_ptr_6"));
                for (j = 0; j < func->param_count; j++) {
                    if (j) asmbuf_puts(b, xh_c_rule_text("abi_tok_ge_19"));
                    emit_c_type(b, func->params[j], xh_c_rule_text("type_ptr_4"));
                }
                if (func->is_variadic) {
                    if (func->param_count) asmbuf_puts(b, xh_c_rule_text("abi_tok_ge_19"));
                    asmbuf_puts(b, xh_c_rule_text("type_ptr_7"));
                }
                asmbuf_puts(b, xh_c_rule_text("type_ptr_8"));
                return;
            }
            emit_c_type(b, t->base, namebuf);
            return;
        }
        case TYPE_ARRAY: {
            /* Multi-dim arrays: collect dimension list, emit base+name then
             * suffix each "[size]" left to right (e.g. int m[2][3] -> m is
             * an array of 2 arrays of int[3]). */
            int dims[16];
            int n = 0;
            Type *cur = t;
            while (cur && cur->kind == TYPE_ARRAY && n < 16) {
                dims[n++] = cur->size;
                cur = cur->base;
            }
            if (cur && cur->kind == TYPE_ARRAY) {
                xh_c_rules_error("C array declarator exceeds dimension capacity");
                return;
            }
            emit_c_type(b, cur, name);
            {
                int k;
                for (k = 0; k < n; k++) {
                    asmbuf_puts(b, xh_c_rule_text("type_array"));
                    if (dims[k] > 0) asmbuf_int(b, dims[k]);
                    asmbuf_puts(b, xh_c_rule_text("type_array_2"));
                }
            }
            return;
        }
        case TYPE_STRUCT:
        case TYPE_UNION: {
            asmbuf_puts(b, t->kind == TYPE_UNION ? xh_c_rule_text("type_ptr") : xh_c_rule_text("type_ptr_2"));
            if (t->fields) {
                StructField *f;
                asmbuf_puts(b, xh_c_rule_text("type_union"));
                for (f = t->fields; f; f = f->next) {
                    if (f->name && f->name[0]) {
                        emit_c_type(b, f->type, f->name);
                        asmbuf_puts(b, xh_c_rule_text("type_union_2"));
                    }
                }
                asmbuf_puts(b, xh_c_rule_text("type_union_3"));
            } else if (t->tag) {
                asmbuf_puts(b, t->tag);
                asmbuf_puts(b, xh_c_rule_text("type_ptr_3"));
            }
            break;
        }
        default:
            asmbuf_puts(b, xh_c_rule_text("abi_tok_ge_35"));
            break;
    }
    asmbuf_puts(b, name);
}

/* Check whether a C type is an aggregate (array/struct/union). */
static int c_type_is_aggregate(Type *t) {
    if (!t) return 0;
    return t->kind == TYPE_ARRAY || t->kind == TYPE_STRUCT || t->kind == TYPE_UNION;
}

/* Compute the stack-effect delta of a single IR instruction.
 * Positive = pushes, negative = pops, zero = no net change. */
static int ir_stack_delta(IrInst *I) {
    switch (I->op) {
        case IR_PUSH_INT:
        case IR_PUSH_VAR:
        case IR_PUSH_ADDR:
        case IR_PUSH_STR:
        case IR_PUSH_GLOBAL:
        case IR_PUSH_GLOBAL_ADDR:
        case IR_PUSH_FUNC:
        case IR_TRUNC:
        case IR_PUSH_FLOAT:
        case IR_64PUSH:
        case IR_DUP:
            return 1;
        case IR_POP:
        case IR_STORE:
        case IR_STORE_GLOBAL:
        case IR_JZ:
        case IR_JNZ:
        case IR_RETURN:
        case IR_BINARY:
        case IR_FADD:
        case IR_FSUB:
        case IR_FMUL:
        case IR_FDIV:
        case IR_FCMP:
        case IR_DIV:
        case IR_MOD:
        case IR_64BINARY:
        case IR_64CMP:
        case IR_LOAD_IDX:
        case IR_STORE_IDX:
        case IR_STORE_FLD:
        case IR_SIMD_ADD16:
        case IR_SIMD_SUB16:
        case IR_SIMD_ADD8:
        case IR_SIMD_SUB8:
            return -1;
        case IR_UNARY:
        case IR_F2I:
        case IR_I2F:
        case IR_F2F:
        case IR_64F2I:
        case IR_64I2F:
        case IR_ALLOCA:
        case IR_SVC:
        case IR_LOAD_FLD:
        case IR_SWAP:
        case IR_SEXT:
        case IR_ZEXT:
        case IR_CDECL64:
        case IR_JMP:
        case IR_JMP_NAMED:
        case IR_LABEL:
        case IR_LABEL_NAMED:
        case IR_CMP_JMP_EQ:
            return 0;
        case IR_CALL: {
            int n = (int)I->val;
            if (n > MAX_CALL_ARGS) n = MAX_CALL_ARGS;
            return -(n) + 1; /* pop n args, push 1 result */
        }
        case IR_MEMCPY:
            return -2; /* pop dst, src */
        default:
            return 0;
    }
}

/* Compute the maximum stack depth (sp value) for an IR function.
 * Uses fixpoint iteration to correctly handle jumps: a forward jump may
 * skip over POP instructions, so the sp at a label reached via a jump can
 * be HIGHER than the sp at that label reached via the fall-through path.
 * The linear-only approach underestimates stk[] in such cases, causing
 * stack overflow at runtime in the generated C code. */
/* Label table for the C backend's stack bookkeeping. Host 16384 / board 4096
 * (the C backend is trimmed on the board anyway), overridable. */
#define MAX_LABELS_FOR_SP  ARK_C_MAX_LABELS
static int compute_max_sp(IrFunc *f) {
    /* Collect named labels and assign them indices after numeric labels. */
    int named_count = 0;
    const char *named_names[256];
    int i;
    int total_labels;
    int label_sp_stack[MAX_LABELS_FOR_SP];
    int *label_sp = label_sp_stack;
    int max_sp = 1;
    int iter;

    for (i = 0; i < f->code_count && named_count < 256; i++) {
        if (f->code[i].op == IR_LABEL_NAMED) {
            const char *name = f->code[i].name;
            int found = 0, j;
            for (j = 0; j < named_count; j++) {
                if (named_names[j] && name && ark_streq(named_names[j], name)) {
                    found = 1; break;
                }
            }
            if (!found) named_names[named_count++] = name;
        }
    }

    total_labels = f->label_count + named_count;
    if (total_labels < 1) total_labels = 1;
    /* If too many labels, fall back to linear scan (overestimate is safe). */
    if (total_labels > MAX_LABELS_FOR_SP) {
        int sp = 0;
        max_sp = 1;
        for (i = 0; i < f->code_count; i++) {
            int delta = ir_stack_delta(&f->code[i]);
            sp += delta;
            if (sp > max_sp) max_sp = sp;
            if (sp < 0) sp = 0;
        }
        return max_sp;
    }
    for (i = 0; i < total_labels; i++) label_sp[i] = -1;

    /* Fixpoint iteration: repeat until sp at labels stabilizes. */
    for (iter = 0; iter < 32; iter++) {
        int sp = 0;
        int changed = 0;
        max_sp = 1;

        for (i = 0; i < f->code_count; i++) {
            IrInst *I = &f->code[i];
            int delta = ir_stack_delta(I);
            int jmp_tgt = -1;
            int jmp_sp = 0;

            switch (I->op) {
                case IR_LABEL:
                    if (label_sp[(int)I->val] > sp) sp = label_sp[(int)I->val];
                    break;
                case IR_LABEL_NAMED: {
                    int j;
                    for (j = 0; j < named_count; j++) {
                        if (named_names[j] && I->name &&
                            ark_streq(named_names[j], I->name)) {
                            int idx = f->label_count + j;
                            if (label_sp[idx] > sp) sp = label_sp[idx];
                            break;
                        }
                    }
                    break;
                }
                case IR_JMP:
                    jmp_tgt = (int)I->val;
                    jmp_sp = sp; /* JMP does not pop */
                    break;
                case IR_JMP_NAMED: {
                    int j;
                    for (j = 0; j < named_count; j++) {
                        if (named_names[j] && I->name &&
                            ark_streq(named_names[j], I->name)) {
                            jmp_tgt = f->label_count + j;
                            jmp_sp = sp;
                            break;
                        }
                    }
                    break;
                }
                case IR_JZ:
                case IR_JNZ:
                    /* JZ/JNZ pop the condition, then jump with sp-1. */
                    jmp_tgt = (int)I->val;
                    jmp_sp = sp - 1;
                    break;
                case IR_CMP_JMP_EQ:
                    /* On match, peeks then pops 1 and jumps. Conservatively
                     * assume the jump path has sp-1. */
                    jmp_tgt = I->aux;
                    jmp_sp = sp - 1;
                    break;
                default:
                    break;
            }

            if (jmp_tgt >= 0 && jmp_tgt < total_labels && jmp_sp > label_sp[jmp_tgt]) {
                label_sp[jmp_tgt] = jmp_sp;
                changed = 1;
            }

            sp += delta;
            if (sp > max_sp) max_sp = sp;
            if (sp < 0) sp = 0;
        }

        if (!changed) break;
    }
    return max_sp;
}

/* Compute the frame size needed for a function. The frame must accommodate:
 *   - local_size: all local variables and parameters (from IR)
 *   - 8000-8128: variadic argument area (only for variadic functions)
 *   - 8176+: struct return area (only for struct-returning functions)
 * We use max(local_size + 64, 8704) to ensure backward compatibility with
 * the fixed areas while allowing larger frames when needed. */
static int compute_frame_size(IrFunc *f) {
    int needed = f->local_size + xh_c_abi()->frame_padding; /* 64 bytes padding for alignment */
    if (needed < xh_c_abi()->frame_min) needed = xh_c_abi()->frame_min; /* preserve fixed areas for variadic/struct return */
    /* round up to 8-byte alignment */
    needed = (needed + xh_c_abi()->frame_align - 1) & ~(xh_c_abi()->frame_align - 1);
    return needed;
}

static int c_emit(IrProgram *prog, char *out, int cap) {
    AsmBuffer buf;
    int fi;
    int main_idx = -1;
    if (xh_c_rules_begin()) return -1;
    asmbuf_init(&buf);

    asmbuf_puts(&buf, xh_c_rule_text("file_header"));
    /* The stack-machine model casts `char frame[]` to long long, int, double
     * pointers etc. Under -O2 -fstrict-aliasing, gcc may assume these casts
     * do not alias and miscompile struct-by-value passes (e.g. returning 0
     * instead of the packed value). Disable strict aliasing for the file. */
    asmbuf_puts(&buf, xh_c_rule_text("ir_cmp_jmp_eq"));
    asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_2"));
    asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_3"));
    asmbuf_puts(&buf, xh_c_rule_text("include_stdio_h"));
    asmbuf_puts(&buf, xh_c_rule_text("include_stdlib_h"));
    asmbuf_puts(&buf, xh_c_rule_text("include_string_h"));
    asmbuf_puts(&buf, xh_c_rule_text("include_stdint_h"));
    asmbuf_puts(&buf, xh_c_rule_text("include_setjmp_h"));
    asmbuf_puts(&buf, xh_c_rule_text("include_malloc_h"));   /* B3: alloca for VLA */
    asmbuf_puts(&buf, xh_c_rule_text("include_wchar_h"));    /* B2: wchar_t type */
    asmbuf_puts(&buf, xh_c_rule_text("include_wctype_h"));   /* iswalpha/towupper etc. */
    asmbuf_puts(&buf, xh_c_rule_text("include_errno_h"));     /* errno: mingw defines it via thread-local accessor */
    asmbuf_puts(&buf, xh_c_rule_text("include_fenv_h"));      /* FE_DFL_ENV: host extern symbol */
    asmbuf_puts(&buf, xh_c_rule_text("include_math_h"));      /* sqrt/fabs/sin/cos/pow */
    asmbuf_puts(&buf, xh_c_rule_text("include_complex_h"));   /* creal/cimag/cabs/carg/cexp */
    asmbuf_puts(&buf, xh_c_rule_text("include_time_h"));      /* time/localtime/strftime */
    asmbuf_puts(&buf, xh_c_rule_text("include_locale_h"));    /* setlocale/localeconv */
    asmbuf_puts(&buf, xh_c_rule_text("include_signal_h"));  /* signal */
    asmbuf_puts(&buf, xh_c_rule_text("include_inttypes_h"));  /* imaxdiv_t/imaxabs/strtoumax */
    /* mingw's swprintf/vswprintf lack the C99 size_t n parameter; use _snwprintf/_vsnwprintf instead. */
    asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_20"));
    asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_21"));
    asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_22"));
    asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_23"));
    asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_24"));
    asmbuf_puts(&buf, xh_c_rule_text("runtime_ir_cmp_jmp_eq_25"));
    /* Frame accessors. `frame` is a char array, so `*(long long*)(frame + N)`
     * is a strict-aliasing violation: the object has declared type char[], and
     * reading/writing it through a casted long long or int pointer lets an
     * optimising host compiler assume the accesses do not alias each other (at
     * -O2 gcc miscompiled test_struct_byval back to 0). memcpy is the one
     * defined way to reinterpret an object representation, so every typed frame
     * access goes through these helpers instead of a cast. */
    asmbuf_puts(&buf,
        xh_c_rule_text("runtime_ir_cmp_jmp_eq_26"));
    asmbuf_puts(&buf,
        xh_c_rule_text("runtime_ir_cmp_jmp_eq_27"));
    asmbuf_puts(&buf,
        xh_c_rule_text("runtime_ir_cmp_jmp_eq_28"));
    asmbuf_puts(&buf,
        xh_c_rule_text("runtime_ir_cmp_jmp_eq_29"));
    /* ark_<libc> wrapper macros: arkcc IR functions are declared as
     * (long long a0..a7) -> long long. These static inline helpers:
     *   1. Take long long slots and cast back to double/pointer/int as needed
     *   2. Call the corresponding host libc function
     *   3. Cast the result back to long long (double uses the union slot)
     * The actual instantiations are below; the IR_CALL path picks the right route. */
    asmbuf_puts(&buf,
        xh_c_rule_text("runtime_ir_cmp_jmp_eq_30")
    );
    /* Single-arg double->double wrapper instantiations */
    asmbuf_puts(&buf,
        xh_c_rule_text("runtime_ir_cmp_jmp_eq_31")
    );
    asmbuf_putc(&buf, '\n');
    asmbuf_puts(&buf, xh_c_rule_text("runtime_ir_cmp_jmp_eq_32"));
    asmbuf_puts(&buf, xh_c_rule_text("runtime_ir_cmp_jmp_eq_33"));
    asmbuf_puts(&buf, xh_c_rule_text("runtime_ir_cmp_jmp_eq_34"));

    /* Float constants: emit %.17g for round-trip precision while keeping the
     * minimal significant digits. Values near 0 (e.g. 1e-10) or huge (e.g.
     * 1e100) require full precision; printing only 9 fractional digits would
     * produce "0.000000000" for 1e-10. */
    {
        int i;
        for (i = 0; i < prog->float_count; i++) {
            char fbuf[64];
            double v = prog->floats[i];
            xh_c_rule_ints(&buf, "module_ir_cmp_jmp_eq_value", i, 0);
            /* Detect NaN/Inf: %.17g would emit mingw's "nan"/"inf" strings,
             * which are not valid C tokens. Use 0.0f/0.0f and HUGE_VAL literals. */
            if (v != v) {
                asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_35"));  /* NAN */
            } else if (v > 1e308) {
                asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_36"));  /* +INFINITY (float HUGE_VALF) */
            } else if (v < -1e308) {
                asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_37"));  /* -INFINITY */
            } else {
                snprintf(fbuf, sizeof(fbuf), "%.17g", v);
                asmbuf_puts(&buf, fbuf);
            }
            asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_38"));
        }
        if (prog->float_count) asmbuf_putc(&buf, '\n');
    }

    /* Complex constants: use %.17g for round-trip precision on real and imag
     * parts. GCC accepts the i suffix; emit as "re + im * 1.0i" form. */
    {
        int i;
        for (i = 0; i < prog->complex_count; i++) {
            char rbuf[64], ibuf[64];
            double re = prog->complexes[i].re;
            double im = prog->complexes[i].im;
            xh_c_rule_ints(&buf, "module_ir_cmp_jmp_eq_value_2", i, 0);
            /* Detect NaN/Inf */
            if (re != re) xh_c_rule_copy(rbuf, sizeof(rbuf), "module_ir_cmp_jmp_eq_35");
            else if (re > 1e308) xh_c_rule_copy(rbuf, sizeof(rbuf), "module_ir_cmp_jmp_eq_36");
            else if (re < -1e308) xh_c_rule_copy(rbuf, sizeof(rbuf), "module_ir_cmp_jmp_eq_37");
            else snprintf(rbuf, sizeof(rbuf), "%.17g", re);
            if (im != im) xh_c_rule_copy(ibuf, sizeof(ibuf), "module_ir_cmp_jmp_eq_35");
            else if (im > 1e308) xh_c_rule_copy(ibuf, sizeof(ibuf), "module_ir_cmp_jmp_eq_36");
            else if (im < -1e308) xh_c_rule_copy(ibuf, sizeof(ibuf), "module_ir_cmp_jmp_eq_37");
            else snprintf(ibuf, sizeof(ibuf), "%.17g", im);
            asmbuf_puts(&buf, rbuf);
            asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_39"));
            asmbuf_puts(&buf, ibuf);
            asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_40"));
        }
        if (prog->complex_count) asmbuf_putc(&buf, '\n');
    }

    /* String constants */
    {
        int i;
        for (i = 0; i < prog->string_count; i++) {
            IrString *s = prog->strings[i];
#if ARKCC_FEATURE_WCHAR
            if (s->is_wide) {
                /* B2: wide strings: emit a wchar_t array using L"..." literal.
                 * gcc will compile to the platform's native wchar_t encoding. */
                xh_c_rule_ints(&buf, "module_ir_cmp_jmp_eq_value_3", s->id, 0);
                emit_str_literal(&buf, s->data, s->len);
                asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_38"));
            } else
#endif
            {
                xh_c_rule_ints(&buf, "module_ir_cmp_jmp_eq_value_4", s->id, 0);
                emit_str_literal(&buf, s->data, s->len);
                asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_38"));
            }
        }
        if (prog->string_count) asmbuf_putc(&buf, '\n');
    }

    /* Function prototypes (must precede global variables so that
     * initializers referencing function symbols, e.g.
     *   static const Backend g_arm_backend = { ..., arm_emit, ... };
     * compile cleanly: the function is already declared). */
    {
        int i;
        for (i = 0; i < prog->func_count; i++) emit_func_proto(&buf, prog->funcs[i]);
        asmbuf_putc(&buf, '\n');
    }

    /* Global variables */
    {
        int i;
        for (i = 0; i < prog->global_count; i++) {
            IrGlobal *g = prog->globals[i];
            int is_aggregate = (g->init_aggregate != 0) ||
                               c_type_is_aggregate(g->type);
            /* Skip host libc variables (stdin/stdout/stderr/errno/FE_DFL_ENV):
             * the C backend would emit extern, but the host already defines them. */
            if (g->is_extern && is_libc_var(g->name)) continue;
            /* Storage class */
            if (g->is_extern) asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_41"));
            else if (g->is_static) asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_42"));
#if ARKCC_FEATURE_ARMCC_KEYWORDS
            /* __thread: thread-local storage, supported by gcc/libpthread on host. */
            if (g->is_tls && !g->is_extern) asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_43"));
#endif
            /* const qualifier forces gcc to place the variable in .rodata.
             * F2: if the type already has is_const (from type_qualified in parser),
             * emit_c_type will emit it; avoid double-const here. */
            if (g->is_const && !g->is_extern && !(g->type && g->type->is_const)) asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_37"));
            /* Type and declarator; pointer/array syntax recurses with '*' */
            emit_c_type(&buf, g->type, g->name);
            /* Alignment via __attribute__((aligned(n))) */
            if (g->align_bytes > 0 && !g->is_extern) {
                xh_c_rule_ints(&buf, "module_ir_cmp_jmp_eq_value_5", g->align_bytes, 0);
            }
            /* used/unused attributes for gcc. */
            if (g->is_used && !g->is_extern)   asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_44"));
            if (g->is_unused && !g->is_extern) asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_45"));
            /* __at(addr): unsupported on host; emit as a comment. */
            if (g->has_at) {
                asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_46"));
                {
                    unsigned a = g->at_addr;
                    char hex[9]; int k;
                    const char *hd = xh_c_rule_text("escape_hex_digits");
                    for (k = 7; k >= 0; k--) { hex[k] = hd[a & 0xF]; a >>= 4; }
                    hex[8] = 0;
                    asmbuf_puts(&buf, hex);
                }
                asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_47"));
            }
#if ARKCC_FEATURE_ARMCC_KEYWORDS
            /* __global_reg(n): unsupported on host; emit as a comment. */
            if (g->global_reg >= 0) {
                xh_c_rule_ints(&buf, "abi_tok_ge_value_2", g->global_reg, 0);
            }
#endif
            /* Section attribute: __attribute__((section("..."))) */
            if (g->section && !g->is_extern) {
                asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_48"));
                asmbuf_puts(&buf, g->section);
                asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_49"));
            }
            if (!g->is_extern) {
                if (g->has_init) {
                    asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_50"));
                    if (is_aggregate && g->init_aggregate) {
                        asmbuf_puts(&buf, g->init_aggregate);
                    } else if (g->is_float) {
                        /* 自定义 double 转字符串，与 irgen.c 的 sb_putf 保持一致：
                         * 大数（v >= 1e16）和极小值（v < 1e-4）使用科学记数法，
                         * 正常范围使用定点表示，保证输出为合法 C 浮点字面量。 */
                        char fb[80]; int fn=0; double v = g->init_float;
                        if (v != v) {
                            asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_51"));
                        } else if (v - v != 0.0) {
                            /* 无穷大检测：inf - inf = NaN，NaN != 0.0。
                             * 不能用 v > DBL_MAX 比较，因为 -O2 下 x87 FPU
                             * 的 80 位扩展精度可能导致误判（DBL_MAX 被误判为 >DBL_MAX）。 */
                            asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_52"));
                        } else {
                            if (v < 0) { fb[fn++] = '-'; v = -v; }
                            if (v == 0.0) {
                                fb[fn++] = '0'; fb[fn++] = '.'; fb[fn++] = '0';
                            } else if (v >= 1e16 || v < 1e-4) {
                                /* 大数或极小值使用科学记数法 */
                                int ex = 0, i, d;
                                /* 大步长归一化，减少迭代次数以降低累积舍入误差 */
                                {
                                    static const double p10[] = {1e1, 1e2, 1e4, 1e8, 1e16, 1e32, 1e64, 1e128, 1e256};
                                    static const int sh[] = {1, 2, 4, 8, 16, 32, 64, 128, 256};
                                    int bi;
                                    for (bi = 8; bi >= 0; bi--) {
                                        while (v >= p10[bi]) { v /= p10[bi]; ex += sh[bi]; }
                                    }
                                    while (v >= 10.0) { v /= 10.0; ex++; }
                                    for (bi = 8; bi >= 0; bi--) {
                                        while (v < 1.0 && v * p10[bi] < 10.0) { v *= p10[bi]; ex -= sh[bi]; }
                                    }
                                    while (v < 1.0) { v *= 10.0; ex--; }
                                }
                                d = (int)v; if (d > 9) d = 9;
                                fb[fn++] = (char)('0' + d);
                                v -= (double)d;
                                fb[fn++] = '.';
                                for (i = 0; i < 17; i++) {
                                    v *= 10.0; d = (int)v;
                                    if (d > 9) d = 9; if (d < 0) d = 0;
                                    fb[fn++] = (char)('0' + d);
                                    v -= (double)d;
                                }
                                while (fn > 0 && fb[fn-1] == '0') fn--;
                                if (fb[fn-1] == '.') fb[fn++] = '0';
                                fb[fn++] = 'e';
                                if (ex >= 0) fb[fn++] = '+';
                                else { fb[fn++] = '-'; ex = -ex; }
                                if (ex < 10) { fb[fn++] = '0'; fb[fn++] = (char)('0' + ex); }
                                else {
                                    char eb[8]; int k = 0;
                                    while (ex > 0) { eb[k++] = (char)('0' + ex % 10); ex /= 10; }
                                    while (k > 0) fb[fn++] = eb[--k];
                                }
                            } else {
                                /* 定点表示：1e-4 <= v < 1e16 */
                                long long ip = (long long)v; char tmp[24]; int tn=0; int kk;
                                if (ip == 0) fb[fn++]='0';
                                else {
                                    while (ip>0) { tmp[tn++]=(char)('0'+ip%10); ip/=10; }
                                    for (kk = tn - 1; kk >= 0; kk--) fb[fn++] = tmp[kk];
                                }
                                fb[fn++] = '.';
                                {
                                    /* 17 位小数保证 double 往返精度 */
                                    double f = v - (double)((long long)v);
                                    int kk2;
                                    for (kk2 = 0; kk2 < 17 && f > 0; kk2++) {
                                        int d; f *= 10; d = (int)f;
                                        if (d > 9) d = 9;
                                        fb[fn++] = (char)('0' + d);
                                        f -= d;
                                    }
                                }
                                while (fn > 0 && fb[fn-1] == '0') fn--;
                                if (fb[fn-1] == '.') fb[fn++] = '0';
                            }
                            fb[fn] = 0;
                            asmbuf_puts(&buf, fb);
                        }
                    } else if (g->init_str) {
                        /* char a[] = "..." */
                        emit_str_literal(&buf, g->init_str, ark_strlen(g->init_str));
                    } else {
                        asmbuf_int(&buf, g->init_int);
                    }
                }
            }
            asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_38"));
        }
        if (prog->global_count) asmbuf_putc(&buf, '\n');
    }


    /* Forward declarations for functions referenced by IR_CALL but not defined
     * in this TU (neither user-defined nor host libc). Emit them as extern
     * prototypes so the .gen.c file compiles cleanly with gcc. */
    {
        int ffi, ii, jj, emitted = 0;
        const char *seen[128];  /* Dedup list of emitted extern prototypes */
        for (ffi = 0; ffi < prog->func_count; ffi++) {
            IrFunc *f = prog->funcs[ffi];
            for (ii = 0; ii < f->code_count; ii++) {
                IrInst *I = &f->code[ii];
                if ((I->op != IR_CALL && I->op != IR_PUSH_FUNC) || !I->name) continue;
                if (I->name[0] == '?') continue;  /* indirect call placeholder */
                if (is_libc_func(I->name)) continue;
                if (is_wrapped_func(I->name)) continue;
                /* Defined in this TU? skip; emit_func_proto already emitted it. */
                {
                    int defined = 0;
                    for (jj = 0; jj < prog->func_count; jj++) {
                        if (ark_streq(prog->funcs[jj]->name, I->name)) { defined = 1; break; }
                    }
                    if (defined) continue;
                }
                /* Dedup */
                {
                    int dup = 0, kk;
                    for (kk = 0; kk < emitted; kk++) if (ark_streq(seen[kk], I->name)) { dup = 1; break; }
                    if (dup) continue;
                }
                if (emitted < 128) seen[emitted++] = I->name;
                emit_extern_proto(&buf, I->name);
            }
        }
        if (emitted) asmbuf_putc(&buf, '\n');
    }

    /* Function bodies */
    for (fi = 0; fi < prog->func_count; fi++) {
        IrFunc *f = prog->funcs[fi];
        int i;
        int param_idx;  /* C 后端参数索引（每个参数占 8 字节，不同于 x86 的 4 字节 slot） */
        if (ark_streq(f->name, xh_c_rule_text("module_ir_cmp_jmp_eq_53"))) main_idx = fi;

        /* alias target: __attribute__((alias(...))) handles body generation. */
        if (f->alias_target) continue;

        if (f->is_weak)   asmbuf_puts(&buf, xh_c_rule_text("attribute_tok_ge_4"));
        if (f->is_naked || f->is_asm_func) asmbuf_puts(&buf, xh_c_rule_text("attribute_tok_ge_5"));
        if (f->is_used)   asmbuf_puts(&buf, xh_c_rule_text("attribute_tok_ge_6"));
        if (f->is_unused) asmbuf_puts(&buf, xh_c_rule_text("attribute_tok_ge_7"));
        if (f->is_irq)    asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_54"));
        if (f->is_constructor) asmbuf_puts(&buf, xh_c_rule_text("attribute_tok_ge_9"));
        if (f->is_destructor)  asmbuf_puts(&buf, xh_c_rule_text("attribute_tok_ge_10"));
        if (f->is_deprecated)  asmbuf_puts(&buf, xh_c_rule_text("attribute_tok_ge_11"));
#if ARKCC_FEATURE_ARMCC_KEYWORDS
        if (f->is_pure)       asmbuf_puts(&buf, xh_c_rule_text("attribute_tok_ge_12"));
        if (f->is_const_attr) asmbuf_puts(&buf, xh_c_rule_text("attribute_tok_ge_13"));
        if (f->is_value_in_regs) asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_55"));
        if (f->is_interwork)     asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_56"));
        if (f->smc_num > 0) {
            xh_c_rule_ints(&buf, "module_ir_cmp_jmp_eq_value_6", f->smc_num, 0);
        }
#endif
        /* --split_sections/-ffunction-sections: per-function section attribute for gcc. */
        if (g_config.split_sections) {
            asmbuf_puts(&buf, xh_c_rule_text("attribute_ir_cmp_jmp_eq_57"));
            asmbuf_puts(&buf, f->name);
            asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_15"));
        }
        asmbuf_puts(&buf, f->is_static ? xh_c_rule_text("abi_tok_ge_16") : xh_c_rule_text("abi_tok_ge_17"));
        asmbuf_puts(&buf, f->name);
        asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_18"));
        for (i = 0; i < MAX_CALL_ARGS; i++) {
            if (i) asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_19"));
            asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_20"));
            asmbuf_int(&buf, i);
        }
        asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_58"));
        if (f->is_asm_func) {
            /* __asm function body: emit the asm text as a single __asm__ block.
             * naked attribute tells gcc to omit prologue/epilogue, so we never
             * emit a return statement (naked functions require no return). */
            asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_59"));
            {
                const char *s = f->asm_body ? f->asm_body : xh_c_rule_text("type_ptr_4");
                int sl = ark_strlen(s);
                /* Convert each asm line into a C string literal. */
                int k;
                int line_start = 1;
                for (k = 0; k <= sl; k++) {
                    char c = (k < sl) ? s[k] : 0;
                    if (line_start) {
                        if (c == ' ' || c == '\t' || c == '\r') continue;
                        if (c == '\n' || c == 0) continue;
                        asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_60"));
                        line_start = 0;
                    }
                    if (c == '\n' || c == 0) {
                        asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_61"));
                        line_start = 1;
                        if (c == 0) break;
                        continue;
                    }
                    if (c == '"') asmbuf_puts(&buf, xh_c_rule_text("escape_quote"));
                    else if (c == '\\') asmbuf_puts(&buf, xh_c_rule_text("escape_backslash"));
                    else asmbuf_putc(&buf, c);
                }
            }
            asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_62"));
            continue;
        }
        asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_63"));
        asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_64"));
        asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_65"));
        {
            int frame_sz = compute_frame_size(f);
            int stk_sz = compute_max_sp(f);
            /* Add safety margin to stk to account for any simulation imprecision */
            stk_sz += xh_c_abi()->stack_margin;
            if (stk_sz < xh_c_abi()->stack_min) stk_sz = xh_c_abi()->stack_min;
            xh_c_rule_ints(&buf, "module_ir_cmp_jmp_eq_value_7", frame_sz, 0);
            xh_c_rule_ints(&buf, "module_ir_cmp_jmp_eq_value_8", stk_sz, 0);
        }
        asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_66"));
        asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_67"));
        asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_68"));
        /* For variadic functions, expose _ark_va_ptr pointing to the first
         * variadic argument stored at frame+8000. stdarg.h's va_start uses
         * this directly. param_count gives the number of named params. */
        if (f->is_variadic) {
            xh_c_rule_ints(&buf, "module_ir_cmp_jmp_eq_value_9", f->param_count, xh_c_abi()->varargs_offset);
        }

        /* C 后端函数签名为 ark_<name>(long long a0, ..., long long a15)，
         * 每个参数占 8 字节。irgen.c 的 aux 低 8 位是 x86 cdecl 的 4 字节 slot
         * 索引（64 位参数占 2 个 slot），与 C 后端的参数索引不一致。
         * 因此用 param_idx 追踪真实参数顺序（0, 1, 2, ...）。 */
        param_idx = 0;
        for (i = 0; i < f->code_count; i++) {
            IrInst *I = &f->code[i];
            asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_69"));
            switch (I->op) {
                case IR_FUNC_BEGIN:
                case IR_FUNC_END:
                    asmbuf_puts(&buf, xh_c_rule_text("ir_func_end"));
                    break;
                case IR_GETARG: {
                    /* aux 低 8 位 = x86 cdecl 的 4 字节 slot 索引；
                     * aux>>8 = 参数类型大小（字节数）。
                     * C 后端函数签名为 long long a0..a15，每个参数 8 字节。
                     * 用 param_idx 追踪真实参数顺序（而非 x86 slot 索引）。
                     * - st_sz > 8：struct 按值传递，a<param_idx> 是指向源 struct
                     *   的指针（由调用者 irgen_lvalue_addr 压入），用 memcpy 拷贝。
                     * - st_sz <= 8：标量/指针/小 struct，直接存储 a<param_idx>。
                     * - val >= 8000：变参区域，aux 低 8 位直接是 a0..a7 索引。 */
                    int arg_idx = I->aux & 0xFF;
                    int st_sz   = (I->aux >> 8) & 0xFFFFFF;
                    if (I->val >= xh_c_abi()->varargs_offset) {
                        /* 变参区域：a<arg_idx> 直接存储到 frame[val] */
                        xh_c_rule_ints(&buf, "ir_getarg_value", I->val, 0);
                        asmbuf_int(&buf, arg_idx);
                        asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_21"));
                    } else if (st_sz > xh_c_abi()->slot_bytes) {
                        /* struct 按值传递（>8 字节）：a<param_idx> 是源指针 */
                        xh_c_rule_ints(&buf, "ir_getarg_value_2", I->val, 0);
                        asmbuf_int(&buf, param_idx);
                        xh_c_rule_ints(&buf, "ir_getarg_value_3", st_sz, 0);
                        param_idx++;
                    } else {
                        /* 标量/指针/小 struct（<=8 字节）：直接存储 */
                        xh_c_rule_ints(&buf, "ir_getarg_value", I->val, 0);
                        asmbuf_int(&buf, param_idx);
                        asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_21"));
                        param_idx++;
                    }
                    break;
                }
                case IR_PUSH_INT:
                    xh_c_rule_ints(&buf, "ir_push_int_value", I->val, 0);
                    break;
                case IR_PUSH_FLOAT:
                    if (I->aux & IR_AUX_COMPLEX) {
                        /* Complex constant: aux 0x800|0x200 = complex(0x800) +
                         * double(0x200); val = index into prog->complexes[]. */
                        xh_c_rule_ints(&buf, "ir_push_float_value", I->val, 0);
                    } else {
                        xh_c_rule_ints(&buf, "ir_push_float_value_2", I->val, 0);
                    }
                    break;
                case IR_PUSH_STR: {
                    /* B2: wide strings reference ark_wstr_<id>; narrow strings use ark_str_<id>. */
                    int is_w = 0;
#if ARKCC_FEATURE_WCHAR
                    if (I->val >= 0 && I->val < prog->string_count &&
                        prog->strings[I->val]->is_wide) is_w = 1;
#endif
                    asmbuf_puts(&buf, xh_c_rule_text("ir_push_str"));
                    asmbuf_puts(&buf, is_w ? xh_c_rule_text("ir_push_str_2") : xh_c_rule_text("ir_push_str_3"));
                    asmbuf_int(&buf, I->val);
                    asmbuf_putc(&buf, ';');
                    break;
                }
                case IR_ALLOCA:
                    /* B3: VLA allocation; replace top-of-stack with alloca(size) pointer. */
                    asmbuf_puts(&buf, xh_c_rule_text("ir_alloca"));
                    break;
                case IR_PUSH_VAR: {
                    /* aux encodes load size and sign:
                     * Low 8 bits = element size (1/2/4/8/16); bit8 (0x100) = unsigned.
                     * For signed integers, sign-extend into a long long slot to
                     * preserve negative values when re-stored as a narrower type.
                     * Sign-extending before comparison avoids mixing 0xFFFFFFFF
                     * (unsigned) with -1 (signed) semantics.
                     * sz==16 means _Complex double; use the .c member to push. */
                    int sz = I->aux & 0xFF;
                    int is_unsigned = I->aux & 0x100;
                    if (sz == 0) sz = 8;
                    if (sz == 16) {
                        xh_c_rule_ints(&buf, "ir_push_var_value", I->val, 0);
                        break;
                    }
                    asmbuf_puts(&buf, xh_c_rule_text("ir_push_var"));
                    asmbuf_int(&buf, I->val);
                    if (sz == 1)
                        asmbuf_puts(&buf, is_unsigned ? xh_c_rule_text("ir_push_var_2") : xh_c_rule_text("ir_push_var_3"));
                    else if (sz == 2)
                        asmbuf_puts(&buf, is_unsigned ? xh_c_rule_text("ir_push_var_4") : xh_c_rule_text("ir_push_var_5"));
                    else if (sz == 4)
                        asmbuf_puts(&buf, is_unsigned ? xh_c_rule_text("ir_push_var_6") : xh_c_rule_text("ir_push_var_7"));
                    else
                        asmbuf_puts(&buf, xh_c_rule_text("ir_push_var_8"));
                    break;
                }
                case IR_PUSH_ADDR:
                    xh_c_rule_ints(&buf, "ir_push_addr_value", I->val, 0);
                    break;
                case IR_PUSH_GLOBAL: {
                    /* Look up the global by name to determine its category
                     * (aggregate vs. scalar vs. float/double). Aggregates use
                     * address-of to push a pointer; floats push via .f member
                     * to avoid bit-pattern loss via (long long) cast.
                     * We scan prog->globals each time because the IR does not
                     * tag globals by category. */
                    int is_agg = 0;
                    int is_flt = 0;
                    int gi;
                    for (gi = 0; gi < prog->global_count; gi++) {
                        if (prog->globals[gi]->name &&
                            ark_streq(prog->globals[gi]->name, I->name)) {
                            Type *gt = prog->globals[gi]->type;
                            is_agg = c_type_is_aggregate(gt);
                            is_flt = gt && (gt->kind == TYPE_FLOAT || gt->kind == TYPE_DOUBLE);
                            break;
                        }
                    }
                    if (is_agg) {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_push_global"));
                        asmbuf_puts(&buf, I->name);
                        asmbuf_putc(&buf, ';');
                    } else if (is_flt) {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_push_global_2"));
                        asmbuf_puts(&buf, I->name);
                        asmbuf_putc(&buf, ';');
                    } else {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_push_global_3"));
                        asmbuf_puts(&buf, I->name);
                        asmbuf_putc(&buf, ';');
                    }
                    break;
                }
                case IR_TRUNC: {
                    /* val = target width, aux = 1 zero-extend (unsigned). */
                    const char *cty = xh_c_rule_text("ir_trunc");
                    if (I->val == 8)       cty = I->aux ? xh_c_rule_text("ir_trunc_2") : xh_c_rule_text("ir_trunc_3");
                    else if (I->val == 16) cty = I->aux ? xh_c_rule_text("ir_trunc_4") : xh_c_rule_text("ir_trunc_5");
                    else                    cty = I->aux ? xh_c_rule_text("ir_trunc_6") : xh_c_rule_text("ir_trunc");
                    asmbuf_puts(&buf, xh_c_rule_text("ir_trunc_7"));
                    asmbuf_puts(&buf, cty);
                    asmbuf_puts(&buf, xh_c_rule_text("ir_trunc_8"));
                    break;
                }
                case IR_PUSH_FUNC:
                    /* Function designator in value position: push the address
                     * of the generated C function. Host libc entries have no
                     * ark_ wrapper (same dispatch rule as IR_CALL), so their
                     * address is the host symbol itself. */
                    if (is_libc_func(I->name)) {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_push_func"));
                        asmbuf_puts(&buf, I->name);
                    } else {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_push_func_2"));
                        asmbuf_puts(&buf, I->name);
                    }
                    asmbuf_putc(&buf, ';');
                    break;
                case IR_PUSH_GLOBAL_ADDR:
                    /* Always push the address of the global (regardless of category). */
                    asmbuf_puts(&buf, xh_c_rule_text("ir_push_global"));
                    asmbuf_puts(&buf, I->name);
                    asmbuf_putc(&buf, ';');
                    break;
                case IR_STORE_GLOBAL:
                    asmbuf_puts(&buf, I->name);
                    asmbuf_puts(&buf, xh_c_rule_text("ir_store_global"));
                    break;
                case IR_POP:
                    asmbuf_puts(&buf, xh_c_rule_text("ir_pop"));
                    break;
                case IR_DUP:
                    asmbuf_puts(&buf, xh_c_rule_text("ir_dup"));
                    break;
                case IR_SWAP:
                    asmbuf_puts(&buf, xh_c_rule_text("ir_swap"));
                    break;
                case IR_SEXT:
                case IR_ZEXT:
                case IR_CDECL64:
                    /* No-op in C backend: virtual stack uses 8-byte long long
                     * slots, so int->long long promotion and 64-bit word-order
                     * conversion are unnecessary (each value occupies one slot). */
                    break;
                case IR_STORE: {
                    /* aux encodes the store size: 1/2/4/8/16. Narrow values
                     * are stored through the corresponding pointer type so that
                     * the high bits of an int are not written past the slot.
                     * sz==16 means _Complex double; store via the .c member.
                     * N1 FIX: mask off IR_AUX_VOLATILE (0x10000) bit. */
                    int sz = I->aux & ~IR_AUX_VOLATILE;
                    if (sz == 0) sz = 8;
                    if (sz == 16) {
                        xh_c_rule_ints(&buf, "ir_store_value", I->val, 0);
                    } else {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_store"));
                        asmbuf_int(&buf, I->val);
                        if (sz == 1)      asmbuf_puts(&buf, xh_c_rule_text("ir_store_2"));
                        else if (sz == 2) asmbuf_puts(&buf, xh_c_rule_text("ir_store_3"));
                        else if (sz == 4) asmbuf_puts(&buf, xh_c_rule_text("ir_store_4"));
                        else              asmbuf_puts(&buf, xh_c_rule_text("ir_store_5"));
                    }
                    break;
                }
                case IR_LOAD_IDX: {
                    /* Bug-I5 FIX: When IR_AUX_VOLATILE is set, emit volatile-qualified
                     * loads so the host C compiler does not merge or eliminate the
                     * read-modify-write of volatile lvalues. */
                    int is_vol = (I->aux & IR_AUX_VOLATILE) != 0;
                    const char *vol = is_vol ? xh_c_rule_text("abi_tok_ge_36") : xh_c_rule_text("type_ptr_4");
                    xh_c_rule_ints(&buf, "ir_load_idx_value", I->val, 0);
                    if (is_vol) asmbuf_puts(&buf, vol);
                    asmbuf_puts(&buf, xh_c_rule_text("ir_load_idx"));
                    asmbuf_puts(&buf, xh_c_rule_text("ir_load_idx_2"));
                    if (is_vol) asmbuf_puts(&buf, vol);
                    asmbuf_puts(&buf, xh_c_rule_text("ir_load_idx_3"));
                    asmbuf_puts(&buf, xh_c_rule_text("ir_load_idx_4"));
                    if (is_vol) asmbuf_puts(&buf, vol);
                    asmbuf_puts(&buf, xh_c_rule_text("ir_load_idx_5"));
                    asmbuf_puts(&buf, xh_c_rule_text("ir_load_idx_6"));
                    if (is_vol) asmbuf_puts(&buf, vol);
                    asmbuf_puts(&buf, xh_c_rule_text("ir_load_idx_7"));
                    break;
                }
                case IR_STORE_IDX: {
                    /* Bug-I5 FIX: When IR_AUX_VOLATILE is set, emit volatile-qualified
                     * stores so the host C compiler preserves the write. */
                    int is_vol = (I->aux & IR_AUX_VOLATILE) != 0;
                    const char *vol = is_vol ? xh_c_rule_text("abi_tok_ge_36") : xh_c_rule_text("type_ptr_4");
                    xh_c_rule_ints(&buf, "ir_store_idx_value", I->val, 0);
                    if (is_vol) asmbuf_puts(&buf, vol);
                    asmbuf_puts(&buf, xh_c_rule_text("ir_store_idx"));
                    asmbuf_puts(&buf, xh_c_rule_text("ir_store_idx_2"));
                    if (is_vol) asmbuf_puts(&buf, vol);
                    asmbuf_puts(&buf, xh_c_rule_text("ir_store_idx_3"));
                    asmbuf_puts(&buf, xh_c_rule_text("ir_store_idx_4"));
                    if (is_vol) asmbuf_puts(&buf, vol);
                    asmbuf_puts(&buf, xh_c_rule_text("ir_store_idx_5"));
                    asmbuf_puts(&buf, xh_c_rule_text("ir_store_idx_6"));
                    if (is_vol) asmbuf_puts(&buf, vol);
                    asmbuf_puts(&buf, xh_c_rule_text("ir_store_idx_7"));
                    asmbuf_puts(&buf, xh_c_rule_text("ir_store_idx_8"));
                    break;
                }
                case IR_BINARY:
                    asmbuf_puts(&buf, xh_c_rule_text("ir_binary"));
                    emit_binop(&buf, I->aux, 0);
                    asmbuf_putc(&buf, ';');
                    break;
                case IR_FADD:
                    if ((I->aux & 0xF00) == 0x900) {
                        /* cexp intrinsic: aux = TOK_PLUS | 0x900 (complex bit
                         * 0x800 plus the 0x100 sub-op selector). */
                        asmbuf_puts(&buf, xh_c_rule_text("ir_fadd"));
                    } else if (I->aux & IR_AUX_COMPLEX) {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_fadd_2"));
                    } else {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_fadd_3"));
                    }
                    break;
                case IR_FSUB:
                    if (I->aux & IR_AUX_COMPLEX) {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_fsub"));
                    } else {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_fsub_2"));
                    }
                    break;
                case IR_FMUL:
                    if (I->aux & IR_AUX_COMPLEX) {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_fmul"));
                    } else {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_fmul_2"));
                    }
                    break;
                case IR_FDIV:
                    /* Float division follows IEEE754: x/0.0 = +/-inf or NaN.
                     * The result feeds isnan/isinf classification paths; we keep
                     * the division at IR_FDIV rather than IR_BINARY+emit_binop. */
                    if (I->aux & IR_AUX_COMPLEX) {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_fdiv"));
                    } else {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_fdiv_2"));
                    }
                    break;
                case IR_FCMP: {
                    /* Mask out the aux double bit (0x200) and pick the comparison op.
                     * C99 floats use the same comparison regardless of width. */
                    int op = I->aux & ~(0x200 | IR_AUX_COMPLEX);
                    const char *cop = xh_c_rule_text("ir_fcmp");
                    switch (op) {
                        case (int)TOK_EQ: cop = xh_c_rule_text("ir_fcmp"); break;
                        case (int)TOK_NE: cop = xh_c_rule_text("tok_ne_2"); break;
                        case (int)TOK_LT: cop = xh_c_rule_text("tok_lt_3");  break;
                        case (int)TOK_GT: cop = xh_c_rule_text("tok_gt_3");  break;
                        case (int)TOK_LE: cop = xh_c_rule_text("tok_le_3"); break;
                        case (int)TOK_GE: cop = xh_c_rule_text("abi_tok_ge_38"); break;
                        default: xh_c_rules_error("unsupported C comparison operation"); break;
                    }
                    if (I->aux & IR_AUX_COMPLEX) {
                        /* C99 6.5.9: the only comparison operators defined for
                         * complex operands are == and !=, and they compare both
                         * parts (NaN propagating exactly like the real case). */
                        if (op == (int)TOK_NE) {
                            asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_39"));
                        } else {
                            asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_40"));
                        }
                        break;
                    }
                    asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_41"));
                    asmbuf_puts(&buf, cop);
                    asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_42"));
                    break;
                }
                case IR_F2I:
                    if (I->aux & IR_AUX_COMPLEX) {
                        /* Complex -> double extractions. aux & 0x300 selects:
                         *   0x000 creal   (real part)
                         *   0x100 cimag   (imaginary part)
                         *   0x200 cabs    (needs libm)
                         *   0x300 carg    (needs libm) */
                        int k = I->aux & 0x300;
                        if (k == 0x200)
                            asmbuf_puts(&buf, xh_c_rule_text("ir_f2i"));
                        else if (k == 0x300)
                            asmbuf_puts(&buf, xh_c_rule_text("ir_f2i_2"));
                        else if (k == 0x100)
                            asmbuf_puts(&buf, xh_c_rule_text("ir_f2i_3"));
                        else
                            asmbuf_puts(&buf, xh_c_rule_text("ir_f2i_4"));
                    } else {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_f2i_5"));
                    }
                    break;
                case IR_I2F:
                    if (I->aux & IR_AUX_COMPLEX) {
                        /* Real -> complex widening (imaginary part 0). aux & 0x700
                         * selects the source representation:
                         *   0x000 float       0x200 double
                         *   0x100 int   (0x400 more if unsigned)
                         * The C backend stores every real in the .f double member,
                         * so float and double sources are the same operation. */
                        int k = I->aux & 0x700;
                        if (k == 0x100 || k == 0x500) {
                            if (k == 0x500)
                                asmbuf_puts(&buf, xh_c_rule_text("ir_i2f"));
                            else
                                asmbuf_puts(&buf, xh_c_rule_text("ir_i2f_2"));
                        } else {
                            asmbuf_puts(&buf, xh_c_rule_text("ir_i2f_3"));
                        }
                    } else {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_i2f_4"));
                    }
                    break;
                case IR_64I2F:
                    /* long long -> double/float. On the C backend all values are
                     * 8-byte slots, so this is a numeric cast (not bit reinterpret).
                     * 0x400 = unsigned: cast through (unsigned long long) first. */
                    if (I->aux & 0x400) {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_64i2f"));
                    } else {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_i2f_4"));
                    }
                    break;
                case IR_64F2I:
                    /* double -> long long. Numeric cast, not bit reinterpret.
                     * 0x400 = unsigned: cast to (unsigned long long) then store. */
                    if (I->aux & 0x400) {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_64f2i"));
                    } else {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_f2i_5"));
                    }
                    break;
                case IR_F2F:
                    /* float <-> float cast. In the C backend the union stores
                     * all floats as double, so float->double is a no-op and
                     * double->float truncates to float precision (then
                     * promotes back to double). Stack effect: 0. */
                    if (I->aux & 0x200) {
                        /* double -> float */
                        asmbuf_puts(&buf, xh_c_rule_text("ir_f2f"));
                    }
                    /* float -> double: no-op in C backend */
                    break;
                case IR_ASM:
                    /* '@' prefix indicates extended asm form: `"text" : outputs : inputs : clobbers`.
                     * Otherwise we emit a basic asm string literal; gcc accepts both forms.
                     * Note: IR_ASM is always volatile; the parser has already validated syntax. */
                    if (I->name && I->name[0] == '@') {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_asm"));
                        asmbuf_puts(&buf, I->name + 1);
                        asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_21"));
                    } else {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_asm"));
                        {
                            const char *s = I->name ? I->name : xh_c_rule_text("type_ptr_4");
                            emit_str_literal(&buf, s, ark_strlen(s));
                        }
                        asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_21"));
                    }
                    break;
                case IR_UNARY: {
                    int op = I->aux & ~IR_AUX_COMPLEX;
                    int is_float = (I->val == 1);
                    if (I->aux & IR_AUX_COMPLEX) {
                        /* Complex unary conj: 1 complex in -> 1 complex out. */
                        asmbuf_puts(&buf, xh_c_rule_text("ir_unary"));
                    } else if (is_float) {
                        /* Float negation: respects IEEE754 sign bit (handles -0.0/NaN).
                         * Uses the stk[].f member to preserve the sign bit semantics. */
                        asmbuf_puts(&buf, xh_c_rule_text("ir_unary_2"));
                    } else {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_unary_3"));
                        if (op == (int)TOK_MINUS) asmbuf_puts(&buf, xh_c_rule_text("ir_unary_4"));
                        else if (op == (int)TOK_BNOT) asmbuf_puts(&buf, xh_c_rule_text("ir_unary_5"));
                        else if (op == (int)TOK_NOT)  asmbuf_puts(&buf, xh_c_rule_text("ir_unary_6"));
                        else xh_c_rules_error("unsupported C unary operation");
                        asmbuf_putc(&buf, ';');
                    }
                    break;
                }
                case IR_LABEL:
                    xh_c_rule_ints(&buf, "ir_label_value", I->val, 0);
                    break;
                case IR_LABEL_NAMED:
                    asmbuf_puts(&buf, xh_c_rule_text("ir_label_named"));
                    asmbuf_puts(&buf, I->name);
                    asmbuf_puts(&buf, xh_c_rule_text("ir_label_named_2"));
                    break;
                case IR_JMP:
                    asmbuf_puts(&buf, xh_c_rule_text("ir_jmp"));
                    asmbuf_int(&buf, I->val);
                    asmbuf_putc(&buf, ';');
                    break;
                case IR_JMP_NAMED:
                    asmbuf_puts(&buf, xh_c_rule_text("ir_jmp_named"));
                    asmbuf_puts(&buf, I->name);
                    asmbuf_putc(&buf, ';');
                    break;
                case IR_JZ:
                    asmbuf_puts(&buf, xh_c_rule_text("ir_jz"));
                    asmbuf_int(&buf, I->val);
                    asmbuf_putc(&buf, ';');
                    break;
                case IR_JNZ:
                    asmbuf_puts(&buf, xh_c_rule_text("ir_jnz"));
                    asmbuf_int(&buf, I->val);
                    asmbuf_putc(&buf, ';');
                    break;
                case IR_CMP_JMP_EQ:
                    xh_c_rule_ints(&buf, "module_ir_cmp_jmp_eq_value_10", I->val, 0);
                    asmbuf_int(&buf, I->aux);
                    asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_70"));
                    break;
                case IR_CALL: {
                    int n = I->val;
                    int k;
                    /* Pop n args off the stack into aa[] (in call order arg0..argN-1).
                     * aa[] is volatile so gcc cannot constant-fold away the stores.
                     * Without volatile, gcc -O2 would erase the union(ark_slot) write
                     * pattern, breaking printf's variadic ABI. The default values are
                     * 0 so unused slots read as NULL (avoids %s printing "(null)"). */
                    asmbuf_puts(&buf, xh_c_rule_text("ir_call"));
                    if (n > MAX_CALL_ARGS) n = MAX_CALL_ARGS;
                    if (is_complex_ret_func(I->name)) {
                        /* Complex-function args are _Complex double (16 bytes, .c member);
                         * popping via .i reads only 8 bytes. Use a separate _Complex array. */
                        asmbuf_puts(&buf, xh_c_rule_text("ir_call_2"));
                        for (k = 0; k < n; k++) {
                            xh_c_rule_ints(&buf, "ir_call_value", k, 0);
                        }
                    } else {
                        for (k = 0; k < n; k++) {
                            xh_c_rule_ints(&buf, "ir_call_value_2", k, 0);
                        }
                    }
                    /* Dispatch to libc if the function is host libc; otherwise ark_ prefix. */
                    if (ark_streq(I->name, xh_c_rule_text("ir_call_3"))) {
                        /* printf variadic ABI: arkcc stores all args as long long in aa[].
                         * aux encodes per-argument type using 2 bits per arg:
                         *   bit(2*i)   = 1 => float/double (promoted to double, XMM/8B)
                         *   bit(2*i+1) = 1 => long long (8 bytes on the wire)
                         *   both 0     => int/ptr/short/char (int-promoted, 4 bytes)
                         * On 32-bit hosts va_arg(int) reads 4 bytes and va_arg(long long)
                         * reads 8 bytes; passing everything as long long shifts subsequent
                         * arguments and produces garbage. Casting to `int` for int-typed
                         * args restores the proper stack layout. */
                        int amask = I->aux;
                        asmbuf_puts(&buf, xh_c_rule_text("ir_call_4"));
                        for (k = 1; k < MAX_CALL_ARGS; k++) {
                            int is_double = (amask >> (2 * k)) & 1;
                            int is_llong  = (amask >> (2 * k + 1)) & 1;
                            if (k > 1) asmbuf_putc(&buf, ',');
                            if (is_double) {
                                xh_c_rule_ints(&buf, "ir_call_value_3", k, 0);
                            } else if (is_llong) {
                                xh_c_rule_ints(&buf, "ir_call_value_4", k, 0);
                            } else {
                                xh_c_rule_ints(&buf, "ir_call_value_5", k, 0);
                            }
                        }
                        asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_21"));
                    } else if (ark_streq(I->name, xh_c_rule_text("ir_call_5"))) {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_call_6"));
                    } else if (ark_streq(I->name, xh_c_rule_text("ir_call_7"))) {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_call_8"));
                    } else if (ark_streq(I->name, xh_c_rule_text("ir_call_9"))) {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_call_10"));
                    } else if (ark_streq(I->name, xh_c_rule_text("ir_call_11"))) {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_call_12"));
                    } else if (ark_streq(I->name, xh_c_rule_text("ir_call_13"))) {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_call_14"));
                    } else if (ark_streq(I->name, xh_c_rule_text("ir_call_15"))) {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_call_16"));
                    } else if (ark_streq(I->name, xh_c_rule_text("ir_call_17"))) {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_call_18"));
                    } else if (ark_streq(I->name, xh_c_rule_text("ir_call_19"))) {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_call_20"));
                    } else if (ark_streq(I->name, xh_c_rule_text("ir_call_21"))) {
                        /* setjmp: dispatched inline because .gen.c has #include <setjmp.h>.
                         * The IR folds jmp_buf as int[20]=80B; we cast the slot back to
                         * jmp_buf. Requires host jmp_buf to fit in 80B; if it grows,
                         * extend the frame size or emit a dedicated buffer for jmp_buf.
                         *
                         * sp MUST be saved before setjmp and restored after the longjmp
                         * return: C99 7.13.2.1p3 makes any non-volatile automatic object
                         * that is modified between setjmp and longjmp indeterminate. sp is
                         * bumped by every IR op in between, so a plain
                         * `stk[sp++] = setjmp(...)` resumes with a garbage sp and walks off
                         * stk[] (observed as a segfault on the second traversal). ark_sp0 is
                         * not modified after the setjmp, so its value is retained. */
                        asmbuf_puts(&buf,
                            xh_c_rule_text("ir_call_22"));
                    } else if (ark_streq(I->name, xh_c_rule_text("ir_call_23"))) {
                        /* longjmp: never returns; we still push a 0 result so gcc -O2
                         * does not delete the push. C mandates val==0 returns 1. */
                        asmbuf_puts(&buf,
                            xh_c_rule_text("ir_call_24"));
                    } else if (I->name[0] == '?') {
                        /* Indirect call through function pointer: aa[0] is the
                         * function pointer, aa[1..n-1] are the actual arguments.
                         * Cast aa[0] to a function pointer and call through it. */
                        int k;
                        asmbuf_puts(&buf, xh_c_rule_text("ir_call_25"));
                        for (k = 1; k < n; k++) {
                            if (k > 1) asmbuf_puts(&buf, xh_c_rule_text("ir_call_26"));
                            asmbuf_puts(&buf, xh_c_rule_text("ir_call_27"));
                        }
                        asmbuf_puts(&buf, xh_c_rule_text("ir_call_28"));
                        for (k = 1; k < n; k++) {
                            if (k > 1) asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_19"));
                            xh_c_rule_ints(&buf, "ir_call_value_4", k, 0);
                        }
                        asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_21"));
                    } else {
                        int is_libc = is_libc_func(I->name) && !is_wrapped_func(I->name);
                        int is_wrapped = is_wrapped_func(I->name);
                        int is_void_ret = is_void_func(I->name);
                        int is_struct_ret = is_struct_ret_func(I->name);
                        /* ROUND4-P2 FIX: Also detect user-defined void functions via
                         * prog->funcs. irgen's AST_EXPR_STMT no longer emits IR_POP
                         * for direct void calls, so the backend must NOT synthesize a
                         * 0 result onto the stack (otherwise sp would drift). This
                         * branch only handles direct calls; indirect ('?') calls
                         * are dispatched above and conservatively push. */
                        if (!is_void_ret) {
                            int fi2;
                            int found = 0;
                            for (fi2 = 0; fi2 < prog->func_count; fi2++) {
                                if (ark_streq(prog->funcs[fi2]->name, I->name)) {
                                    Type *rt = prog->funcs[fi2]->ret_type;
                                    found = 1;
                                    if (rt && rt->kind == TYPE_VOID) is_void_ret = 1;
                                    break;
                                }
                            }
                            /* Declared-only callee: irgen knows the prototype,
                             * so take the result shape from the call site. */
                            if (!found && !is_void_ret)
                                is_void_ret = (int)((I->val >> 33) & 1);
                        }
                        int k;
                        /* libc: pass only n args; wrapped: wrappers have fixed 8-arg
                         * signature; user-defined: pass all MAX_CALL_ARGS args. */
                        int nargs = is_libc ? n : (is_wrapped ? xh_c_abi()->wrapper_args : MAX_CALL_ARGS);
                        if (is_struct_ret) {
                            const char *tname = struct_ret_type(I->name);
                            asmbuf_puts(&buf, xh_c_rule_text("type_union"));
                            asmbuf_puts(&buf, tname);
                            asmbuf_puts(&buf, xh_c_rule_text("ir_call_29"));
                            asmbuf_puts(&buf, I->name);
                            asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_18"));
                            for (k = 0; k < n; k++) {
                                if (k) asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_19"));
                                xh_c_rule_ints(&buf, "ir_call_value_6", k, 0);
                            }
                            /* Store the returned aggregate with memcpy: writing through a casted
                             * T* into the char[] frame is the same aliasing violation, and sizeof(_r)
                             * keeps the copy exact without re-spelling the type name. */
                            xh_c_rule_ints(&buf, "ir_call_30", xh_c_abi()->struct_return_offset, 0);
                        } else if (is_complex_ret_func(I->name)) {
                            /* Complex-returning functions: store result via .c member, pass
                             * args from aac[] (_Complex array). Do NOT cast to long long
                             * (would lose the imaginary part). These are in is_libc_func,
                             * so call host libc directly. */
                            asmbuf_puts(&buf, xh_c_rule_text("ir_call_31"));
                            asmbuf_puts(&buf, I->name);
                            asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_18"));
                            for (k = 0; k < n; k++) {
                                if (k) asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_19"));
                                xh_c_rule_ints(&buf, "ir_call_value_7", k, 0);
                            }
                            asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_21"));
                        } else if (is_void_ret) {
                            /* ROUND4-P2 FIX: Do not push a synthesized 0 result.
                             * irgen_discard_result skips IR_POP for direct void
                             * calls, so pushing here would leave sp unbalanced. */
                            asmbuf_puts(&buf, xh_c_rule_text("type_ptr_4"));
                            if (!is_libc) asmbuf_puts(&buf, xh_c_rule_text("ir_call_32"));
                            asmbuf_puts(&buf, I->name);
                            asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_18"));
                            for (k = 0; k < nargs; k++) {
                                if (k) asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_19"));
                                if (is_libc)
                                    asmbuf_puts(&buf, xh_c_rule_text("ir_call_33"));
                                else
                                    xh_c_rule_ints(&buf, "ir_call_value_4", k, 0);
                            }
                            asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_21"));
                        } else {
                            asmbuf_puts(&buf, xh_c_rule_text("ir_push_global_3"));
                            if (!is_libc) asmbuf_puts(&buf, xh_c_rule_text("ir_call_32"));
                            asmbuf_puts(&buf, I->name);
                            asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_18"));
                            for (k = 0; k < nargs; k++) {
                                if (k) asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_19"));
                                if (is_libc)
                                    asmbuf_puts(&buf, xh_c_rule_text("ir_call_33"));
                                else
                                    xh_c_rule_ints(&buf, "ir_call_value_4", k, 0);
                            }
                            asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_21"));
                        }
                    }
                    asmbuf_puts(&buf, xh_c_rule_text("ir_call_34"));
                    break;
                }
                case IR_SVC: {
                    /* __svc(n) is an ARM supervisor call; in C it is a no-op.
                     * We emit the comment so the SVC number is preserved in code. */
                    int svc_n = I->val;
                    (void)svc_n;
                    asmbuf_puts(&buf, xh_c_rule_text("ir_svc"));
                    xh_c_rule_ints(&buf, "ir_svc_value", svc_n, 0);
                    asmbuf_puts(&buf, xh_c_rule_text("ir_svc_2"));
                    break;
                }
                /* === Signed/unsigned 32-bit division: aux=0 signed, aux=1 unsigned === */
                case IR_DIV:
                    asmbuf_puts(&buf, xh_c_rule_text("ir_div"));
                    if (I->aux) asmbuf_puts(&buf, xh_c_rule_text("ir_div_2"));
                    else        asmbuf_puts(&buf, xh_c_rule_text("ir_div_3"));
                    asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_21"));
                    break;
                case IR_MOD:
                    asmbuf_puts(&buf, xh_c_rule_text("ir_div"));
                    if (I->aux) asmbuf_puts(&buf, xh_c_rule_text("ir_mod"));
                    else        asmbuf_puts(&buf, xh_c_rule_text("ir_mod_2"));
                    asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_21"));
                    break;
                /* === 64-bit operations: C uses long long natively === */
                case IR_64PUSH:
                    /* val=high 32 bits, aux=low 32 bits; assemble into a long long. */
                    xh_c_rule_ints(&buf, "ir_64push_value", I->val, 0);
                    asmbuf_int(&buf, I->aux);
                    asmbuf_puts(&buf, xh_c_rule_text("ir_64push"));
                    break;
                case IR_64BINARY:
                    /* Same as IR_BINARY but a/c are long long, preserving 64-bit. */
                    asmbuf_puts(&buf, xh_c_rule_text("ir_binary"));
                    emit_binop(&buf, I->aux, 1);
                    asmbuf_putc(&buf, ';');
                    break;
                case IR_64CMP: {
                    /* aux bit 0x100 marks an unsigned comparison (unsigned long long).
                     * Signed -1 vs unsigned ULLONG_MAX would compare differently;
                     * EQ/NE are bit-equal regardless of sign, but LT/LE/GT/GE differ. */
                    int is_unsigned = (I->aux & 0x100) != 0;
                    int op = I->aux & ~0x100;
                    const char *cop = xh_c_rule_text("ir_fcmp");
                    const char *lcast = is_unsigned ? xh_c_rule_text("ir_64cmp") : xh_c_rule_text("ir_64cmp_2");
                    const char *rcast = is_unsigned ? xh_c_rule_text("ir_64cmp_3") : xh_c_rule_text("ir_64cmp_4");
                    switch (op) {
                        case (int)TOK_EQ: cop = xh_c_rule_text("ir_fcmp"); break;
                        case (int)TOK_NE: cop = xh_c_rule_text("tok_ne_2"); break;
                        case (int)TOK_LT: cop = xh_c_rule_text("tok_lt_3");  break;
                        case (int)TOK_GT: cop = xh_c_rule_text("tok_gt_3");  break;
                        case (int)TOK_LE: cop = xh_c_rule_text("tok_le_3"); break;
                        case (int)TOK_GE: cop = xh_c_rule_text("abi_tok_ge_38"); break;
                        default: xh_c_rules_error("unsupported C comparison operation"); break;
                    }
                    asmbuf_puts(&buf, xh_c_rule_text("ir_div"));
                    asmbuf_puts(&buf, lcast);
                    asmbuf_puts(&buf, xh_c_rule_text("type_ptr_3"));
                    asmbuf_puts(&buf, cop);
                    asmbuf_puts(&buf, xh_c_rule_text("type_ptr_3"));
                    asmbuf_puts(&buf, rcast);
                    asmbuf_puts(&buf, xh_c_rule_text("abi_tok_ge_43"));
                    break;
                }
                /* === Stack-based memcpy: pop order = dst, then src === */
                case IR_MEMCPY:
                    xh_c_rule_ints(&buf, "ir_memcpy_value", I->val, 0);
                    break;
                /* === Bitfield store/load: val=bit offset, aux=bit width === */
                case IR_STORE_FLD:
                    /* aux = bit width. Use unsigned long long arithmetic so a
                     * 32-bit field (`1 << 32`) is still well defined. */
                    xh_c_rule_ints(&buf, "ir_store_fld_value", I->aux, 0);
                    asmbuf_int(&buf, I->val);
                    xh_c_rule_ints(&buf, "ir_store_fld_value_2", I->val, 0);
                    break;
                case IR_LOAD_FLD: {
                    /* aux low 8 bits = bit width, aux bit 8 (0x100) = the field
                     * is signed. Signed fields must be sign-extended after the
                     * extract (C99 6.3.1.3): without it `int c:4` holding -8
                     * read back as +8. The ARM backend already does this; the
                     * C backend used to ignore the flag and, worse, used
                     * `1 << (width|0x100)` (undefined shift) as the mask. */
                    int fld_w = I->aux & 0xFF;
                    int fld_signed = (I->aux & 0x100) != 0;
                    if (fld_w < 0) fld_w = 0;
                    if (fld_w > 32) fld_w = 32;
                    xh_c_rule_ints(&buf, "ir_load_fld_value", I->val, 0);
                    asmbuf_int(&buf, fld_w);
                    asmbuf_puts(&buf, xh_c_rule_text("ir_load_fld"));
                    if (fld_signed && fld_w > 0 && fld_w < 32) {
                        /* Narrow through (int) before the arithmetic shift so
                         * the sign bit lands where a 32-bit shift expects it
                         * (_v is 64-bit, so <<28 of 8 would stay positive). */
                        xh_c_rule_ints(&buf, "ir_load_fld_value_2", 32 - fld_w, 0);
                        asmbuf_int(&buf, 32 - fld_w);
                        asmbuf_puts(&buf, xh_c_rule_text("ir_load_fld_2"));
                    } else if (fld_signed && fld_w == 32) {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_load_fld_3"));
                    }
                    asmbuf_puts(&buf, xh_c_rule_text("ir_load_fld_4"));
                    break;
                }
                case IR_RETURN: {
                    /* Struct/union return by value (> 8 bytes): the stack top
                     * is a pointer to the struct. Copy the struct to a *static*
                     * ret buffer and return its address so the caller can memcpy.
                     * We use static (not `frame + 8176`) because `frame` is a
                     * local stack array; returning its address is UB: after
                     * the callee returns, the stack frame is destroyed and the
                     * caller's memcpy reads garbage. This was the root cause of
                     * the bootstrap crash when Token (40 bytes) was returned
                     * by lexer_peek/next. Single-threaded compiler use makes
                     * the static buffer safe (no re-entrancy).
                     * Small structs (<= 8 bytes) are returned as a value
                     * directly; callers must use IR_STORE (not IR_MEMCPY) for
                     * them, since the result is an rvalue, not a pointer. */
                    Type *rt = f->ret_type;
                    if (rt && (rt->kind == TYPE_STRUCT || rt->kind == TYPE_UNION) &&
                        type_size(rt) > xh_c_abi()->slot_bytes) {
                        int rsz = type_size(rt);
                        if (rsz > xh_c_abi()->return_buffer_bytes) {
                            if (cap > 0) out[0] = 0;
                            return -1;
                        }
                        xh_c_rule_ints(&buf, "ir_return_value", rsz, xh_c_abi()->return_buffer_bytes);
                    } else {
                        asmbuf_puts(&buf, xh_c_rule_text("ir_return"));
                    }
                    break;
                }
                /* === 0.7.0+ SIMD emulation: arithmetic on packed lanes ===
                 * SADD16/SSUB16: split a 32-bit word into two 16-bit lanes.
                 * SADD8/SSUB8: split into four 8-bit lanes.
                 * These emulate ARM DSP intrinsics for the host C backend. */
                case IR_SIMD_ADD16:
                    asmbuf_puts(&buf, xh_c_rule_text("ir_simd_add16"));
                    break;
                case IR_SIMD_SUB16:
                    asmbuf_puts(&buf, xh_c_rule_text("ir_simd_sub16"));
                    break;
                case IR_SIMD_ADD8:
                    asmbuf_puts(&buf, xh_c_rule_text("ir_simd_add8"));
                    break;
                case IR_SIMD_SUB8:
                    asmbuf_puts(&buf, xh_c_rule_text("ir_simd_sub8"));
                    break;
                default:
                    xh_c_rules_error("unsupported C IR instruction");
                    break;
            }
            asmbuf_putc(&buf, '\n');
        }
        asmbuf_puts(&buf, xh_c_rule_text("ir_simd_sub8_2"));
    }

    if (main_idx >= 0) {
        asmbuf_puts(&buf, xh_c_rule_text("ir_simd_sub8_3"));
        asmbuf_puts(&buf, xh_c_rule_text("ir_simd_sub8_4"));
    }

    {
        int n = buf.len;
        if (xh_c_rules_failed()) { if (cap > 0) out[0] = 0; return -1; }
        int i;
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

static int c_assemble(const char *asm_text, unsigned char *out, int cap) {
    /* The C backend has no assemble step; we just copy the source text.
     * The driver writes it to a .c file and invokes gcc externally. */
    int n = ark_strlen(asm_text);
    int i;
    if (n > cap) return -1;
    for (i = 0; i < n; i++) out[i] = (unsigned char)asm_text[i];
    return n;
}

static int c_link(unsigned char *code, int size, struct LinkContext *lc) {
    (void)code; (void)size; (void)lc;
    return 0;
}

static const ArkBackend g_c_backend = {
    "c",
    "host-c-source",
    (int)sizeof(void*),
    c_emit,
    c_assemble,
    c_link,
    ARK_ABI_SYSV
};

const ArkBackend *ark_backend_c(void) {
    return &g_c_backend;
}
