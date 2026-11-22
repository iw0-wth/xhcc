/* xhcc feature-complete, bounded-memory profile. Force-include before headers.
 * XH_TARGET selects the extra code generator; ARM stays for core object APIs.
 * All limits and feature defaults remain overridable with compiler -D flags.
 * Budget is linear RAM, not the size of the emitted compiler module. */
#ifndef XH_SMALL_H
#define XH_SMALL_H
#ifndef XHCC
#define XHCC 1
#endif
#define XH_TARGET_ALL 0
#define XH_TARGET_ARM 1
#define XH_TARGET_RISCV32 2
#define XH_TARGET_X86 3
#define XH_TARGET_X86_64 4
#define XH_TARGET_C 5
#ifndef XH_TARGET
#define XH_TARGET XH_TARGET_ALL
#endif
#if XH_TARGET < XH_TARGET_ALL || XH_TARGET > XH_TARGET_C
#error "XH_TARGET must be 0=ALL, 1=ARM, 2=RISCV32, 3=X86, 4=X86_64 or 5=C"
#endif
#ifndef ARKCC_PROFILE
#define ARKCC_PROFILE 0 /* Full features, bounded buffers below. */
#endif
#ifndef ARKCC_BACKEND_ARM
#define ARKCC_BACKEND_ARM 1
#endif
#ifndef ARKCC_BACKEND_X86_64
#define ARKCC_BACKEND_X86_64 (XH_TARGET == XH_TARGET_ALL || XH_TARGET == XH_TARGET_X86 || XH_TARGET == XH_TARGET_X86_64)
#endif
#ifndef ARKCC_BACKEND_RISCV32
#define ARKCC_BACKEND_RISCV32 (XH_TARGET == XH_TARGET_ALL || XH_TARGET == XH_TARGET_RISCV32)
#endif
#ifndef ARKCC_BACKEND_C
#define ARKCC_BACKEND_C (XH_TARGET == XH_TARGET_ALL || XH_TARGET == XH_TARGET_C)
#endif
#ifndef ARK_POOL_SIZE
#define ARK_POOL_SIZE (1024*1024)
#endif
#ifndef ARK_SRC_BUF_SIZE
#define ARK_SRC_BUF_SIZE (32*1024)
#endif
#ifndef ARK_ASM_BUF_SIZE
#define ARK_ASM_BUF_SIZE (128*1024)
#endif
#ifndef ARK_CODE_BUF_SIZE
#define ARK_CODE_BUF_SIZE (32*1024)
#endif
#ifndef ARK_OBJ_BUF_SIZE
#define ARK_OBJ_BUF_SIZE (64*1024)
#endif
#ifndef ARK_LINK_BUF_SIZE
#define ARK_LINK_BUF_SIZE (32*1024)
#endif
#ifndef PP_BUF_SIZE
#define PP_BUF_SIZE (64*1024)
#endif
#ifndef PP_INC_BUF
#define PP_INC_BUF (16*1024)
#endif
#ifndef PP_MAX_INC_DEPTH
#define PP_MAX_INC_DEPTH 8
#endif
#ifndef ARM_MAX_SYMBOLS
#define ARM_MAX_SYMBOLS 512
#endif
#ifndef ARM_MAX_RELOCS
#define ARM_MAX_RELOCS 512
#endif
#ifndef ARM_MAX_GVARS
#define ARM_MAX_GVARS 128
#endif
#ifndef ARK_LINK_MAX_SYMS
#define ARK_LINK_MAX_SYMS 64
#endif
#ifndef ARK_LINK_MAX_RELOCS
#define ARK_LINK_MAX_RELOCS 64
#endif
#ifndef ARK_LINK_MAX_OBJS
#define ARK_LINK_MAX_OBJS 16
#endif
#ifndef ARK_PP_MAX_INC_DIRS
#define ARK_PP_MAX_INC_DIRS 32
#endif
#ifndef ARK_MAX_PRE_DEFINES
#define ARK_MAX_PRE_DEFINES 64
#endif
#ifndef ARKCC_MACRO_BODY_MAX
#define ARKCC_MACRO_BODY_MAX (16*1024)
#endif
/* Host-full defaults restore diagnostics, DWARF, IPO/IR serialization, FPU
 * emission and optional optimizers. Runtime opt-in is still required for
 * optional passes; see config.h and docs/feature-parity.md. */
#endif
