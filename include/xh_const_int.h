/* Checked target-typed integer AST evaluation shared by frontend consumers. */
#ifndef XH_CONST_INT_H
#define XH_CONST_INT_H
#include "ast.h"
typedef struct XhConstInt { unsigned long long bits; Type *type; } XhConstInt;
/* 1: constant, 0: unsupported/nonconstant, -1: evaluated arithmetic is invalid.
 * bits is masked to type_size(type); casts and unsigned arithmetic wrap there.
 * No address or floating expressions are accepted. Only selected ?: / logical
 * operands are evaluated. Consumers must preserve their existing error path on
 * failure, never substitute a zero. Legacy eval_const_expr remains unchanged.
 * The parser's local bitwidth checker predates this API; migrate it separately
 * after preserving its deliberately narrower accepted grammar and diagnostics. */
int xh_const_int_eval(AstNode *node, XhConstInt *out);
/* Signed interpretation, including unsigned high-bit patterns for text output.
 * agginit consumes signed literals as the same bits; result.type retains the
 * original unsignedness for consumers that require typed C serialization. */
long long xh_const_int_signed(XhConstInt value);
#endif
