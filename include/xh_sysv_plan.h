#ifndef XH_SYSV_PLAN_H
#define XH_SYSV_PLAN_H
#include "type.h"
#include "xh_abiplan.h"
#define XH_SYSV_PLAN_VERSION 2
#define XH_SYSV_PLAN_SCALAR_VERIFIED 1u
#define XH_SYSV_PLAN_SMALL_INTEGER_AGGREGATE_VERIFIED 2u
#define XH_SYSV_PLAN_SMALL_SSE_AGGREGATE_VERIFIED 4u
#define XH_SYSV_PLAN_INTEGER_PAIR_AGGREGATE_VERIFIED 8u
#define XH_SYSV_PLAN_MIXED_PAIR_AGGREGATE_VERIFIED 16u
int xh_sysv_mixed_pair_struct(Type *type);
int xh_sysv_integer_pair_struct(Type *type);
int xh_sysv_small_sse_struct(Type *type);
/* Source-type certification; rejects unions, packed/bitfield/FP structs. */
int xh_sysv_small_integer_struct(Type *type);
#define XH_SYSV_ABI_SUBSET 1
typedef struct {unsigned version,abi_id,rule_fingerprint,arg_count,stack_bytes,sret,flags;XhTypeclassResult result;XhAbiArgumentPlan args[XH_ABIPLAN_MAX_ARGUMENTS];} XhSysvPlan;
/* Builds persistent plans. Default codegen retains the private wire; explicit
 * plan-use mode consumes v2 flags 1 (scalar), 2 (also natural 1..8-byte
 * integer/pointer structs), or 4 (also a single-eightbyte SSE struct: one
 * float, two floats, or one double). No unions, packed/bitfields, arrays,
 * mixed integer/FP struct members or variadic aggregate actuals. Other plans remain
 * inspection-only; this is not full native aggregate ABI lowering.
 * Function must have a complete prototype. actual_types may be NULL for named
 * parameter planning; otherwise includes promoted variadic argument types.
 * Type graph rejects bitfields, complex and unsupported leaves. Atomic output. */
int xh_sysv_plan_set_path(const char *path);
int xh_build_sysv_plan(const char *rule_target,const char *prefix,Type *function,Type *const *actual_types,unsigned count,XhSysvPlan *out);
/* Canonical wire codec: little-endian u32 fields, no host struct dump.
 * size returns bytes or 0 invalid. encode returns byte count or -1;
 * decode returns 0/-1, exact input length required, output unchanged on error. */
unsigned xh_sysv_plan_wire_size(const XhSysvPlan *plan);
int xh_sysv_plan_encode(const XhSysvPlan *plan,unsigned char *out,unsigned capacity);
int xh_sysv_plan_decode(const unsigned char *data,unsigned length,XhSysvPlan *out);
#endif
