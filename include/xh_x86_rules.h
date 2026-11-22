#ifndef XH_X86_RULES_H
#define XH_X86_RULES_H
/* Adapter to the shared bounded external template VM. No built-in rules. */
enum { XH_ABI_VOID, XH_ABI_INTEGER, XH_ABI_FLOAT, XH_ABI_POINTER, XH_ABI_AGGREGATE };
typedef struct { int bank,words,width,alignment,indirect; } XhX86AbiClass;
int xh_x86_classify(int result,int kind,int size,int alignment,int variadic,int index,XhX86AbiClass *out);
int xh_x86_allocate(const XhX86AbiClass *value,int *integer_used,int *vector_used,int *stack_used,int *where);
int xh_x86_rules_begin(int mode64);
const char *xh_x86_rule(const char *key);
const char *xh_x86_expand(const char *key,const char *const *args,int count);
int xh_x86_number(const char *key);
const char *xh_x86_semantic(const char *family,const char *operation,int width,const char *sign,const char *form,int pop,int push,const char *const *args,int count);
/* Finite aggregate wire-size classifier; returns 4/8 or pointer width. */
int xh_x86_aggregate_wire(int bytes);
int xh_x86_register(const char *name, int *width);
int xh_x86_rules_failed(void);
#endif
