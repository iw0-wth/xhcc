# IR core v1: executable language-neutral foundation

Status: executable core prototype plus the first production consumer migration.
The i386 and AMD64 integer IR_BINARY dispatches now consume neutral operation IDs
through the legacy adapter. The C frontend, optimizers and default ARKIR wire
representation retain their existing contracts. This is a stack-machine
contract, not SSA and not a completed migration of the compiler to a
language-independent IR.

## Existing boundary

`include/ir.h` imports `common.h` (which currently defines TokenKind and IrOp)
and `type.h`. Binary operation IDs in legacy IrInst.aux are C TOK_* numbers.
Functions, parameters and globals reference the C Type graph; global initializer
text and several function attributes remain C-specific. `src/ir/irgen.c` lowers
C AST to this stack machine. `irser.c` persists that Type graph in ARKIR versions
1/2. The registry selects native engines; XhTargetDescriptor validates target
data-model/ABI facts, but cannot itself make this C graph language-neutral.

## Independent contract and implemented profile

`include/xh_ir_contract.h` and `src/ir/ir_contract.c` include no C frontend,
common.h, tokens, AST, Type, backend, global config, allocator or platform API.

Version 1 fixes numeric IDs for value categories, signedness, binary operations
and core instructions. Integer widths and pointer representation widths are
explicit bytes. Pointer address spaces are explicit (0 is generic). Aggregates
use an explicit byte extent, without C struct/union tags, fields or qualifiers.
The descriptor vocabulary permits integers through 16 bytes, IEEE binary float
widths 2/4/8/16, pointers through 16 bytes and nonempty aggregate extents. These
are representational facts; successful descriptor validation does not imply
that an execution profile or backend supports them.

XhIrAbiPlanRef reserves explicit table version, ABI identity and plan index.
There is no executable CALL opcode in this profile, no persisted ABI plan
reference in its instructions, and no implication that the current SysV plan
schema already covers other ABIs.

The executable v1 profile is deliberately closed: one parameterless function,
32-bit signed/unsigned integer values, at most 128 instructions and stack depth
64. CONST pushes its raw low 32 bits, BINARY pops rhs then lhs, and RETURN
consumes the sole remaining value and must be the final instruction. Operand
types must match exactly; no implicit promotions, truth conversions or C usual
arithmetic conversions exist here. Comparisons produce unsigned 32-bit 0 or 1.
All 16 binary operations have neutral IDs independent of token numbering:
add/sub/mul/div/rem, bitwise and/or/xor, shifts, eq/ne/lt/le/gt/ge.

Addition/subtraction/multiplication and bitwise operations use low 32-bit modular
results for both signednesses. Division/remainder use operand signedness and
truncate signed division toward zero. Division by zero, signed INT32_MIN/-1,
and shift counts outside [0,31] are trapping conditions; core v1 validation
checks shape/types, not dynamic operands. A later execution profile must define
how those traps are implemented before claiming support for arbitrary inputs.
Shifts use raw bits; signed right shift sign-extends, unsigned right shift
zero-extends. Signedness selects ordered comparisons. The current bridge
preserves the existing machine backend's behavior, and tests avoid dynamic trap
inputs. It does not claim to add portable trap enforcement to legacy engines.

Effects distinguish pure operations, possible traps and terminators. Memory and
call effect bits are reserved for future operations. Unknown instructions return
an invalid effect mask, and the validator rejects them. There is no control
flow/join validation or SSA claim. C short-circuit operators must be lowered to
control flow by the frontend when a future profile supports it; they are not
bitwise binary operators.

## Codec

The independent codec uses literal 8-byte magic `XHIRCORE`, unlike the existing
stage XHIR envelope or ARKIR payload. It is not inserted into existing object
files, and does not change default legacy format behavior.

All words are little endian with fixed widths and no C-struct padding:

| Offset | Header field |
| --- | --- |
| 0 | 8 magic bytes |
| 8 | u32 contract version (1) |
| 12 | u32 complete byte length |
| 16 | result descriptor: five u32 words |
| 36 | u32 instruction count |
| 40 | instruction records, 36 bytes each |

A descriptor is category, byte width, signedness, address space, aggregate byte
extent. A record is u32 opcode, u32 binary ID, descriptor, u64 raw constant bits.
Reserved binary/bits fields must be zero on instructions that do not use them.
Only validated executable-profile programs encode. Decode bounds the record
count before computing lengths, rejects unsupported versions/truncation/trailing
bytes, validates semantics and returns no program on failure. Storage is supplied
by the caller; no allocation or deserialization of native pointers occurs.

## Legacy bridge and real execution

`xh_ir_legacy_adapter.h` / `ir_contract_legacy.c` are the explicit compatibility
boundary and intentionally import legacy IR/C types. Their bidirectional mapping
covers all 16 neutral binary IDs and refuses unknown IDs/tokens. The bridge
validates core before allocating a legacy IrProgram, maps integer result types
to type_int/type_uint, preserves signed raw constants, and maps unsigned
operations to the legacy aux unsigned bit. Its caller supplies a trusted,
backend-safe symbol name. The main C frontend does not call this core-to-legacy
producer bridge; its direct caller remains the independent synthetic frontend
test. Separately, the production x86 integer binary consumers now call the
legacy-to-neutral operation accessor described below.

`tests/test_ir_contract.py` builds an isolated WASI harness from the actual core,
legacy IR/codec and AMD64 backend. Synthetic frontend code constructs neutral
instruction arrays directly without parsing any C source or AST. Each program
passes typed core validation, canonical encode/decode/re-encode equality, bridge,
real ARKIR serialize/deserialize, actual AMD64 assembly emission, LLVM assembly
and link, then Unicorn AMD64 CPU execution. Cases are 6*7 -> 42, unsigned
0xffffffff/2 -> 2147483647 and unsigned 0xffffffff>2 -> 1. The unsigned cases
would fail if the boundary silently interpreted them as signed. The harness also
rejects type mismatch, stack underflow, unsupported 8-byte executable values,
unknown operators, codec capacity failure, truncation and unknown version.

This closes a genuine non-C producer-to-existing-backend path. It does not remove
legacy Type graph coupling or make every existing IR instruction neutral.

## Legacy binary consumer boundary

`xh_ir_legacy_binary(const IrInst *)` is the production consumer accessor for
legacy IR_BINARY instructions. It checks the opcode, accepts only low-byte C
token IDs with the unsigned bit (0x100) and IR_AUX_SLOT64 (0x2000), then returns
the independent neutral operation. Null instructions, other opcodes, unknown
tokens and any unknown aux flag return INVALID. It does not reinterpret
IR_64BINARY, IR_64CMP, IR_FCMP, dedicated IR_DIV/IR_MOD or unary operations.
Signedness and machine slot width remain explicit, separate legacy facts for
the consuming backend; this accessor does not mutate the instruction or wire.

The existing C producer sets unsigned for ordered comparisons/right shift and
sets SLOT64 for AMD64 pointer arithmetic/comparisons. C logical AND/OR lower to
branches and normalized NE values before IR_BINARY emission, and compound
assignments map to their base operators. Dedicated DIV/MOD remain their own
opcodes, while the neutral bridge can emit their binary equivalents. Therefore
16 mapped binary operations cover this boundary without treating short-circuit
logical operations as bitwise operations. Other opcode families need their own
future accessors rather than bypassing opcode checks.

The isolated test covers all 16 operations with all four legal flag combinations,
rejects wrong opcodes and every undocumented high aux bit, and confirms the
accessor returns the same operation after existing ARKIR serialization. Backend
migration consumes neutral IDs while old ARKIR bytes remain unchanged.

Both integer IR_BINARY dispatch sites in the shared x86 backend (i386 and
AMD64) now use this accessor. The scalar rule selector switches on neutral enum
IDs. Invalid operations must fail directly through the backend diagnostic path,
without looking up a package-defined `scalar.invalid` template. Wide integer,
floating-point and unary token dispatches, other targets, the C producer and
optimizer raw-token consumers are outside this first migration. The unified
four-application Python build and full integration suite passed after this
migration, including the non-C synthetic CPU test and malformed integer-aux
rejections. This is evidence for these consumers, not for whole-IR neutrality.

## Reviewable migration stages

1. Completed foundation: additive API, codec, validator, adapter and synthetic
   CPU evidence. The core-to-legacy producer bridge remains outside the main C
   frontend path.
2. First consumer migration implemented: i386/AMD64 integer IR_BINARY dispatch
   uses the strict neutral accessor. Continue with precise, coordinated changes
   to C irgen boundaries, optimizer consumers, other targets and other operation
   families. Keep ARKIR numeric fields stable through explicit serialization
   adapters until a new format version is deliberately introduced. Never
   reinterpret old raw aux values as new enum IDs.
3. Extend executable profiles with explicit widths, memory/volatile effects,
   labels and typed stack joins, calls/signature/ABI plan references. Define
   target capability checks and trap policies; add non-C tests per capability.
4. Replace legacy Type-based function/global signatures with neutral descriptors
   and move C initializer text/attributes into frontend adapters. Only then
   evaluate a versioned neutral whole-module transport and remove the bridge.

Do not advertise whole-IR language neutrality until the remaining producers,
passes, consumers and transport no longer require C Type/token semantics.
