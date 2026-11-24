/* XH IR core v1: language-independent, target-neutral stack-machine contract.
 * This header deliberately depends only on ISO fixed-width integer types. */
#ifndef XH_IR_CONTRACT_H
#define XH_IR_CONTRACT_H
#include <stdint.h>

#define XH_IR_CORE_VERSION 1u
#define XH_IR_CORE_MAX_CODE 128u
#define XH_IR_CORE_MAX_STACK 64u
#define XH_IR_CORE_MAGIC "XHIRCORE"

typedef enum {
    XH_IR_VALUE_VOID = 0, XH_IR_VALUE_INTEGER = 1,
    XH_IR_VALUE_FLOAT = 2, XH_IR_VALUE_POINTER = 3,
    XH_IR_VALUE_AGGREGATE = 4
} XhIrValueCategory;
typedef enum {
    XH_IR_SIGN_NONE = 0, XH_IR_SIGN_SIGNED = 1, XH_IR_SIGN_UNSIGNED = 2
} XhIrSignedness;
/* Width/extent are bytes, never stack slots. Address space 0 is generic;
 * nonzero spaces require an explicit target capability at lowering time. */
typedef struct {
    uint32_t category, byte_width, signedness, address_space, aggregate_bytes;
} XhIrValueType;
/* Stable independent IDs; these are NOT lexer/token enum numbers. */
typedef enum {
    XH_IR_BIN_INVALID = 0,
    XH_IR_BIN_ADD = 1, XH_IR_BIN_SUB = 2, XH_IR_BIN_MUL = 3,
    XH_IR_BIN_DIV = 4, XH_IR_BIN_REM = 5,
    XH_IR_BIN_AND = 6, XH_IR_BIN_OR = 7, XH_IR_BIN_XOR = 8,
    XH_IR_BIN_SHL = 9, XH_IR_BIN_SHR = 10,
    XH_IR_BIN_EQ = 11, XH_IR_BIN_NE = 12,
    XH_IR_BIN_LT = 13, XH_IR_BIN_LE = 14,
    XH_IR_BIN_GT = 15, XH_IR_BIN_GE = 16
} XhIrBinaryOp;
typedef enum {
    XH_IR_CORE_CONST = 1, XH_IR_CORE_BINARY = 2, XH_IR_CORE_RETURN = 3
} XhIrCoreOpcode;
enum {
    XH_IR_EFFECT_PURE = 0, XH_IR_EFFECT_MAY_TRAP = 1,
    XH_IR_EFFECT_TERMINATOR = 2,
    XH_IR_EFFECT_READ_MEMORY = 4, XH_IR_EFFECT_WRITE_MEMORY = 8,
    XH_IR_EFFECT_CALL = 16
};
/* Future call operations reference an external, versioned ABI plan table.
 * This describes identity only, and never smuggles a target plan into core v1.
 * No call instruction is executable in the v1 profile below. */
typedef struct {
    uint32_t table_version, abi_id, plan_index;
} XhIrAbiPlanRef;
typedef struct {
    uint32_t opcode, binary_op;
    XhIrValueType type; /* CONST result / BINARY operands / RETURN result */
    uint64_t bits;     /* CONST raw bits; must be zero for other instructions */
} XhIrCoreInst;
/* A closed, parameterless single function. Code is caller-owned. Not SSA.
 * Executable v1 profile: 32-bit signed/unsigned integers, straight-line code.
 * Other value descriptors are valid vocabulary but rejected by this profile. */
typedef struct {
    uint32_t version;
    XhIrValueType result;
    const XhIrCoreInst *code;
    uint32_t code_count;
} XhIrCoreProgram;
typedef enum {
    XH_IR_CORE_OK = 0, XH_IR_CORE_BAD_VERSION, XH_IR_CORE_BAD_TYPE,
    XH_IR_CORE_UNSUPPORTED_TYPE, XH_IR_CORE_BAD_OPCODE,
    XH_IR_CORE_BAD_OPERAND, XH_IR_CORE_STACK_UNDERFLOW,
    XH_IR_CORE_STACK_OVERFLOW, XH_IR_CORE_TYPE_MISMATCH,
    XH_IR_CORE_BAD_RETURN, XH_IR_CORE_LIMIT, XH_IR_CORE_BAD_WIRE
} XhIrCoreStatus;
typedef struct { XhIrCoreStatus status; uint32_t instruction; } XhIrCoreError;

int xh_ir_value_type_valid(const XhIrValueType *type);
unsigned xh_ir_core_effects(const XhIrCoreInst *inst);
XhIrCoreStatus xh_ir_core_validate(const XhIrCoreProgram *program, XhIrCoreError *error);
/* Independent canonical little-endian XHIRCORE codec. Never ARKIR/XHIR.
 * Encode returns bytes or -1; decode writes caller-owned storage only after
 * structural size checks. On failure *program is cleared. No allocation. */
int xh_ir_core_encode(const XhIrCoreProgram *program, unsigned char *out, uint32_t capacity);
XhIrCoreStatus xh_ir_core_decode(const unsigned char *wire, uint32_t length,
    XhIrCoreProgram *program, XhIrCoreInst *storage, uint32_t capacity,
    XhIrCoreError *error);
#endif
