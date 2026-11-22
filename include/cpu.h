/* Ark C Compiler - cpu.h
 * Target CPU selection: query CPU info and apply predefined macros based on --cpu/-mcpu.
 * Covers common armcc 5 Cortex-M series: M0/M0+/M1/M3/M4/M7/M23/M33.
 */
#ifndef ARK_CPU_H
#define ARK_CPU_H

#include "common.h"

typedef struct {
    const char *name;       /* "Cortex-M0", "Cortex-M3", "Cortex-M4", "Cortex-M7" etc. */
    int arch_m;             /* __TARGET_ARCH_6_M, __TARGET_ARCH_7_M, __TARGET_ARCH_7E_M */
    int arch_em;            /* __TARGET_ARCH_7E_M */
    int has_dsp;            /* __TARGET_FEATURE_DSP */
    int has_fpu;            /* __TARGET_FPU_VFP */
    int has_fpu_dp;         /* double-precision FPU */
    int thumb_only;         /* Thumb-only instruction set */
    int thumb2;            /* backend ISA capability; Thumb-1 remains unsupported */
    int fpu_version;       /* 0/4/5, independent of CPU marketing name */
    const char *fpu_name;   /* "VFPv4-SP", "VFPv5-D16" etc. */
} CpuInfo;

/* Look up CPU info by name; returns NULL if not found.
 * Name matching is case-insensitive and supports omitting the "Cortex-" prefix (e.g. "M4" == "Cortex-M4"). */
const CpuInfo *cpu_lookup(const char *name);
/* Copy selected package path (<=511 bytes), invalidate bounded cache. NULL
 * chooses legacy packages/arm/cpus.rules. No file read until first lookup. */
int cpu_set_description_path(const char *path);

/* Set/override predefined macros based on CPU info (overrides the default hardcoded M7 values).
 * Called before preproc_process so subsequent preprocessing sees the correct __TARGET_ARCH_* macros. */
void cpu_apply_predefines(const CpuInfo *cpu);

#endif /* ARK_CPU_H */
