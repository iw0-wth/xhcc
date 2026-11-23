#ifndef XH_ABIPLAN_H
#define XH_ABIPLAN_H
#include "xh_typeclass.h"
/* Bounded argument-placement mechanism. Persisted scalar plans may be
 * consumed by opt-in AMD64 codegen; aggregate placement remains inspection.
 * V1 supports two independently allocated register banks and <=2 fragments.
 * No wire serialization is implied by these C structs. */
#define XH_ABIPLAN_VERSION 1
#define XH_ABIPLAN_MAX_ARGUMENTS 64
#define XH_ABIPLAN_MAX_CLASSES 8
#define XH_ABIPLAN_BANK_NONE 2
typedef struct {unsigned bank,demand,share_previous;} XhAbiPlanClass;
typedef struct {
 unsigned version,capacity[2],stack_slot,call_alignment,max_alignment,sret_bank,sret_count,class_count;
 XhAbiPlanClass classes[XH_ABIPLAN_MAX_CLASSES];
} XhAbiPlanPolicy;
typedef struct {unsigned version,used[2],stack_size,argument_count,sret_reserved;} XhAbiPlanState;
typedef struct {unsigned object_offset,size,bank,index,stack_offset;} XhAbiPlanLocation;
typedef struct {
 unsigned version,by_value_stack,size,alignment,count,stack_offset,stack_extent;
 XhAbiPlanLocation fragments[XH_TYPECLASS_MAX_FRAGMENTS];
} XhAbiArgumentPlan;
/* Policy uses shared XHTEMPLATE alias/prefix. Callers explicitly configure
 * its path, e.g. xh_typeclass_set_path("x86_64",aggregate_rules).
 * Any shared-template target switch invalidates earlier raw template pointers. */
int xh_abiplan_load(const char *target,const char *prefix,XhAbiPlanPolicy *out);
/* Each operation is atomic: on error state/output remain byte-for-byte intact.
 * sret reserves policy.sret_count register slots in policy.sret_bank. */
int xh_abiplan_begin(const XhAbiPlanPolicy *policy,int sret,XhAbiPlanState *state);
int xh_abiplan_argument(const XhAbiPlanPolicy *policy,XhAbiPlanState *state,const XhTypeclassResult *classification,unsigned size,unsigned alignment,XhAbiArgumentPlan *out);
/* Stateless metadata consistency check, not cryptographic certification.
 * class_proof is NULL or arg_count*2 class IDs for emitted locations; only
 * explicit proof permits policy.share_previous within one argument. Without
 * proof every repeated register index is rejected. Never mutates inputs. */
int xh_abiplan_validate_layout(const XhAbiPlanPolicy *policy,const XhAbiArgumentPlan *args,unsigned arg_count,unsigned stack_bytes,unsigned sret,const XhTypeclassResult *result,const unsigned *class_proof);
int xh_abiplan_finish(const XhAbiPlanPolicy *policy,const XhAbiPlanState *state,unsigned *stack_bytes);
#endif
