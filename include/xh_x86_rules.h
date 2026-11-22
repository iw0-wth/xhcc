#ifndef XH_X86_RULES_H
#define XH_X86_RULES_H
/* Adapter to the shared bounded external template VM. No built-in rules. */
int xh_x86_rules_begin(int mode64);
const char *xh_x86_rule(const char *key);
const char *xh_x86_expand(const char *key,const char *const *args,int count);
int xh_x86_number(const char *key);
/* Finite aggregate wire-size classifier; returns 4/8 or pointer width. */
int xh_x86_aggregate_wire(int bytes);
int xh_x86_register(const char *name, int *width);
int xh_x86_rules_failed(void);
#endif
