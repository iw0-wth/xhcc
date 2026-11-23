#ifndef XH_SYSV_PLAN_H
#define XH_SYSV_PLAN_H
#include "type.h"
#include "xh_abiplan.h"
#define XH_SYSV_PLAN_VERSION 2
#define XH_SYSV_PLAN_SCALAR_VERIFIED 1u
#define XH_SYSV_PLAN_SMALL_INTEGER_AGGREGATE_VERIFIED 2u
/* Source-type certification; rejects unions, packed/bitfield/FP structs. */
int xh_sysv_small_integer_struct(Type *type);
#define XH_SYSV_ABI_SUBSET 1
typedef struct {unsigned version,abi_id,rule_fingerprint,arg_count,stack_bytes,sret,flags;XhTypeclassResult result;XhAbiArgumentPlan args[XH_ABIPLAN_MAX_ARGUMENTS];} XhSysvPlan;
/* Builds persistent plans. Default codegen retains the private wire; explicit
 * plan-use mode consumes v2 SCALAR_VERIFIED or SMALL_INTEGER_AGGREGATE_VERIFIED
 * plans. The latter currently certifies only natural 4/8-byte integer/pointer
 * structs (including qualified nested structs), never unions, packed/bitfields,
 * floating members or variadic aggregate actuals. Other aggregate plans remain
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
