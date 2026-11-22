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
int xh_c_semantic_bind(const char *family,const char *operation,int width,const char *sign,
                       AsmBuffer *output,int *delta,const char *const *bindings,int count);
int xh_c_semantic_select(const char *family, const char *operation, int width,
                         const char *sign, AsmBuffer *output, int *delta);
const char *xh_c_rule_text(const char *key);
int xh_c_rule_emit(AsmBuffer *out, const char *key, const char *const *args, int count);
int xh_c_rule_ints(AsmBuffer *out, const char *key, long long a, long long b);
int xh_c_rule_member(const char *key, const char *name);
const char *xh_c_rule_map(const char *key, const char *name);
int xh_c_rule_find(const char *key,const char *name,char *out,int cap);
int xh_c_call_effect(const char *route,const char *result,int *pop,int *push);
void xh_c_effect_set(int pop,int push);
void xh_c_effect_get(int *pop,int *push);
int xh_c_call_piece(const char *family,const char *operation,const char *const *args,char *out,int capacity);
int xh_c_rule_copy(char *out, int capacity, const char *key);
void xh_c_rules_error(const char *message);
int xh_c_rules_failed(void);
#endif
