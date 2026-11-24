/* Explicit compatibility boundary. This header imports the legacy C graph. */
#ifndef XH_IR_LEGACY_ADAPTER_H
#define XH_IR_LEGACY_ADAPTER_H
#include "xh_ir_contract.h"
#include "ir.h"
/* Unknown tokens/operators return INVALID / -1. No token numbers in core. */
XhIrBinaryOp xh_ir_binary_from_c_token(int token);
int xh_ir_binary_to_c_token(XhIrBinaryOp operation);
/* Allocates legacy IR only after core validation. Supports v1 executable
 * profile; name must be an externally supplied backend-safe symbol name.
 * Caller owns the legacy arena lifetime. No existing compiler path calls this. */
IrProgram *xh_ir_core_to_legacy(const XhIrCoreProgram *core, const char *name,
    XhIrCoreError *error);
#endif
