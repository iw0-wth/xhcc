#ifndef XH_TEMPLATE_H
#define XH_TEMPLATE_H
/* One lazily loaded target table. Returned pointers survive until a different
 * path/target is loaded or release is called. No executable/native evaluation. */
#define XH_TEMPLATE_MAX_BYTES (512u*1024u)
#define XH_TEMPLATE_MAX_RULES 4096
#define XH_TEMPLATE_MAX_ARGS 16
int xh_template_set_path(const char *target,const char *path);
void xh_template_release(void);
unsigned xh_template_generation(void);
/* FNV-1a identity of the complete loaded file, not a cryptographic checksum. */
unsigned xh_template_fingerprint(const char *target);
const char *xh_template_get(const char *target,const char *key);
int xh_template_emit(const char *target,const char *key,const char *const *args,int count,char *out,int cap);
#endif
