#ifndef XH_ARM_ELF_RELOC_H
#define XH_ARM_ELF_RELOC_H
#include <stddef.h>
#include <stdint.h>
/* Little-endian ARM ELF32 REL subset. Output parameters remain unchanged on
 * failure. MOVW/MOVT are independent 4-byte relocations, not paired fixups.
 * PREL31 (42) sign-extends the encoded low31 addend, preserves bit31, and
 * checks ((S+A)|T)-P fits signed31. All apply failures preserve input bytes. */
int xh_arm_elf_rel_decode(unsigned type,const unsigned char *code,size_t length,size_t site,int64_t *addend,unsigned *width);
/* symbol_thumb explicitly distinguishes Thumb function state from odd data
 * addresses. Only when true, symbol bit0 is cleared before S+A arithmetic.
 * Branches require Thumb state;
 * ARM-state interworking, BLX and veneers are explicitly unsupported here.
 * place is the address of the first relocated byte, without a PC bias. */
int xh_arm_elf_rel_apply(unsigned type,unsigned char *code,size_t length,size_t site,uint32_t symbol,uint32_t place,unsigned symbol_thumb);
#endif
