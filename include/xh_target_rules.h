#ifndef XH_TARGET_RULES_H
#define XH_TARGET_RULES_H
#define XH_TARGET_MAX_MACROS 64
typedef struct {char name[64],value[96];} XhTargetMacro;
typedef struct {
 char id[64],engine[32],abi[32],va_mode[24];
 unsigned pointer_bytes,long_bytes,int_bytes,long_long_bytes,slot_bytes;
 unsigned vararg_words,complex_enabled,object_machine,object_class,little_endian;
 unsigned armcc_asm,native_asm,arm_immediates;
 unsigned complex_words,slot64_ptrs,ir_word_bytes;
 int macro_count;XhTargetMacro macros[XH_TARGET_MAX_MACROS];
} XhTargetDescriptor;
int xh_target_rules_set_path(const char *engine,const char *path);
const XhTargetDescriptor *xh_target_rules_get(const char *engine);
/* Validate against currently implemented engine data models/ABI contracts.
 * Custom IDs/macros are supported; a descriptor cannot invent a new ABI. */
int xh_target_rules_validate(const XhTargetDescriptor *descriptor,const char *engine);
void xh_target_rules_release(void);
#endif
