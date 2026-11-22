#ifndef XH_RV_RULES_H
#define XH_RV_RULES_H
#include "codegen.h"
void xh_rv_rules_reset(void);
const char *xh_rv_rule_text(const char *key);
int xh_rv_rules_failed(void);
void xh_rv_rule(AsmBuffer *buffer,const char *key);
void xh_rv_rule_args(AsmBuffer *buffer,const char *key,const char *const *args,int count);
void xh_rv_rule_int(AsmBuffer *buffer,const char *key,int value);
void xh_rv_rule_four(AsmBuffer *buffer,const char *key,int a,int b,int c,int d);
#endif
