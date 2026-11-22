#ifndef XH_SEMANTIC_H
#define XH_SEMANTIC_H
/* Generic finite normalized semantic selection. Record source is a shared
 * XHTEMPLATE value, not a target-specific interpreter. No wildcards/eval. */
#define XH_SEMANTIC_MAX_RECORDS 384
typedef struct {const char *family,*operation,*sign,*form;int width;} XhSemanticInput;
typedef struct {char template_key[96];int pop,push;} XhSemanticResult;
/* 1 matched, 0 absent, -1 malformed table or invalid input. */
int xh_semantic_select(const char *target,const char *table_key,const XhSemanticInput *input,XhSemanticResult *result);
void xh_semantic_release(void);
#endif
