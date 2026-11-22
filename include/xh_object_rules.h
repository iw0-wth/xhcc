#ifndef XH_OBJECT_RULES_H
#define XH_OBJECT_RULES_H
#define XH_OBJECT_MAX_RELOCS 64
#define XH_OBJECT_MAX_ATTRS 128
typedef struct {unsigned internal,elf;} XhObjectReloc;
typedef struct {
 char target[32],abi[32];unsigned machine,elf_class,endian,flags;
 unsigned char attributes[XH_OBJECT_MAX_ATTRS];unsigned attributes_size;
 XhObjectReloc relocations[XH_OBJECT_MAX_RELOCS];int relocation_count;
} XhObjectTarget;
int xh_object_rules_set_path(const char *target,const char *path);
const XhObjectTarget *xh_object_rules_get(const char *target);
int xh_object_rules_relocation(const XhObjectTarget *target,unsigned internal,unsigned *elf);
void xh_object_rules_release(void);
#endif
