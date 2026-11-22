#ifndef XH_ARM_RULES_H
#define XH_ARM_RULES_H
#include "codegen.h"
/* ARM text emission uses the common bounded external template VM.  Selection,
 * register/ABI bookkeeping, optimizers and Thumb encoding remain in C. */
void xh_arm_rules_reset(void);
int xh_arm_rules_failed(void);
void xh_arm_rule(AsmBuffer *b,const char *key);
void xh_arm_rule_args(AsmBuffer *b,const char *key,const char *const *args,int count);
void xh_arm_rule_ints(AsmBuffer *b,const char *key,int a,int c,int d);
#endif
