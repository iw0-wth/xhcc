/* Bounded, allocation-free ELF32 little-endian ARM/RV32 ET_REL reader.
 * Independently implemented from the ELF wire format; no third-party code.
 * The caller owns data and must keep it alive and immutable for the view's life.
 */
#ifndef XH_ELF_READ_H
#define XH_ELF_READ_H
#include <stddef.h>
#include <stdint.h>
#define XH_ELF_MAX_SECTIONS 256u
#define XH_ELF_MAX_ALIGN 65536u
#define XH_ELF_MAX_RECORDS 65536u
#define XH_ELF_SHT_NULL 0u
#define XH_ELF_SHT_PROGBITS 1u
#define XH_ELF_SHT_SYMTAB 2u
#define XH_ELF_SHT_STRTAB 3u
#define XH_ELF_SHT_NOBITS 8u
#define XH_ELF_SHT_REL 9u
#define XH_ELF_SHT_RELA 4u
typedef enum { XH_ELF_TARGET_ARM=0, XH_ELF_TARGET_RISCV32=1 } XhElfTarget;
#define XH_ELF_SHN_UNDEF 0u
#define XH_ELF_SHN_ABS 0xfff1u
#define XH_ELF_SHN_COMMON 0xfff2u
#define XH_ELF_SHF_ALLOC 2u
#define XH_ELF_STB_LOCAL 0u
#define XH_ELF_STB_GLOBAL 1u
#define XH_ELF_STB_WEAK 2u
#define XH_ELF_STT_FUNC 2u
/* Zero means success; errors never leave an open view in *out. */
enum { XH_ELF_OK=0, XH_ELF_ERR_ARGUMENT=-1, XH_ELF_ERR_FORMAT=-2,
       XH_ELF_ERR_BOUNDS=-3, XH_ELF_ERR_UNSUPPORTED=-4,
       XH_ELF_ERR_INDEX=-5, XH_ELF_ERR_THUMB=-6 };
typedef struct {
    const unsigned char *data;
    size_t len;
    uint32_t shoff, flags;
    uint16_t shnum, shstrndx, machine;
} XhElfView;
typedef struct {
    uint32_t name_index, type, flags, addr, offset, size, link, info, align, entsize;
    const char *name;
    const unsigned char *data; /* NULL for NULL/NOBITS, including nonzero BSS. */
} XhElfSection;
typedef struct {
    uint32_t name_index, value, size;
    uint16_t shndx;
    uint8_t bind, type, other;
    const char *name;
} XhElfSymbol;
typedef struct {
    uint32_t offset, symbol;
    uint8_t type; /* Unknown wire types are preserved for the relocation decoder. */
} XhElfRel;
typedef struct {
    uint32_t offset, symbol;
    uint8_t type;
    int32_t addend;
} XhElfRela;
/* ARM uses EABI5 REL; RV32 uses soft ABI flags=0 and RELA (no RVC/RVE).
 * The legacy open entry retains its ARM-only contract. */
int xh_elf_open_target(const unsigned char *data,size_t len,XhElfTarget target,XhElfView *out);
int xh_elf_open(const unsigned char *data, size_t len, XhElfView *out);
/* Accessors require an unmodified successfully opened view and input. */
int xh_elf_section(const XhElfView *view, unsigned index, XhElfSection *out);
int xh_elf_symbol(const XhElfView *view, unsigned symtab, uint32_t index, XhElfSymbol *out);
int xh_elf_rel(const XhElfView *view, unsigned relsec, uint32_t index, XhElfRel *out);
int xh_elf_rela(const XhElfView *view,unsigned relasec,uint32_t index,XhElfRela *out);
/* Optional Cortex-M policy: ordinary defined FUNC symbols must have Thumb bit.
 * Undefined, ABS and COMMON function values are left to the caller's policy. */
int xh_elf_validate_thumb(const XhElfView *view);
#endif
