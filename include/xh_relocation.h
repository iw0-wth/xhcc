#ifndef XH_RELOCATION_H
#define XH_RELOCATION_H
#include <stdint.h>
#include <stddef.h>
typedef struct {uint64_t symbol,addend,place,image_base;} XhRelocContext;
int xh_relocation_set_path(const char *target,const char *path);
int xh_relocation_load(const char *target);
int xh_relocation_apply(const char *target,const char *rule,unsigned char *code,size_t length,size_t site,const XhRelocContext *context);
void xh_relocation_release(void);
#endif
