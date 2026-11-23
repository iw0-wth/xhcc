# ABI plan persistence in IR version 2

This round adds opt-in recording and persistence of an experimental SysV AMD64
classification/placement plan. It does **not** switch native caller/callee
lowering, remove the existing private aggregate wire convention, or establish
GCC/Clang ABI interoperability. Default compilation does not request plans.

## Opt-in frontend behavior

`-frecord-sysv-abi` requests plan recording for `x86_64`. Other targets reject
this option. At each supported call the frontend retains the signature and
post-conversion actual argument types long enough to build a plan. Named
parameters use the declared parameter types; variadic float arguments use their
promoted double type. Functions receive parameter/result references to their
whole-signature plan. A failed or unsupported inspection is diagnosed, not
silently replaced with a guessed plan.

The current adapter requires a complete supported function prototype. Missing
or unknown parameter types, bitfields, complex leaves and unsupported type-graph
shapes are rejected. Arrays, structures and unions are traversed within explicit
graph/depth limits; a sufficiently large expanded graph can exceed those limits.
At most 64 arguments and two fragments per argument/result are represented.
These limits describe the recorder, not a claim that every recorded signature
already has native code generation using that plan.

Rule storage is selected through the package's aggregate-rule registration. The
builder uses target alias `x86_64.aggregate` and prefix `abiplan`. Its
`rule_fingerprint` is the shared template loader's FNV32 fingerprint of the
actual rule-file bytes, including comments. It detects configuration changes;
it is not cryptographic authentication. Capturing a fingerprint does not yet
mean native code generation checks and consumes it.

## IR container version and references

The legacy magic remains eight bytes `ARKIR1\n` including its C-string trailing
NUL. The following header is explicit little-endian:

```text
magic[8] | u32 version | u32 body_length
```

The writer now emits version **2**. The reader accepts versions 1 and 2 and
rejects every other version. A v1 body has no plan table or plan-reference
fields; reading v1 initializes all plan references to zero. It cannot recover
plans that were never recorded. Existing binaries that read only version 1
must rebuild before consuming v2 objects; no backward-reader compatibility is
claimed.

Version 2 adds the following prefix before the existing type table:

```text
u32 plan_count
repeat plan_count times:
    u32 plan_byte_length
    byte canonical_plan[plan_byte_length]
```

`plan_count` is bounded by `IR_ABI_MAX_PLANS` (currently 1024). Plans are allocated
only when present and are owned by the program arena. Each instruction record
adds an `i32 abi_plan` after `aux2`; it must be zero except on `IR_CALL`. Each
function record adds `i32 parameter_plan` and `i32 result_plan` after its return
type reference. These are **1-based indices** into the program plan table;
zero means absent. Parameter/result references can point at the same
whole-signature plan, whose fields provide their respective views.

`IR_CALL.aux2` remains the argument-wire-size table index. The new plan index is
independent of it, so old size/alignment/pointer/FP-class metadata is not
reinterpreted as a plan. Unknown opcodes, invalid name fields, bad plan indices,
malformed plan records and truncated bodies fail deserialization.

## Canonical plan codec, version 1

All fields below are `u32` little-endian, written individually. No raw C struct,
pointer, alignment padding or host integer layout is serialized.

| Record | Fields in wire order |
| --- | --- |
| Header | magic `0x50414858` (bytes `XHAP`), version=1, abi_id=1, rule_fingerprint, arg_count, stack_bytes, sret |
| Result | memory, count, followed by exactly two `{offset, size, class_id}` fragments |
| Each argument | version=1, by_value_stack, size, alignment, count, stack_offset, stack_extent, followed by exactly two location fragments |
| Location fragment | object_offset, size, bank, index, stack_offset |

The plan's exact byte length is `4 * (15 + 17 * arg_count)`: 60 bytes for zero
arguments and at most 4412 bytes for 64. Unused fragment slots must be all zero.
The current ABI id identifies this inspection subset, not an unrestricted
universal ABI vocabulary. Result metadata currently records classification;
it is not a complete native result-register execution recipe.

Validation includes supported versions/ABI id, argument/fragment counts,
stack alignment and extent, power-of-two bounded argument alignment,
nonoverlapping in-object fragments, register-bank/index bounds and consistent
memory/sret fields. The codec validates before encoding or committing decoded
output. It rejects extra trailing bytes as well as truncation, and does not
serialize unused trailing C-array storage.

The public codec and builder contracts are declared in `include/xh_sysv_plan.h`;
`IrAbiPlan` is its typed alias in `include/ir.h`.

## Merge ownership and index relocation

`ir_program_add_abi_plan` validates and deep-copies a plan into the destination
program arena, returning a 1-based index. `ir_program_append_unit` first appends
the source unit's plans, then adds the previous destination plan count to every
nonzero call, parameter and result plan reference. Zero references remain zero.
String, floating constant and `aux2` argument-size rebasing retain their existing
independent handling. Synthetic control-flow instructions initialize the new
reference to zero.

This is metadata preservation and reference relocation. It does not re-plan a
call under a different rule fingerprint or authorize merging incompatible ABI
policies as equivalent native code. A future native consumer must validate the
ABI identity/rule policy agreement before using placements.

## Actual verification in this round

`tests/test_abi_plan_ir.py` passed **2/2** tests:

1. An isolated harness using the actual IR implementation, serializer and plan
   codec creates a plan, attaches call and function references, serializes and
   reads it, merges into a program with an existing plan, and verifies references
   rebase from 1 to 2. It also reads an actual v1 empty-body fixture with no plans
   and rejects an unknown container version.
2. The rebuilt compiler's modern `-frecord-sysv-abi -c --private-object --obj-ir`
   path compiles a mixed double/long aggregate signature. The resulting private
   object contains version 2 IR, at least two nonempty plans, codec version/ABI 1,
   the expected argument count and a nonzero rule fingerprint. This is real
   frontend-produced metadata, not only a handwritten codec fixture.

The second test was executed with the rebuilt `xhccm` compiler entry, without
invoking make. A separate builder/codec suite owned by the x86 agent checks
rule-file comment changes alter the fingerprint, wire truncation/corruption is
rejected atomically, and unsupported bitfields preserve caller output. Those
checks should retain their own run evidence rather than being inferred from
this two-test result.

## Next phase and package guidance

Native caller and callee integration is the next phase. It must consume the
same plan for argument banks, stack objects, hidden result handling, return
classification and variadic metadata; preserve evaluation order and stack
balance; and pass mixed-toolchain ABI execution tests. Recording successfully
is a prerequisite, not proof of that integration.

Package authors should describe this mode as inspection/recording only. A
package update, capability name or skill document must not present it as an
ABI switch before a production native consumer exists and is validated. No new
package capability is implied merely by accepting the compiler option; manifest
capability changes need to match the actual compiled consumer and schema.
