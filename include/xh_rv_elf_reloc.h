/* Independent RV32 ELF RELA application; no private-object addend conventions. */
#ifndef XH_RV_ELF_RELOC_H
#define XH_RV_ELF_RELOC_H
#include <stdint.h>
#include <stddef.h>
typedef struct {
 uint32_t symbol,place;int32_t addend;
 uint32_t pair_symbol,pair_place;int32_t pair_addend;unsigned has_pair;
} XhRvElfRelContext;
/* NONE/RELAX width0. CALL/CALL_PLT width8, other accepted writes width4.
 * RELAX association is validated by the caller; no independent patch occurs. */
int xh_rv_elf_rela_width(unsigned type,unsigned *width);
/* PCREL_LO uses a resolved matching HI20 target/site/addend in pair_*.
 * The caller resolves its local anchor and requires the LO's addend be zero.
 * All patch failures leave bytes unchanged. No relaxation or RVC expansion. */
int xh_rv_elf_rela_apply(unsigned type,unsigned char *code,size_t length,size_t site,const XhRvElfRelContext *context);
#endif
