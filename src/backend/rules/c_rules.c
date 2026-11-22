/* C adapter; parsing, bounds and expansion belong to the shared template VM. */
#include "xh_c_rules.h"
#include "xh_template.h"
#include "common.h"
#include "diag.h"
#include <stdio.h>
#include <string.h>

static int c_failed;
static XhCAbi abi;
const XhCAbi *xh_c_abi(void) { return &abi; }
static int abi_number(const char *key, int min, int max, int *out) {
    const char *p = xh_template_get("c", key);
    unsigned n = 0;
    if (!p || !*p) goto bad;
    while (*p) {
        if (*p < '0' || *p > '9' || n > 1000000u) goto bad;
        n = n * 10u + (unsigned)(*p++ - '0');
    }
    if (n < (unsigned)min || n > (unsigned)max) goto bad;
    *out = (int)n;
    return 0;
bad:
    diag_tool(DIAG_SEV_ERROR, "c templates", "invalid or incompatible C ABI numeric rule", key,0,0,0,0,0);
    c_failed = 1;
    return -1;
}
static int load_abi(void) {
    /* Exact constraints describe the existing front-end/IR ABI contract.
     * Layout changes require corresponding front-end and runtime changes. */
    if (abi_number("abi_pointer_bits", (int)sizeof(void*)*8, (int)sizeof(void*)*8, &abi.pointer_bits) ||
        abi_number("abi_slot_bytes", 8, 8, &abi.slot_bytes) ||
        abi_number("abi_max_call_args", 16, 16, &abi.max_call_args) ||
        abi_number("abi_wrapper_args", 8, 8, &abi.wrapper_args) ||
        abi_number("abi_varargs_offset", 8000, 8000, &abi.varargs_offset) ||
        abi_number("abi_struct_return_offset", 8176, 8176, &abi.struct_return_offset) ||
        abi_number("abi_frame_align", 8, 8, &abi.frame_align) ||
        abi_number("policy_frame_padding", 0, 65536, &abi.frame_padding) ||
        abi_number("policy_frame_min", 8704, 1048576, &abi.frame_min) ||
        abi_number("policy_stack_min", 256, 65536, &abi.stack_min) ||
        abi_number("policy_stack_margin", 16, 65536, &abi.stack_margin) ||
        abi_number("policy_return_buffer_bytes", 256, 65536, &abi.return_buffer_bytes)) return -1;
    return 0;
}

static const char *const required[] = {
    "abi_struct_return_types",
    "abi_libcs",
    "abi_voids",
    "abi_structs",
    "abi_cplx",
    "abi_wrapped",
    "abi_vars",
    "abi_tok_ge_value",
    "abi_tok_ge_value_2",
    "module_ir_cmp_jmp_eq_value",
    "module_ir_cmp_jmp_eq_value_2",
    "module_ir_cmp_jmp_eq_value_3",
    "module_ir_cmp_jmp_eq_value_4",
    "module_ir_cmp_jmp_eq_value_5",
    "module_ir_cmp_jmp_eq_value_6",
    "module_ir_cmp_jmp_eq_value_7",
    "module_ir_cmp_jmp_eq_value_8",
    "module_ir_cmp_jmp_eq_value_9",
    "ir_getarg_value",
    "ir_getarg_value_2",
    "ir_getarg_value_3",
    "ir_push_int_value",
    "ir_push_float_value",
    "ir_push_float_value_2",
    "ir_push_var_value",
    "ir_push_addr_value",
    "ir_store_value",
    "ir_load_idx_value",
    "ir_store_idx_value",
    "ir_label_value",
    "module_ir_cmp_jmp_eq_value_10",
    "ir_call_value",
    "ir_call_value_2",
    "ir_call_value_3",
    "ir_call_value_4",
    "ir_call_value_5",
    "ir_call_value_6",
    "ir_call_value_7",
    "ir_svc_value",
    "ir_64push_value",
    "ir_memcpy_value",
    "ir_store_fld_value",
    "ir_store_fld_value_2",
    "ir_load_fld_value",
    "ir_load_fld_value_2",
    "ir_return_value",
    "ir_simd_sub8_value",
    "escape_newline",
    "escape_tab",
    "escape_return",
    "escape_backslash",
    "escape_quote",
    "escape_hex_digits",
    "escape_literal_boundary",
    "tok_plus",
    "tok_minus",
    "tok_mul",
    "tok_div",
    "tok_mod",
    "tok_band",
    "tok_bor",
    "tok_bxor",
    "tok_shl",
    "tok_shr",
    "tok_shr_2",
    "tok_shr_3",
    "tok_eq",
    "tok_ne",
    "tok_lt",
    "tok_lt_2",
    "tok_gt",
    "tok_gt_2",
    "tok_le",
    "tok_le_2",
    "tok_ge",
    "abi_tok_ge_2",
    "abi_tok_ge_3",
    "attribute_tok_ge_4",
    "attribute_tok_ge_5",
    "attribute_tok_ge_6",
    "attribute_tok_ge_7",
    "attribute_tok_ge_8",
    "attribute_tok_ge_9",
    "attribute_tok_ge_10",
    "attribute_tok_ge_11",
    "attribute_tok_ge_12",
    "attribute_tok_ge_13",
    "attribute_tok_ge_14",
    "abi_tok_ge_15",
    "abi_tok_ge_16",
    "abi_tok_ge_17",
    "abi_tok_ge_18",
    "abi_tok_ge_19",
    "abi_tok_ge_20",
    "abi_tok_ge_21",
    "abi_tok_ge_22",
    "abi_tok_ge_23",
    "abi_tok_ge_24",
    "abi_tok_ge_25",
    "abi_tok_ge_26",
    "abi_tok_ge_27",
    "abi_tok_ge_28",
    "abi_tok_ge_29",
    "abi_tok_ge_30",
    "abi_tok_ge_31",
    "abi_tok_ge_32",
    "abi_tok_ge_33",
    "abi_tok_ge_34",
    "abi_tok_ge_35",
    "abi_tok_ge_36",
    "abi_tok_ge_37",
    "type_void",
    "type_char",
    "type_short",
    "type_int",
    "type_long",
    "type_bool",
    "type_float",
    "type_double",
    "type_wchar",
    "type_complex",
    "type_ptr",
    "type_ptr_2",
    "type_ptr_3",
    "type_ptr_4",
    "type_ptr_5",
    "type_ptr_6",
    "type_ptr_7",
    "type_ptr_8",
    "type_array",
    "type_array_2",
    "type_union",
    "type_union_2",
    "type_union_3",
    "file_header",
    "ir_cmp_jmp_eq",
    "module_ir_cmp_jmp_eq_2",
    "module_ir_cmp_jmp_eq_3",
    "include_stdio_h",
    "include_stdlib_h",
    "include_string_h",
    "include_stdint_h",
    "include_setjmp_h",
    "include_malloc_h",
    "include_wchar_h",
    "include_wctype_h",
    "include_errno_h",
    "include_fenv_h",
    "include_math_h",
    "include_complex_h",
    "include_time_h",
    "include_locale_h",
    "include_signal_h",
    "include_inttypes_h",
    "module_ir_cmp_jmp_eq_20",
    "module_ir_cmp_jmp_eq_21",
    "module_ir_cmp_jmp_eq_22",
    "module_ir_cmp_jmp_eq_23",
    "module_ir_cmp_jmp_eq_24",
    "runtime_ir_cmp_jmp_eq_25",
    "runtime_ir_cmp_jmp_eq_26",
    "runtime_ir_cmp_jmp_eq_27",
    "runtime_ir_cmp_jmp_eq_28",
    "runtime_ir_cmp_jmp_eq_29",
    "runtime_ir_cmp_jmp_eq_30",
    "runtime_ir_cmp_jmp_eq_31",
    "runtime_ir_cmp_jmp_eq_32",
    "runtime_ir_cmp_jmp_eq_33",
    "runtime_ir_cmp_jmp_eq_34",
    "module_ir_cmp_jmp_eq_35",
    "module_ir_cmp_jmp_eq_36",
    "module_ir_cmp_jmp_eq_37",
    "module_ir_cmp_jmp_eq_38",
    "module_ir_cmp_jmp_eq_39",
    "module_ir_cmp_jmp_eq_40",
    "module_ir_cmp_jmp_eq_41",
    "module_ir_cmp_jmp_eq_42",
    "module_ir_cmp_jmp_eq_43",
    "module_ir_cmp_jmp_eq_44",
    "module_ir_cmp_jmp_eq_45",
    "module_ir_cmp_jmp_eq_46",
    "module_ir_cmp_jmp_eq_47",
    "module_ir_cmp_jmp_eq_48",
    "module_ir_cmp_jmp_eq_49",
    "module_ir_cmp_jmp_eq_50",
    "module_ir_cmp_jmp_eq_51",
    "module_ir_cmp_jmp_eq_52",
    "module_ir_cmp_jmp_eq_53",
    "module_ir_cmp_jmp_eq_54",
    "module_ir_cmp_jmp_eq_55",
    "module_ir_cmp_jmp_eq_56",
    "attribute_ir_cmp_jmp_eq_57",
    "module_ir_cmp_jmp_eq_58",
    "module_ir_cmp_jmp_eq_59",
    "module_ir_cmp_jmp_eq_60",
    "module_ir_cmp_jmp_eq_61",
    "module_ir_cmp_jmp_eq_62",
    "module_ir_cmp_jmp_eq_63",
    "module_ir_cmp_jmp_eq_64",
    "module_ir_cmp_jmp_eq_65",
    "module_ir_cmp_jmp_eq_66",
    "module_ir_cmp_jmp_eq_67",
    "module_ir_cmp_jmp_eq_68",
    "module_ir_cmp_jmp_eq_69",
    "ir_func_end",
    "ir_push_str",
    "ir_push_str_2",
    "ir_push_str_3",
    "ir_alloca",
    "ir_push_var",
    "ir_push_var_2",
    "ir_push_var_3",
    "ir_push_var_4",
    "ir_push_var_5",
    "ir_push_var_6",
    "ir_push_var_7",
    "ir_push_var_8",
    "ir_push_global",
    "ir_push_global_2",
    "ir_push_global_3",
    "ir_trunc",
    "ir_trunc_2",
    "ir_trunc_3",
    "ir_trunc_4",
    "ir_trunc_5",
    "ir_trunc_6",
    "ir_trunc_7",
    "ir_trunc_8",
    "ir_push_func",
    "ir_push_func_2",
    "ir_store_global",
    "ir_pop",
    "ir_dup",
    "ir_swap",
    "ir_store",
    "ir_store_2",
    "ir_store_3",
    "ir_store_4",
    "ir_store_5",
    "ir_load_idx",
    "ir_load_idx_2",
    "ir_load_idx_3",
    "ir_load_idx_4",
    "ir_load_idx_5",
    "ir_load_idx_6",
    "ir_load_idx_7",
    "ir_store_idx",
    "ir_store_idx_2",
    "ir_store_idx_3",
    "ir_store_idx_4",
    "ir_store_idx_5",
    "ir_store_idx_6",
    "ir_store_idx_7",
    "ir_store_idx_8",
    "ir_binary",
    "ir_fadd",
    "ir_fadd_2",
    "ir_fadd_3",
    "ir_fsub",
    "ir_fsub_2",
    "ir_fmul",
    "ir_fmul_2",
    "ir_fdiv",
    "ir_fdiv_2",
    "ir_fcmp",
    "tok_ne_2",
    "tok_lt_3",
    "tok_gt_3",
    "tok_le_3",
    "abi_tok_ge_38",
    "abi_tok_ge_39",
    "abi_tok_ge_40",
    "abi_tok_ge_41",
    "abi_tok_ge_42",
    "ir_f2i",
    "ir_f2i_2",
    "ir_f2i_3",
    "ir_f2i_4",
    "ir_f2i_5",
    "ir_i2f",
    "ir_i2f_2",
    "ir_i2f_3",
    "ir_i2f_4",
    "ir_64i2f",
    "ir_64f2i",
    "ir_f2f",
    "ir_asm",
    "ir_unary",
    "ir_unary_2",
    "ir_unary_3",
    "ir_unary_4",
    "ir_unary_5",
    "ir_unary_6",
    "ir_label_named",
    "ir_label_named_2",
    "ir_jmp",
    "ir_jmp_named",
    "ir_jz",
    "ir_jnz",
    "module_ir_cmp_jmp_eq_70",
    "ir_call",
    "ir_call_2",
    "ir_call_3",
    "ir_call_4",
    "ir_call_5",
    "ir_call_6",
    "ir_call_7",
    "ir_call_8",
    "ir_call_9",
    "ir_call_10",
    "ir_call_11",
    "ir_call_12",
    "ir_call_13",
    "ir_call_14",
    "ir_call_15",
    "ir_call_16",
    "ir_call_17",
    "ir_call_18",
    "ir_call_19",
    "ir_call_20",
    "ir_call_21",
    "ir_call_22",
    "ir_call_23",
    "ir_call_24",
    "ir_call_25",
    "ir_call_26",
    "ir_call_27",
    "ir_call_28",
    "ir_call_29",
    "ir_call_30",
    "ir_call_31",
    "ir_call_32",
    "ir_call_33",
    "ir_call_34",
    "ir_svc",
    "ir_svc_2",
    "ir_div",
    "ir_div_2",
    "ir_div_3",
    "ir_mod",
    "ir_mod_2",
    "ir_64push",
    "ir_64cmp",
    "ir_64cmp_2",
    "ir_64cmp_3",
    "ir_64cmp_4",
    "abi_tok_ge_43",
    "ir_load_fld",
    "ir_load_fld_2",
    "ir_load_fld_3",
    "ir_load_fld_4",
    "ir_return",
    "ir_simd_add16",
    "ir_simd_sub16",
    "ir_simd_add8",
    "ir_simd_sub8",
    "ir_simd_sub8_2",
    "ir_simd_sub8_3",
    "ir_simd_sub8_4",
};
/* Bounded semantic record table. Records describe lowering selection, complete
 * sequence reference and actual C-slot effect, not fragments of operator text. */
#define C_SELECT_LIMIT 192
typedef struct { char family[12], op[12], sign[12], key[96]; int width, pop, push; } CSelect;
static CSelect selectors[C_SELECT_LIMIT];
static int selector_count;
static char operands[6][32];
static int semantic_word(const char **cursor, char *out, int cap) {
    const char *p = *cursor; int n = 0;
    while (*p == ' ' || *p == '\t') p++;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n') {
        if (n >= cap - 1) return -1;
        out[n++] = *p++;
    }
    out[n] = 0; *cursor = p; return n;
}
static int semantic_number(const char *p, int max, int *out) {
    int n = 0;
    if (!*p) return -1;
    while (*p) { if (*p < '0' || *p > '9' || n > max) return -1; n = n*10 + *p++-'0'; }
    if (n > max) return -1; *out = n; return 0;
}
static int semantic_load(void) {
    const char *p = xh_template_get("c", "semantic.selectors");
    const char *args; int i;
    selector_count = 0;
    if (!p || strncmp(p, "CSELECT 1\n", 10)) goto invalid;
    p += 10;
    while (*p) {
        CSelect *record; char width[8], pop[8], push[8], extra[2];
        if (selector_count >= C_SELECT_LIMIT || *p == '\n') goto invalid;
        record = &selectors[selector_count];
        if (semantic_word(&p,record->family,sizeof(record->family)) < 1 ||
            semantic_word(&p,record->op,sizeof(record->op)) < 1 ||
            semantic_word(&p,width,sizeof(width)) < 1 ||
            semantic_word(&p,record->sign,sizeof(record->sign)) < 1 ||
            semantic_word(&p,record->key,sizeof(record->key)) < 1 ||
            semantic_word(&p,pop,sizeof(pop)) < 1 ||
            semantic_word(&p,push,sizeof(push)) < 1 ||
            semantic_word(&p,extra,sizeof(extra)) != 0 ||
            semantic_number(width,128,&record->width) ||
            semantic_number(pop,2,&record->pop) || semantic_number(push,1,&record->push)) goto invalid;
        if ((record->width != 32 && record->width != 64 && record->width != 128) ||
            (!ark_streq(record->family,"binary") && !ark_streq(record->family,"compare") &&
             !ark_streq(record->family,"unary") && !ark_streq(record->family,"float") && !ark_streq(record->family,"direct")) ||
            (!ark_streq(record->sign,"s") && !ark_streq(record->sign,"u") &&
             !ark_streq(record->sign,"real") && !ark_streq(record->sign,"complex"))) goto invalid;
        {
            static const char *const known[] = {"ADD","SUB","MUL","DIV","MOD","BAND","BOR","BXOR","SHL","SHR","EQ","NE","LT","GT","LE","GE","NEG","BNOT","NOT","CONJ","EXP"};
            int found = 0, j;
            for (j=0;j<(int)(sizeof(known)/sizeof(known[0]));j++) if (ark_streq(record->op,known[j])) found = 1;
            if (!found) goto invalid;
        }
        if (record->push != 1 || record->pop != (ark_streq(record->family,"unary") || ark_streq(record->op,"EXP") ? 1 : 2)) goto invalid;
        for (i=0;i<selector_count;i++) if (record->width == selectors[i].width &&
            ark_streq(record->family,selectors[i].family) && ark_streq(record->op,selectors[i].op) && ark_streq(record->sign,selectors[i].sign)) goto invalid;
        if (!xh_template_get("c",record->key)) goto invalid;
        selector_count++;
        if (*p == '\n') p++; else if (*p) goto invalid;
    }
    if (!selector_count) goto invalid;
    args = xh_template_get("c","semantic.operands");
    if (!args) goto invalid;
    for (i=0;i<6;i++) {
        int j;
        if (semantic_word(&args,operands[i],sizeof(operands[i])) < 1) goto invalid;
        for (j=0;operands[i][j];j++) if (!((operands[i][j]>='a'&&operands[i][j]<='z') ||
            (operands[i][j]>='A'&&operands[i][j]<='Z') || operands[i][j]=='_' ||
            (j>0 && operands[i][j]>='0'&&operands[i][j]<='9'))) goto invalid;
    }
    { char extra[2]; if (semantic_word(&args,extra,sizeof(extra)) != 0) goto invalid; }
    return 0;
invalid:
    xh_c_rules_error("invalid C semantic selector records or stack effects");
    return -1;
}
int xh_c_semantic_select(const char *family,const char *operation,int width,const char *sign,AsmBuffer *output,int *delta) {
    int i;
    for (i=0;i<selector_count;i++) {
        CSelect *r = &selectors[i];
        if (r->width == width && ark_streq(r->family,family) && ark_streq(r->op,operation) && ark_streq(r->sign,sign)) {
            const char *args[6]; int j;
            *delta = r->push-r->pop;
            if (!output) return 0;
            for (j=0;j<6;j++) args[j] = operands[j];
            return xh_c_rule_emit(output,r->key,args,6);
        }
    }
    xh_c_rules_error("required C semantic lowering selection is missing");
    return -1;
}

int xh_c_rules_begin(void) {
    int i;
    c_failed = 0;
    for (i = 0; i < (int)(sizeof(required)/sizeof(required[0])); i++) {
        if (!xh_template_get("c", required[i])) { c_failed = 1; return -1; }
    }
    if (load_abi() || semantic_load()) return -1;
    /* This data record is indexed by a nibble in the literal encoder. */
    if (ark_strlen(xh_template_get("c", "escape_hex_digits")) != 16) {
        diag_tool(DIAG_SEV_ERROR, "c templates", "hex digit template must contain exactly 16 bytes", 0,0,0,0,0,0);
        c_failed = 1;
        return -1;
    }
    return 0;
}
const char *xh_c_rule_text(const char *key) {
    const char *text = xh_template_get("c", key);
    if (!text) { c_failed = 1; return ""; }
    return text;
}
int xh_c_rule_emit(AsmBuffer *out, const char *key, const char *const *args, int count) {
    /* The largest fixed runtime wrapper fragment is <32K. Dynamic arguments
     * are bounded identifiers/numbers, never a whole translation unit. */
    static char text[16384];
    int n = xh_template_emit("c", key, args, count, text, sizeof(text));
    if (n < 0) { c_failed = 1; return -1; }
    asmbuf_puts(out, text);
    return 0;
}
int xh_c_rule_ints(AsmBuffer *out, const char *key, long long a, long long b) {
    char aa[32], bb[32];
    const char *args[2];
    snprintf(aa, sizeof(aa), "%lld", a);
    snprintf(bb, sizeof(bb), "%lld", b);
    args[0] = aa; args[1] = bb;
    return xh_c_rule_emit(out, key, args, 2);
}
int xh_c_rule_member(const char *key, const char *name) {
    const char *p = xh_c_rule_text(key);
    int n, i;
    if (!name) return 0;
    n = ark_strlen(name);
    while (*p) {
        while (*p == ' ' || *p == '\n' || *p == '\t') p++;
        for (i = 0; i < n && p[i] && p[i] == name[i]; i++) {}
        if (i == n && (!p[i] || p[i] == ' ' || p[i] == '\n' || p[i] == '\t')) return 1;
        while (*p && *p != ' ' && *p != '\n' && *p != '\t') p++;
    }
    return 0;
}
const char *xh_c_rule_map(const char *key, const char *name) {
    static char value[96];
    const char *p = xh_c_rule_text(key);
    int n = name ? ark_strlen(name) : 0;
    while (*p) {
        const char *start = p;
        int k = 0, matched;
        while (*p && *p != '=' && *p != ' ') p++;
        matched = (p - start == n);
        while (matched && k < n) { if (start[k] != name[k]) matched = 0; k++; }
        if (*p != '=') break;
        start = ++p;
        while (*p && *p != ' ') p++;
        if (matched) {
            int len = (int)(p - start), i;
            if (len < 1 || len >= (int)sizeof(value)) break;
            for (i = 0; i < len; i++) value[i] = start[i];
            value[len] = 0;
            return value;
        }
        while (*p == ' ') p++;
    }
    diag_tool(DIAG_SEV_ERROR, "c templates", "missing or invalid C ABI return type mapping", key,name,0,0,0,0);
    c_failed = 1;
    return "";
}
int xh_c_rule_copy(char *out, int capacity, const char *key) {
    int n = xh_template_emit("c", key, 0, 0, out, capacity);
    if (n < 0) { c_failed = 1; if (capacity > 0) out[0] = 0; return -1; }
    return 0;
}
void xh_c_rules_error(const char *message) {
    diag_tool(DIAG_SEV_ERROR, "c backend", message, 0,0,0,0,0,0);
    c_failed = 1;
}
int xh_c_rules_failed(void) { return c_failed; }
