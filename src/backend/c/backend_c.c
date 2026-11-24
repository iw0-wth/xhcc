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
#include "xh_ir_legacy_adapter.h"
#include "xh_target_rules.h"
#include <stdio.h>   /* snprintf used for float constant formatting */
#include <string.h>
#include <stdlib.h>

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

/* Decode host-known IR schema to semantic facts. Package records own the
 * lowering selection, complete emitted sequence and matching stack effect. */
static const char *c_operation(int token) {
    switch (token) {
        case TOK_PLUS: return "ADD";
        case TOK_MINUS: return "SUB";
        case TOK_MUL: return "MUL";
        case TOK_DIV: return "DIV";
        case TOK_MOD: return "MOD";
        case TOK_BAND: return "BAND";
        case TOK_BOR: return "BOR";
        case TOK_BXOR: return "BXOR";
        case TOK_SHL: return "SHL";
        case TOK_SHR: return "SHR";
        case TOK_EQ: return "EQ";
        case TOK_NE: return "NE";
        case TOK_LT: return "LT";
        case TOK_GT: return "GT";
        case TOK_LE: return "LE";
        case TOK_GE: return "GE";
        case TOK_BNOT: return "BNOT";
        case TOK_NOT: return "NOT";
        default: return 0;
    }
}
/* Integer BINARY enters through the explicit legacy compatibility boundary.
 * Other legacy opcodes retain their own token/aux schemas below. */
static const char *c_integer_operation(XhIrBinaryOp operation) {
    switch(operation) {
        case XH_IR_BIN_ADD:return "ADD";
        case XH_IR_BIN_SUB:return "SUB";
        case XH_IR_BIN_MUL:return "MUL";
        case XH_IR_BIN_DIV:return "DIV";
        case XH_IR_BIN_REM:return "MOD";
        case XH_IR_BIN_AND:return "BAND";
        case XH_IR_BIN_OR:return "BOR";
        case XH_IR_BIN_XOR:return "BXOR";
        case XH_IR_BIN_SHL:return "SHL";
        case XH_IR_BIN_SHR:return "SHR";
        case XH_IR_BIN_EQ:return "EQ";
        case XH_IR_BIN_NE:return "NE";
        case XH_IR_BIN_LT:return "LT";
        case XH_IR_BIN_LE:return "LE";
        case XH_IR_BIN_GT:return "GT";
        case XH_IR_BIN_GE:return "GE";
        default:return 0;
    }
}
static const char *c_integer_binary(const IrInst *instruction,int *width) {
    XhIrBinaryOp operation=xh_ir_legacy_binary(instruction);
    if(operation==XH_IR_BIN_INVALID) {
        xh_c_rules_error("invalid neutral integer binary operation or flags");return 0;
    }
    *width=32;
    if(instruction->aux&IR_AUX_SLOT64) {
        const XhTargetDescriptor *target=xh_target_rules_get("c");
        if(!target || target->ir_word_bytes!=8) {
            xh_c_rules_error("unsupported C integer binary slot width");return 0;
        }
        *width=64;
    }
    return c_integer_operation(operation);
}
static int c_validate_integer_binary(IrProgram *program) {
    int fi,i,width;
    for(fi=0;fi<program->func_count;fi++) {
        IrFunc *function=program->funcs[fi];
        for(i=0;i<function->code_count;i++) if(function->code[i].op==IR_BINARY &&
            !c_integer_binary(&function->code[i],&width))return -1;
    }
    return 0;
}

static int c_semantic(IrInst *I, AsmBuffer *out, int *delta) {
    const char *family, *op = 0, *sign;
    int width;
    switch (I->op) {
        case IR_BINARY:
            family = "binary"; op = c_integer_binary(I,&width);
            if (!op) return -1;
            sign = I->aux & 0x100 ? "u" : "s";
            break;
        case IR_64BINARY:
            family = "binary"; op = c_operation(I->aux & ~0x100);
            width = I->op == IR_BINARY ? 32 : 64; sign = I->aux & 0x100 ? "u" : "s";
            break;
        case IR_DIV: case IR_MOD:
            family = "direct"; op = I->op == IR_DIV ? "DIV" : "MOD";
            width = 32; sign = I->aux ? "u" : "s"; break;
        case IR_64CMP:
            family = "compare"; op = c_operation(I->aux & ~0x100);
            width = 64; sign = I->aux & 0x100 ? "u" : "s"; break;
        case IR_FCMP:
            family = "compare"; op = c_operation(I->aux & ~(0x200 | IR_AUX_COMPLEX));
            width = I->aux & IR_AUX_COMPLEX ? 128 : 64;
            sign = I->aux & IR_AUX_COMPLEX ? "complex" : "real"; break;
        case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV:
            family = "float";
            op = I->op == IR_FADD ? "ADD" : I->op == IR_FSUB ? "SUB" : I->op == IR_FMUL ? "MUL" : "DIV";
            if (I->op == IR_FADD && (I->aux & 0xF00) == 0x900) op = "EXP";
            width = I->aux & IR_AUX_COMPLEX ? 128 : 64;
            sign = I->aux & IR_AUX_COMPLEX ? "complex" : "real"; break;
        case IR_UNARY:
            family = "unary";
            if (I->aux & IR_AUX_COMPLEX) { op = "CONJ"; width = 128; sign = "complex"; }
            else if (I->val == 1) { op = "NEG"; width = 64; sign = "real"; }
            else { op = c_operation(I->aux); if (I->aux == TOK_MINUS) op = "NEG"; width = 64; sign = "s"; }
            break;
        default: return 0;
    }
    if (!op) { xh_c_rules_error("unsupported C semantic operation"); return -1; }
    return xh_c_semantic_select(family, op, width, sign, out, delta) ? -1 : 1;
}

/* Schema normalization supplies bounded values; selectors own whole sequences. */
static int c_memory(IrInst *I, IrFunc *f, IrProgram *prog, int *param_idx, AsmBuffer *out, int *delta) {
    const char *family, *op, *sign = "s";
    int width = 64, size;
    char val[32], aux[32], extra[32], buffer[32];
    const char *args[6];
    snprintf(val,sizeof val,"%lld",I->val);
    snprintf(aux,sizeof aux,"%d",I->aux);
    extra[0]='0';extra[1]=0;
    snprintf(buffer,sizeof buffer,"%d",xh_c_abi()->return_buffer_bytes);
    args[0]=val;args[1]=aux;args[2]=extra;args[3]="";args[4]=I->name?I->name:"";args[5]=buffer;
    switch (I->op) {
        case IR_LOAD_FLD:case IR_STORE_FLD:
            family="bitfield";op=I->op==IR_LOAD_FLD?"LOAD":"STORE";width=32;
            size=I->op==IR_LOAD_FLD?I->aux&0xff:I->aux;
            if(size<1||size>32||I->val<0||I->val>31||I->val+size>32 ||
               (I->op==IR_LOAD_FLD&&(I->aux&~0x1ff))) {
                xh_c_rules_error("invalid C bitfield width, offset or flags");return -1;
            }
            sign=I->op==IR_LOAD_FLD&&(I->aux&0x100)?"s":"u";
            snprintf(aux,sizeof aux,"%d",size);break;
        case IR_SIMD_ADD16:case IR_SIMD_SUB16:case IR_SIMD_ADD8:case IR_SIMD_SUB8:
            if(I->aux){xh_c_rules_error("invalid C SIMD flags");return -1;}
            family="simd";op=I->op==IR_SIMD_ADD16||I->op==IR_SIMD_ADD8?"ADD":"SUB";
            width=I->op==IR_SIMD_ADD16||I->op==IR_SIMD_SUB16?16:8;break;
        case IR_GETARG:
            family="arg";op="LOAD";
            size=(I->aux>>8)&0xffffff;
            if (out && !param_idx) {xh_c_rules_error("missing C parameter binding");return -1;}
            if (I->val>=xh_c_abi()->varargs_offset) size=0;
            if (size>xh_c_abi()->slot_bytes) sign="aggregate";
            snprintf(aux,sizeof aux,"%d",I->val>=xh_c_abi()->varargs_offset?I->aux&0xff:param_idx?*param_idx:0);
            snprintf(extra,sizeof extra,"%d",size);
            snprintf(buffer,sizeof buffer,"%d",xh_c_abi()->slot_bytes);
            if (out && I->val<xh_c_abi()->varargs_offset) (*param_idx)++;
            break;
        case IR_PUSH_GLOBAL:
            family="global";op="LOAD";
            if (prog) {
                int gi;
                for(gi=0;gi<prog->global_count;gi++) if(prog->globals[gi]->name&&ark_streq(prog->globals[gi]->name,I->name)) {
                    Type *t=prog->globals[gi]->type;
                    if(t&&(t->kind==TYPE_ARRAY||t->kind==TYPE_STRUCT||t->kind==TYPE_UNION))sign="aggregate";
                    else if(t&&(t->kind==TYPE_FLOAT||t->kind==TYPE_DOUBLE))sign="real";
                    break;
                }
            }
            break;
        case IR_PUSH_GLOBAL_ADDR: family="global";op="ADDR";break;
        case IR_STORE_GLOBAL: family="global";op="STORE";break;
        case IR_PUSH_VAR:
            family="frame";op="LOAD";width=I->aux&0xff;
            if (!width)width=xh_c_abi()->slot_bytes;
            sign=width==16?"complex":I->aux&0x100?"u":"s";break;
        case IR_STORE:
            family="frame";op="STORE";width=I->aux&~IR_AUX_VOLATILE;
            if (!width)width=xh_c_abi()->slot_bytes;
            sign=width==16?"complex":"s";break;
        case IR_PUSH_ADDR: family="frame";op="ADDR";width=8;break;
        case IR_LOAD_IDX: case IR_STORE_IDX:
            family="index";op=I->op==IR_LOAD_IDX?"LOAD":"STORE";
            width=(int)I->val;
            if (width < 1) { xh_c_rules_error("invalid C indexed access stride"); return -1; }
            /* IR stride may be an aggregate size; the historical indexed
             * fallback accesses an int while retaining that byte stride. */
            if (width!=1 && width!=2 && width!=8) width=4;
            sign=I->aux&IR_AUX_VOLATILE?"v":"s";break;
        case IR_TRUNC:
            family="convert";op="TRUNC";width=(int)I->val;sign=I->aux?"u":"s";break;
        case IR_SEXT: family="convert";op="SEXT";break;
        case IR_ZEXT: family="convert";op="ZEXT";break;
        case IR_CDECL64: family="convert";op="CDECL64";break;
        case IR_POP: family="stack";op="POP";break;
        case IR_DUP: family="stack";op="DUP";break;
        case IR_SWAP: family="stack";op="SWAP";break;
        case IR_ALLOCA: family="stack";op="ALLOCA";break;
        case IR_MEMCPY: family="stack";op="MEMCPY";break;
        case IR_LABEL: family="control";op="LABEL";break;
        case IR_LABEL_NAMED: family="control";op="LABEL";sign="named";break;
        case IR_JMP: family="control";op="JMP";break;
        case IR_JMP_NAMED: family="control";op="JMP";sign="named";break;
        case IR_JZ: family="control";op="JZ";break;
        case IR_JNZ: family="control";op="JNZ";break;
        case IR_CMP_JMP_EQ: family="control";op="CMP_EQ";break;
        case IR_RETURN:
            family="control";op="RETURN";
            if (f && f->ret_type && (f->ret_type->kind==TYPE_STRUCT || f->ret_type->kind==TYPE_UNION) &&
                (size=type_size(f->ret_type))>xh_c_abi()->slot_bytes) {
                if(size>xh_c_abi()->return_buffer_bytes){xh_c_rules_error("C aggregate return exceeds buffer capacity");return -1;}
                sign="aggregate";snprintf(extra,sizeof extra,"%d",size);
            }
            break;
        default:return 0;
    }
    return xh_c_semantic_bind(family,op,width,sign,out,delta,args,6)?-1:1;
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
    if (f->is_weak)   asmbuf_puts(b, xh_c_attribute("attribute_tok_ge_4"));
    if (f->is_naked || f->is_asm_func) asmbuf_puts(b, xh_c_attribute("attribute_tok_ge_5"));
    if (f->is_used)   asmbuf_puts(b, xh_c_attribute("attribute_tok_ge_6"));
    if (f->is_unused) asmbuf_puts(b, xh_c_attribute("attribute_tok_ge_7"));
    if (f->forceinline) asmbuf_puts(b, xh_c_attribute("attribute_tok_ge_8"));
    /* constructor/destructor emit into gcc's .init_array/.fini_array sections;
     * deprecated triggers a gcc warning at the use site. */
    if (f->is_constructor) asmbuf_puts(b, xh_c_attribute("attribute_tok_ge_9"));
    if (f->is_destructor)  asmbuf_puts(b, xh_c_attribute("attribute_tok_ge_10"));
    if (f->is_deprecated)  asmbuf_puts(b, xh_c_attribute("attribute_tok_ge_11"));
#if ARKCC_FEATURE_ARMCC_KEYWORDS
    /* armcc 5 keywords mapped to the closest gcc equivalent:
     *   __pure            -> __attribute__((pure))
     *   __attribute__((const)) -> __attribute__((const))
     *   __value_in_regs   -> no gcc equivalent; ignored (struct return ABI differs)
     *   __interwork       -> ARM/Thumb interworking; ignored on host builds
     *   __smc(n)          -> secure monitor call; ignored on host builds */
    if (f->is_pure)       asmbuf_puts(b, xh_c_attribute("attribute_tok_ge_12"));
    if (f->is_const_attr) asmbuf_puts(b, xh_c_attribute("attribute_tok_ge_13"));
#endif
    if (f->alias_target) {
        /* alias attribute: __attribute__((alias("ark_xxx"))) */
        asmbuf_puts(b, xh_c_attribute("attribute_tok_ge_14"));
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

/* IR metadata normalization plus bounded argument iteration. All target call
 * routes, cast spellings and invocation productions are external selections. */
static int c_call(IrProgram *prog, IrInst *I, AsmBuffer *out, int *delta) {
    const char *result="scalar",*argmode="llong",*prep="integer";
    char route[32],index[16],offset[16];
    char arguments[2048],signature[512],fragment[128];
    const char *bind[6];
    int n,k,nargs,start=0,pop,push,is_host,is_wrapped,used=0,sigused=0;
    unsigned long long raw=(unsigned long long)I->val;
    if (I->op!=IR_CALL)return 0;
    if (!I->name || !I->name[0] || (raw&0xffffffffu)>(unsigned)MAX_CALL_ARGS) {
        xh_c_rules_error("invalid C call name or argument count");return -1;
    }
    n=(int)(raw&0xffffffffu);
    if (is_void_func(I->name) || ((raw>>33)&1))result="void";
    if (prog)for(k=0;k<prog->func_count;k++)if(ark_streq(prog->funcs[k]->name,I->name)) {
        Type *rt=prog->funcs[k]->ret_type;
        if(rt&&rt->kind==TYPE_VOID)result="void";
        break;
    }
    is_wrapped=is_wrapped_func(I->name);
    is_host=is_libc_func(I->name)&&!is_wrapped;
    if(is_complex_ret_func(I->name)){result="complex";prep="complex";argmode="complex";}
    else if(is_struct_ret_func(I->name))result="struct";
    if(I->name[0]=='?') {
        if(n<1){xh_c_rules_error("C indirect call requires a function pointer");return -1;}
        strcpy(route,"indirect");start=1;nargs=n;argmode="llong";
        /* IR indirect calls currently promise one integer result. */
        if(!ark_streq(result,"scalar")){xh_c_rules_error("unsupported C indirect result shape");return -1;}
    } else {
        int found=xh_c_rule_find("call.routes",I->name,route,sizeof route);
        if(found<0)return -1;
        if(!found)strcpy(route,is_host?"host":is_wrapped?"wrapper":"user");
        nargs=is_host?n:is_wrapped?xh_c_abi()->wrapper_args:MAX_CALL_ARGS;
        if(is_host&&!ark_streq(prep,"complex"))argmode="pointer";
        /* Explicit routes are complete historical host call productions. */
        if(found){nargs=ark_streq(route,"printf")?MAX_CALL_ARGS:n;result="scalar";}
    }
    if(xh_c_call_effect(route,result,&pop,&push))return -1;
    xh_c_effect_set(n*pop,push);
    *delta=push-n*pop;
    if(!out)return 1;
    snprintf(offset,sizeof offset,"%d",xh_c_abi()->struct_return_offset);
    bind[0]=index;bind[1]=arguments;bind[2]="";bind[3]=signature;bind[4]=offset;bind[5]="";
    if(xh_c_semantic_bind("callprep",prep,64,"s",out,&k,bind,6))return -1;
    for(k=0;k<n;k++) {
        int ignored;snprintf(index,sizeof index,"%d",k);
        if(xh_c_semantic_bind("callpop",prep,64,"s",out,&ignored,bind,6))return -1;
    }
    arguments[0]=0;signature[0]=0;
    for(k=start;k<nargs;k++) {
        int len;const char *mode=argmode;
        if(ark_streq(route,"printf")) {
            unsigned mask=(unsigned)I->aux;
            mode=k==0?"format":(mask>>(2*k))&1?"double":(mask>>(2*k+1))&1?"llong":"integer";
        }
        snprintf(index,sizeof index,"%d",k);
        if(k>start) {
            len=xh_c_call_piece("callaux","separator",bind,fragment,sizeof fragment);
            if(len<0||used+len>=(int)sizeof arguments)goto overflow;
            memcpy(arguments+used,fragment,len);used+=len;
        }
        len=xh_c_call_piece("callarg",mode,bind,fragment,sizeof fragment);
        if(len<0||used+len>=(int)sizeof arguments)goto overflow;
        memcpy(arguments+used,fragment,len+1);used+=len;
        if(ark_streq(route,"indirect")) {
            if(k>start) {
                len=xh_c_call_piece("callaux","separator",bind,fragment,sizeof fragment);
                if(len<0||sigused+len>=(int)sizeof signature)goto overflow;
                memcpy(signature+sigused,fragment,len);sigused+=len;
            }
            len=xh_c_call_piece("callaux","signature",bind,fragment,sizeof fragment);
            if(len<0||sigused+len>=(int)sizeof signature)goto overflow;
            memcpy(signature+sigused,fragment,len+1);sigused+=len;
        }
    }
    bind[0]=I->name;
    if(ark_streq(result,"struct"))bind[2]=struct_ret_type(I->name);
    if(xh_c_semantic_bind("call",route,64,result,out,&k,bind,6))return -1;
    if(xh_c_semantic_bind("callaux","end",64,"s",out,&k,bind,6))return -1;
    return 1;
overflow:
    xh_c_rules_error("C call argument or signature expansion exceeds capacity");return -1;
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

/* The walker handles graph structure and finite traversal. Package records
 * choose complete scalar/declarator/qualifier and aggregate productions. */
static void c_type_production(AsmBuffer *b,const char *operation,const char *name,const char *tag) {
    char text[1024];
    if(xh_c_production("type",operation,name,tag,text,sizeof text)>=0)asmbuf_puts(b,text);
}
static const char *c_type_kind(int kind) {
    switch(kind){
        case TYPE_VOID:return "VOID";
        case TYPE_CHAR:return "CHAR";
        case TYPE_SHORT:return "SHORT";
        case TYPE_INT:return "INT";
        case TYPE_LONG:return "LONG";
        case TYPE_LLONG:return "LLONG";
        case TYPE_BOOL:return "BOOL";
        case TYPE_FLOAT:return "FLOAT";
        case TYPE_DOUBLE:return "DOUBLE";
        case TYPE_ENUM:return "ENUM";
        case TYPE_WCHAR:return "WCHAR";
        default:return 0;
    }
}
static void emit_c_type_walk(AsmBuffer *b,Type *t,const char *name,Type **path,int depth) {
    const char *scalar;int i;
    if(!t){c_type_production(b,"LLONG",name,0);return;}
    if(depth>=64){xh_c_rules_error("C type traversal depth exceeds capacity");return;}
    for(i=0;i<depth;i++)if(path[i]==t){xh_c_rules_error("cyclic C type expansion");return;}
    path[depth]=t;
    if(t->kind!=TYPE_PTR){
        if(t->is_volatile)c_type_production(b,"QUAL_VOLATILE",0,0);
        if(t->is_const)c_type_production(b,"QUAL_CONST",0,0);
    }
    scalar=c_type_kind(t->kind);
    if(scalar){c_type_production(b,scalar,name,0);return;}
    switch(t->kind){
        case TYPE_COMPLEX:
            c_type_production(b,t->base&&t->base->kind==TYPE_FLOAT?"COMPLEX_FLOAT":"COMPLEX_DOUBLE",name,0);return;
        case TYPE_PTR:{
            char next[256];
            if(xh_c_production("type","POINTER",name,0,next,sizeof next)<0)return;
            if(t->base&&(t->base->kind==TYPE_STRUCT||t->base->kind==TYPE_UNION)){
                c_type_production(b,t->base->tag?(t->base->kind==TYPE_UNION?"UNION_REF":"STRUCT_REF"):"ANON_POINTER",next,t->base->tag);return;
            }
            if(t->base&&t->base->kind==TYPE_FUNC){
                Type *f=t->base;
                emit_c_type_walk(b,f->base,"",path,depth+1);
                c_type_production(b,"FUNPTR_OPEN",name,0);
                if(f->param_count<0||f->param_count>256){xh_c_rules_error("C function type parameter capacity exceeded");return;}
                for(i=0;i<f->param_count;i++){
                    if(i)c_type_production(b,"PARAM_SEPARATOR",0,0);
                    emit_c_type_walk(b,f->params[i],"",path,depth+1);
                }
                if(f->is_variadic){if(f->param_count)c_type_production(b,"PARAM_SEPARATOR",0,0);c_type_production(b,"VARIADIC",0,0);}
                c_type_production(b,"FUNPTR_CLOSE",0,0);return;
            }
            emit_c_type_walk(b,t->base,next,path,depth+1);return;
        }
        case TYPE_ARRAY:{
            int dims[16],n=0,k;Type *cur=t;
            while(cur&&cur->kind==TYPE_ARRAY&&n<16){
                for(k=0;k<depth+n;k++)if(path[k]==cur && !(n==0&&k==depth)){xh_c_rules_error("cyclic C array type");return;}
                if(depth+n>=64){xh_c_rules_error("C type traversal depth exceeds capacity");return;}
                path[depth+n]=cur;dims[n++]=cur->size;cur=cur->base;
            }
            if(cur&&cur->kind==TYPE_ARRAY){xh_c_rules_error("C array declarator exceeds dimension capacity");return;}
            emit_c_type_walk(b,cur,name,path,depth+n);
            for(k=0;k<n;k++){char dim[24];snprintf(dim,sizeof dim,"%d",dims[k]);c_type_production(b,dims[k]>0?"ARRAY_DIM":"ARRAY_UNSIZED",dim,0);}return;
        }
        case TYPE_STRUCT:case TYPE_UNION:{
            StructField *field=t->fields;int count=0;
            c_type_production(b,t->kind==TYPE_UNION?"UNION_HEAD":"STRUCT_HEAD",0,0);
            if(field){
                c_type_production(b,"BODY_OPEN",0,0);
                while(field){
                    if(++count>1024){xh_c_rules_error("C aggregate field traversal exceeds capacity");return;}
                    if(field->name&&*field->name){emit_c_type_walk(b,field->type,field->name,path,depth+1);c_type_production(b,"FIELD_END",0,0);}
                    field=field->next;
                }
                c_type_production(b,"BODY_CLOSE",0,0);
            }else if(t->tag)c_type_production(b,"TAG",0,t->tag);
            c_type_production(b,"NAME",name,0);return;
        }
        default:xh_c_rules_error("unsupported C type production");return;
    }
}
static void emit_c_type(AsmBuffer *b,Type *t,const char *name){
    Type *path[64];emit_c_type_walk(b,t,name,path,0);
}

/* Check whether a C type is an aggregate (array/struct/union). */
static int c_type_is_aggregate(Type *t) {
    if (!t) return 0;
    return t->kind == TYPE_ARRAY || t->kind == TYPE_STRUCT || t->kind == TYPE_UNION;
}

/* Compute the stack-effect delta of a single IR instruction.
 * Positive = pushes, negative = pops, zero = no net change. */
static int ir_stack_delta_ctx(IrProgram *prog,IrInst *I) {
    int delta, selected = c_call(prog,I,0,&delta);
    if (selected) return selected > 0 ? delta : 0;
    selected = c_memory(I, 0, 0, 0, 0, &delta);
    if (selected) return selected > 0 ? delta : 0;
    selected = c_semantic(I, 0, &delta);
    if (selected) return selected > 0 ? delta : 0;
    switch (I->op) {
        case IR_PUSH_INT:
        case IR_PUSH_VAR:
        case IR_PUSH_ADDR:
        case IR_PUSH_STR:
        case IR_PUSH_GLOBAL:
        case IR_PUSH_GLOBAL_ADDR:
        case IR_PUSH_FUNC:
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
        case IR_TRUNC:
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

static int ir_stack_delta(IrInst *I) { return ir_stack_delta_ctx(0,I); }

/* Finite CFG mechanism. Target-selected records supply pop/push; this worklist
 * accepts one exact depth per reachable instruction, never a guessed bound. */
static int c_stack_effect(IrProgram *prog,IrFunc *f,IrInst *I,int *pop,int *push) {
    int delta,selected=c_call(prog,I,0,&delta);
    if(selected){if(selected<0)return -1;xh_c_effect_get(pop,push);return 0;}
    selected=c_memory(I,f,prog,0,0,&delta);
    if(!selected)selected=c_semantic(I,0,&delta);
    if(selected){if(selected<0)return -1;xh_c_effect_get(pop,push);return 0;}
    *pop=0;*push=0;
    switch(I->op){
        case IR_PUSH_INT:case IR_PUSH_FLOAT:case IR_PUSH_STR:case IR_PUSH_FUNC:case IR_64PUSH:*push=1;break;
        case IR_F2I:case IR_I2F:case IR_64I2F:case IR_64F2I:case IR_F2F:case IR_SVC:case IR_LOAD_FLD:*pop=1;*push=1;break;
        case IR_STORE_FLD:*pop=2;break;
        case IR_SIMD_ADD16:case IR_SIMD_SUB16:case IR_SIMD_ADD8:case IR_SIMD_SUB8:*pop=2;*push=1;break;
        case IR_FUNC_BEGIN:case IR_FUNC_END:case IR_ASM:break;
        default:xh_c_rules_error("unsupported IR in C stack analysis");return -1;
    }return 0;
}
static int c_depth_join(int node,int depth,int count,int *depths,int *queue,int *tail){
    if(node==count)return 0;
    if(node<0||node>count){xh_c_rules_error("invalid C CFG successor");return -1;}
    if(depths[node]>=0){if(depths[node]!=depth){xh_c_rules_error("inconsistent C value-stack depth at CFG join or cycle");return -1;}return 0;}
    depths[node]=depth;queue[(*tail)++]=node;return 0;
}
static int compute_max_sp(IrProgram *prog,IrFunc *f) {
    int *state=0,*depths,*queue,*targets;
    int count=f->code_count,head=0,tail=0,max_depth=0,i,j;
    if(count<0||count>ARK_C_MAX_LABELS){xh_c_rules_error("C CFG instruction budget exceeded");return -1;}
    if(!count)return 1;
    state=(int*)malloc((size_t)count*3*sizeof(int));
    if(!state){xh_c_rules_error("C CFG analysis allocation failed");return -1;}
    depths=state;queue=state+count;targets=state+2*count;
    for(i=0;i<count;i++){depths[i]=-1;targets[i]=-1;}
    /* Validate labels and resolve all edges, including unreachable code. */
    for(i=0;i<count;i++){
        IrInst *I=&f->code[i];
        if(I->op==IR_LABEL || I->op==IR_LABEL_NAMED){
            if((I->op==IR_LABEL&&(I->val<0||I->val>2147483647LL)) ||
               (I->op==IR_LABEL_NAMED&&(!I->name||!*I->name))){xh_c_rules_error("invalid C CFG label");goto fail;}
            for(j=0;j<i;j++)if(f->code[j].op==I->op &&
                (I->op==IR_LABEL?f->code[j].val==I->val:ark_streq(f->code[j].name,I->name))){xh_c_rules_error("duplicate C CFG label");goto fail;}
        }
        if(I->op==IR_JMP||I->op==IR_JZ||I->op==IR_JNZ||I->op==IR_CMP_JMP_EQ||I->op==IR_JMP_NAMED){
            int named=I->op==IR_JMP_NAMED;long long id=I->op==IR_CMP_JMP_EQ?I->aux:I->val;
            if((named&&(!I->name||!*I->name))||(!named&&(id<0||id>2147483647LL))){xh_c_rules_error("invalid C CFG branch target");goto fail;}
            for(j=0;j<count;j++)if(named?(f->code[j].op==IR_LABEL_NAMED&&ark_streq(f->code[j].name,I->name)):(f->code[j].op==IR_LABEL&&f->code[j].val==id)){targets[i]=j;break;}
            if(targets[i]<0){xh_c_rules_error("unresolved C CFG branch target");goto fail;}
        }
    }
    depths[0]=0;queue[tail++]=0;
    while(head<tail){
        int node=queue[head++],pop,push,depth=depths[node],after;
        IrInst *I=&f->code[node];
        if(c_stack_effect(prog,f,I,&pop,&push))goto fail;
        if(depth<pop || (I->op==IR_CMP_JMP_EQ&&depth<1)){xh_c_rules_error("C value-stack underflow in reachable IR");goto fail;}
        after=depth-pop+push;
        if(after>65536){xh_c_rules_error("C value-stack depth budget exceeded");goto fail;}
        if(after>max_depth)max_depth=after;
        if(I->op==IR_RETURN)continue;
        if(I->op==IR_JMP||I->op==IR_JMP_NAMED){if(c_depth_join(targets[node],after,count,depths,queue,&tail))goto fail;continue;}
        if(I->op==IR_JZ||I->op==IR_JNZ||I->op==IR_CMP_JMP_EQ){
            int branch=I->op==IR_CMP_JMP_EQ?after-1:after;
            if(c_depth_join(targets[node],branch,count,depths,queue,&tail))goto fail;
        }
        if(c_depth_join(node+1,after,count,depths,queue,&tail))goto fail;
    }
    free(state);return max_depth>0?max_depth:1;
fail:free(state);return -1;
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
    if (c_validate_integer_binary(prog)) { if (cap > 0) out[0] = 0; return -1; }
    if (xh_c_rules_begin()) { if (cap > 0) out[0] = 0; return -1; }
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

        if (f->is_weak)   asmbuf_puts(&buf, xh_c_attribute("attribute_tok_ge_4"));
        if (f->is_naked || f->is_asm_func) asmbuf_puts(&buf, xh_c_attribute("attribute_tok_ge_5"));
        if (f->is_used)   asmbuf_puts(&buf, xh_c_attribute("attribute_tok_ge_6"));
        if (f->is_unused) asmbuf_puts(&buf, xh_c_attribute("attribute_tok_ge_7"));
        if (f->is_irq)    asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_54"));
        if (f->is_constructor) asmbuf_puts(&buf, xh_c_attribute("attribute_tok_ge_9"));
        if (f->is_destructor)  asmbuf_puts(&buf, xh_c_attribute("attribute_tok_ge_10"));
        if (f->is_deprecated)  asmbuf_puts(&buf, xh_c_attribute("attribute_tok_ge_11"));
#if ARKCC_FEATURE_ARMCC_KEYWORDS
        if (f->is_pure)       asmbuf_puts(&buf, xh_c_attribute("attribute_tok_ge_12"));
        if (f->is_const_attr) asmbuf_puts(&buf, xh_c_attribute("attribute_tok_ge_13"));
        if (f->is_value_in_regs) asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_55"));
        if (f->is_interwork)     asmbuf_puts(&buf, xh_c_rule_text("module_ir_cmp_jmp_eq_56"));
        if (f->smc_num > 0) {
            xh_c_rule_ints(&buf, "module_ir_cmp_jmp_eq_value_6", f->smc_num, 0);
        }
#endif
        /* --split_sections/-ffunction-sections: per-function section attribute for gcc. */
        if (g_config.split_sections) {
            asmbuf_puts(&buf, xh_c_attribute("attribute_ir_cmp_jmp_eq_57"));
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
            int stk_sz = compute_max_sp(prog,f);
            if(stk_sz<0){if(cap>0)out[0]=0;return -1;}
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
            {
                int delta, selected = c_call(prog,I,&buf,&delta);
                if (!selected) selected = c_memory(I, f, prog, &param_idx, &buf, &delta);
                if (!selected) selected = c_semantic(I, &buf, &delta);
                if (selected < 0) { if (cap > 0) out[0] = 0; return -1; }
                if (selected) { asmbuf_putc(&buf, '\n'); continue; }
            }
            switch (I->op) {
                case IR_FUNC_BEGIN:
                case IR_FUNC_END:
                    asmbuf_puts(&buf, xh_c_rule_text("ir_func_end"));
                    break;
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
                case IR_64PUSH:
                    /* val=high 32 bits, aux=low 32 bits; assemble into a long long. */
                    xh_c_rule_ints(&buf, "ir_64push_value", I->val, 0);
                    asmbuf_int(&buf, I->aux);
                    asmbuf_puts(&buf, xh_c_rule_text("ir_64push"));
                    break;
                /* === Stack-based memcpy: pop order = dst, then src === */
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
