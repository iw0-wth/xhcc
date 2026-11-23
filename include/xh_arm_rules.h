#ifndef XH_ARM_RULES_H
#define XH_ARM_RULES_H
#include "codegen.h"
/* ARM text emission uses the common bounded external template VM.  Selection,
 * register/ABI bookkeeping, optimizers and Thumb encoding remain in C. */
typedef struct {int word_bytes,core_regs,pair_words,pair_align,stack_align,saved_frame,sret_words;int scalar32_size,scalar32_align,scalar64_size,scalar64_align,indirect_limit,indirect_size,indirect_align;} XhArmAbi;
const XhArmAbi *xh_arm_abi(void);
/* External finite memory-form policy: returns 2 or 4-byte form, -1 for invalid. */
int xh_arm_mem_form(int kind,int is_load,int rt,int rn,int rm,int imm,int wb,int shift);
void xh_arm_rules_reset(void);
int xh_arm_rules_failed(void);
void xh_arm_semantic(AsmBuffer *b,const char *family,const char *operation,int width,const char *sign,const char *form,int pop,int push);
void xh_arm_semantic_args(AsmBuffer *b,const char *family,const char *operation,int width,const char *sign,const char *form,int pop,int push,const char *const *args,int count);
void xh_arm_rule(AsmBuffer *b,const char *key);
void xh_arm_rule_args(AsmBuffer *b,const char *key,const char *const *args,int count);
void xh_arm_rule_ints(AsmBuffer *b,const char *key,int a,int c,int d);
#endif
