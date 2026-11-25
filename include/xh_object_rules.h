#ifndef XH_OBJECT_RULES_H
#define XH_OBJECT_RULES_H
#define XH_OBJECT_MAX_RELOCS 64
#define XH_OBJECT_MAX_ATTRS 128
#define XH_OBJECT_MAX_ATTRIBUTE_PROFILES 4
typedef struct {char name[24];unsigned char bytes[XH_OBJECT_MAX_ATTRS];unsigned size;} XhObjectAttributeProfile;
typedef struct {unsigned internal,elf;} XhObjectReloc;
typedef struct {
 char target[32],abi[32];unsigned machine,elf_class,endian,flags;
 unsigned char attributes[XH_OBJECT_MAX_ATTRS];unsigned attributes_size;
 XhObjectReloc relocations[XH_OBJECT_MAX_RELOCS];int relocation_count;
 XhObjectAttributeProfile attribute_profiles[XH_OBJECT_MAX_ATTRIBUTE_PROFILES];unsigned attribute_profile_count;
} XhObjectTarget;
int xh_object_rules_set_path(const char *target,const char *path);
const XhObjectTarget *xh_object_rules_get(const char *target);
int xh_object_rules_relocation(const XhObjectTarget *target,unsigned internal,unsigned *elf);
/* Profile lookup returns descriptor-owned bytes. required=1 rejects absent
 * profile; required=0 falls back to the legacy default attributes field. */
int xh_object_rules_attributes_for_profile(const XhObjectTarget *target,const char *profile,int required,const unsigned char **bytes,unsigned *size);
void xh_object_rules_release(void);
#endif
