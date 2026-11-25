#ifndef XH_ARM_THUMB1_H
#define XH_ARM_THUMB1_H
#include "codegen.h"
int xh_arm_thumb1_enabled(void);
/* Restricted scalar Thumb-1 legalization. Rejects every unrecognized shape. */
int xh_arm_thumb1_legalize(const char *input,AsmBuffer *output);
#endif
