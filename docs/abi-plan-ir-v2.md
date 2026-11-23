# ABI plan persistence in IR version 2

The IR carries persistent SysV AMD64 classification/placement plans. Recording
and native consumption are separate opt-ins: `-frecord-sysv-abi` records metadata;
`-fuse-sysv-abi-plan` records and consumes verified scalar or certified **1–8-byte integer-structure** plans for
caller/callee placement. Default compilation retains its original lowering.
Other aggregate plans remain record-only and are explicitly rejected by use mode.
Scalar interoperability is tested below; complete aggregate SysV interoperability
is not claimed.

## Opt-in frontend behavior

`-frecord-sysv-abi` requests plan recording for `x86_64`. Other targets reject
this option. At each supported call the frontend retains the signature and
post-conversion actual argument types long enough to build a plan. Named
parameters use the declared parameter types; variadic float arguments use their
promoted double type. Functions receive parameter/result references to their
whole-signature plan. A failed or unsupported inspection is diagnosed, not
silently replaced with a guessed plan. Use mode additionally requires a version
2 plan with a supported verification flag and matching ABI identity/rule fingerprint. Integers,
float/double and pointers, including stack spills and variadic calls, are within
the scalar consumer. The small-aggregate phase also supports nonempty structures
of 1 through 8 bytes, containing only integer/pointer fields or recursively
eligible nested structures with proper natural alignment and in-bounds fields.
Packed/unaligned/bitfield, floating-field, union, array-field and larger
aggregates remain rejected. Aggregate values through
variadic `...` are rejected even when the same structure is valid as a named
parameter. Structure arguments may use one GPR or spill to the stack; certified
1–8-byte structure results are returned through the supported integer result
location. Function/array parameter types
undergo the required C pointer adjustment before planning. `_Generic`-selected
function designators recover prototypes from real declared symbols; argument
values are not used to invent missing prototypes.

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
it is not cryptographic authentication. Native use validates that fingerprint
before consuming the recorded placements, including plans read back through LTO.

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

## Exact object access and capability boundary

For native plan use, partial aggregate loads carry
`IR_AUX_AGGREGATE_EXACT=0x40000` on `IR_LOAD_IDX`. Its value is the actual
1–8-byte object width; the output retains the legacy `ceil(bytes/4)` IR word
shape. The AMD64 consumer reads only those bytes. Callee parameter copies and
returned-value stores likewise preserve the object's exact extent, including
3/5/6/7-byte objects. This is essential for adjacent objects and page boundaries;
rounding an object access to 4 or 8 bytes is not authorized by a rounded call slot.

Serialization validates the width and flag combination. Other engines or
non-use modes must reject this extension rather than reinterpret it as ordinary
loads. The feature capability is `abi.plan-small-int-struct.v2`, separate from
canonical codec version 2 and the outer IR version. Flags=2 remains the same
certification value, but consumers limited to the older 4/8-byte subset must not
claim this newer capability.

## Canonical plan codec, versions 1 and 2

All fields below are `u32` little-endian, written individually. No raw C struct,
pointer, alignment padding or host integer layout is serialized.

| Record | Fields in wire order |
| --- | --- |
| Header | magic `0x50414858` (bytes `XHAP`), version (1 or 2), abi_id=1, rule_fingerprint, arg_count, stack_bytes, sret; v2 then adds flags |
| Result | memory, count, followed by exactly two `{offset, size, class_id}` fragments |
| Each argument | version=1, by_value_stack, size, alignment, count, stack_offset, stack_extent, followed by exactly two location fragments |
| Location fragment | object_offset, size, bank, index, stack_offset |

Canonical v1 has exact length `4 * (15 + 17 * arg_count)`: 60 bytes for zero
arguments, at most 4412 bytes for 64. Canonical v2 adds the flags word after
`sret`, giving `4 * (16 + 17 * arg_count)`: 64 to 4416 bytes. Unused fragment
slots must be all zero. The ABI id remains 1; outer IR container version remains
2 independently of the inner plan version.

Version 2 verification values are:

| flags | Source-type proof and consumer permission |
| --- | --- |
| 0 | No native-use certification; record/inspect only. |
| 1 (`SCALAR_VERIFIED`) | Result, named parameters and actuals are supported scalars, after C parameter decay; void results are allowed. |
| 2 (`SMALL_INTEGER_AGGREGATE_VERIFIED`) | The complete signature/call mixes supported scalars with at least one certified 1–8-byte integer/pointer structure. |

Values 1 and 2 are alternatives, not independent bits to OR: **3 is rejected**,
as are other unknown values. Combining source-type proofs produces 2 when any
eligible small structure is present, 1 for scalar-only, and 0 if any component
is unsupported. Every nested structure must independently satisfy the exact
1–8-byte, field-bounds and natural-alignment checks; a placement resembling a
single integer fragment is insufficient proof. Variadic aggregate actuals clear
native eligibility. Version 1 decodes with flags=0 and remains record-compatible,
but native use rejects it. The outer IR container stays version 2; adding this
second certified subset does not change its field layout.

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
policies as equivalent native code. The native plan consumer validates the ABI identity and rule fingerprint
before using placements; mismatches are rejected rather than silently re-planned.

## Actual verification in this round

`tests/test_abi_plan_ir.py` passed **2/2** tests:

1. An isolated harness using the actual IR implementation, serializer and plan
   codec creates a plan, attaches call and function references, serializes and
   reads it, merges into a program with an existing plan, and verifies references
   rebase from 1 to 2. It also reads an actual v1 empty-body fixture with no plans
   and rejects an unknown container version.
2. The rebuilt compiler's modern `-frecord-sysv-abi -c --private-object --obj-ir`
   path compiles a mixed double/long aggregate signature. The resulting private
   object contains version 2 IR, at least two nonempty plans, canonical codec version 2 / ABI 1,
   the expected argument count and a nonzero rule fingerprint. This is real
   frontend-produced metadata, not only a handwritten codec fixture.

The second test was executed with the rebuilt `xhccm` compiler entry, without
invoking make. A separate builder/codec suite owned by the x86 agent checks
rule-file comment changes alter the fingerprint, wire truncation/corruption is
rejected atomically, and unsupported bitfields preserve caller output. Those
checks should retain their own run evidence rather than being inferred from
this two-test result.

## Native execution evidence

`tests/test_abi_plan_codegen.py` passed **7/7** against root artifact 906421,
with `CLANG_REFERENCE=D:\msys64\ucrt64\bin\clang.exe` and no skip:

1. Two separately compiled xhcc translation units execute integer registers and
   stack spills, ten double arguments, float results, pointers and mixed scalar
   variadic access, returning 1.
2. Unsupported floating, packed, bitfield, union, array-field and 16-byte
   structures are explicitly rejected.
3. Private-object plans with only their rule fingerprints changed fail LTO use
   with a stale-plan diagnostic and no output.
4. Both scalar xhcc caller -- Clang callee and Clang caller -- xhcc callee execute.
5. Struct4 and struct8 arguments/results, a structure passed after six occupied
   GPR slots, and a pointer-field structure execute in three routes: xhcc/xhcc,
   Clang callee and Clang caller. Each combined fixture returns 1.
6. Struct1/2/3/5/6/7 arguments/results and after-six-GPR spills execute through
   the same three compilation routes. Destination canaries before and after
   each object remain unchanged; each combined check returns 0.
7. Aggregate variadic actuals remain explicitly rejected.

The canary gate initially exposed a missing 16-bit mov operand-size prefix in
three assembler paths: a textual word store encoded as a dword store. After the
prefix correction, the unchanged seven-test suite passed. This records a real
boundary failure and fix rather than a weakened byte-check fixture.

This establishes the tested certified small-integer-structure subset, not full
SysV aggregate interoperability. Recursive certification is a source contract;
this execution fixture directly covers flat sizes 1 through 8 and pointer fields.
The preceding IR persistence and builder/codec suites remain separate evidence.

## Next phase and package guidance

Remaining aggregate integration includes larger/multi-fragment
structures, floating/mixed classes, MEMORY/sret, unions/arrays, packed/bitfield
layouts and aggregate variadic access. Those need their own source certification,
caller/callee lowering and mixed-toolchain execution tests. Recording their
classifications does not establish native support.

Package/skill documentation must distinguish record mode, certified scalar/
1–8-byte integer-structure use, and unsupported aggregate use. Neither flag
is enabled by default. Capability names must describe an integrated consumer
and its schema; accepting an option is not a universal ABI guarantee.
