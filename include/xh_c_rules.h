#ifndef XH_C_RULES_H
#define XH_C_RULES_H
#include "codegen.h"
/* The C backend has no built-in template fallback. Text pointers belong to
 * the shared VM cache and remain valid until its target/path changes. */
typedef struct {
    int pointer_bits, slot_bytes, max_call_args, wrapper_args;
    int frame_align, frame_padding, frame_min, varargs_offset, struct_return_offset;
    int stack_min, stack_margin, return_buffer_bytes;
} XhCAbi;
const XhCAbi *xh_c_abi(void);
int xh_c_rules_begin(void);
const char *xh_c_rule_text(const char *key);
int xh_c_rule_emit(AsmBuffer *out, const char *key, const char *const *args, int count);
int xh_c_rule_ints(AsmBuffer *out, const char *key, long long a, long long b);
int xh_c_rule_member(const char *key, const char *name);
const char *xh_c_rule_map(const char *key, const char *name);
int xh_c_rule_copy(char *out, int capacity, const char *key);
int xh_c_rules_failed(void);
#endif
