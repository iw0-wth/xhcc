/* Exact target-word stack validation for legacy IR. No allocation or mutation.
 * Activate only for functions containing ALLOCA until a target certifies its
 * complete effect table; unsupported operations must never default to zero.
 * This is not the one-value/one-slot C backend or XHIRCORE contract. */
#ifndef XH_IR_STACK_H
#define XH_IR_STACK_H
#include "ir.h"

typedef enum {
    XH_STACK_NEXT, XH_STACK_STOP, XH_STACK_JUMP, XH_STACK_BRANCH
} XhStackFlow;
typedef struct {
    int pop, push;                 /* fallthrough effect (or sole edge) */
    int taken_pop, taken_push;     /* BRANCH taken edge, from entry depth */
    XhStackFlow flow;
} XhStackEffect;
/* Return zero only for a completely understood opcode AND operand encoding.
 * Callback must not mutate IR. For JUMP/BRANCH, legacy op identifies target:
 * JMP_NAMED uses name; CMP_JMP_EQ uses aux; JMP/JZ/JNZ use val. */
typedef int (*XhStackEffectFn)(void *context, const IrFunc *function,
                             int instruction, XhStackEffect *effect);
typedef enum {
    XH_STACK_OK, XH_STACK_BAD_INPUT, XH_STACK_LIMIT,
    XH_STACK_BAD_LABEL, XH_STACK_DUPLICATE_LABEL, XH_STACK_BAD_TARGET,
    XH_STACK_UNSUPPORTED, XH_STACK_BAD_EFFECT, XH_STACK_UNDERFLOW,
    XH_STACK_JOIN_MISMATCH, XH_STACK_BAD_ALLOCA
} XhStackStatus;
typedef struct { XhStackStatus status; int instruction; } XhStackError;
typedef struct {
    int *depth;                    /* n entries: entry words, -1 unreachable */
    int *queue;                    /* n entries: private worklist scratch */
    int *target;                   /* n entries: private resolved edges */
    int capacity;
    int max_words;                 /* positive caller-selected depth budget */
} XhStackWorkspace;
/* All labels/branch targets and all effects (including unreachable code) are
 * checked. Reachable edges require one identical depth at every join/cycle.
 * No pointers escape, and workspace can be reused immediately after emission.
 * Complexity: O(n) space, O(n^2) label resolution, O(n) worklist propagation.
 * Callback is invoked twice for reachable instructions; it must be stable. */
XhStackStatus xh_ir_stack_validate(const IrFunc *function,
    XhStackEffectFn effect, void *context, XhStackWorkspace *workspace,
    XhStackError *error);
/* After successful validation only: depth[i]-1, or -1 if unreachable/not ALLOCA.
 * The ALLOCA emitter must reject -1 or omit unreachable allocation emission. */
int xh_ir_stack_alloca_live_words(const IrFunc *function,
    const XhStackWorkspace *workspace, int instruction);
#endif
