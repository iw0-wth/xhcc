# ARM Thumb-1 initial scalar subset

The ARM backend now has an initial, restricted ARMv6-M scalar path. This is not
complete Thumb-1 support for all C programs, all ARM CPUs, or the whole ARM
backend. Existing Thumb-2 targets continue to use their established path.

## Selection and implementation

The selected CPU description enables this path when `arch_m == 6`,
`thumb_only != 0`, and `thumb2 == 0`. Selection uses capability fields rather
than CPU name comparisons. This currently admits the package descriptions for
M0, M0+ and M1. It does not advertise ARMv8-M baseline/M23 support.

The existing C frontend, IR and ARM code generator are reused. A separate,
bounded text legalizer in [thumb1.c](../src/backend/arm/thumb1.c) processes the
emitted assembly before output or internal assembly. It recognizes specific
scalar instruction/operand shapes and rejects unrecognized shapes instead of
passing them through as potentially illegal Thumb-2 instructions. Input lines
are limited to 511 bytes. The final encoding path also checks a restricted
Thumb-1 encoding set.

The target sequences live in [arm.rules](../packages/arm/arm.rules), including
`t1.header`, `t1.constant` and `t1.compare`. The header explicitly selects unified
syntax and `.cpu cortex-m0`. Constants use low-register byte construction;
comparisons replace IT/ITE materialization with local conditional branches.
SP adjustments are split into aligned pieces of at most 508 bytes. Both
assembler passes use the Thumb-1 branch widths, and the external relocation
rules enforce short-branch range and alignment. The existing optimization and
layout algorithms remain in C; they have not all become external policy.

The legalizer is intentionally limited rather than a general assembler or a
claim that arbitrary Thumb-2 text can be translated. Its final instruction
checks do not replace independent ISA verification. CPU-dependent ELF build
attributes select an external Thumb-1 object profile; the object must not
claim the old ARMv7-M/Thumb-2 profile for this path.

MicroPython's Thumb assembler was read only to understand architectural
strategies such as capability gates, constant construction and branch sizing.
Its implementation includes both Thumb-1 and Thumb-2 paths; it was not treated
as a wholly Thumb-1 implementation. No MicroPython source implementation was
copied into this path.

## Supported first-step behavior

The verified subset covers low-register 32-bit integer arithmetic and bitwise
operations, comparisons, local scalar loads/stores, small local control flow,
direct BL calls, ordinary function frames and 32-bit constants. Memory offsets
are limited to the supported narrow forms: low-register accesses use the
31-scaled-offset range, and word SP-relative accesses use aligned offsets up to
1020 bytes. Frame adjustment requests above the current 4096-byte bound are
rejected, and other addressing shapes can reach their own narrower limits
first.

Conditional branches must fit their signed 8-bit halfword displacement;
unconditional branches must fit their signed 11-bit halfword displacement.
Out-of-range branches fail. There is no long-branch trampoline implementation
in this first step.

Division/remainder helper lowering, global objects and global addresses,
strings, floating/complex operations, 64-bit operations, unsupported high
register forms and unsupported memory shapes are outside this subset. They
must produce a diagnostic rather than silently emit Thumb-2 instructions.
This includes cases where the selected CPU is recognized but the program uses
an operation not implemented by this initial path.

## Validation

[The Thumb-1 codegen tests](../tests/test_thumb1_codegen.py) exercise both an
independently constructed IR program and a production C input. The production
C case compiles local variables, a while loop, branches, a direct helper call
and a large constant through the actual `-mcpu=cortex-m0 -O0 -S` entry point.
The generated output is passed unchanged to GNU assembler with
`-mcpu=cortex-m0 -mthumb`, then GNU ld, then Unicorn. Its result is
`0x12345678 + 45 == 0x123456a5`. The unsupported division case is rejected.

GNU's Cortex-M0 assembler is the independent instruction-legality check.
Unicorn's Thumb/MCLASS execution alone is insufficient because its instruction
acceptance is broader than the intended CPU subset. Execution results do not
claim complete MCU peripheral, exception or timing behavior.

[Object-rule tests](../tests/test_object_rules.py) separately check direct
object generation and Thumb-1 CPU/ISA attributes. Passing the `-S` test is not
used as a substitute for direct-object validation. Existing ARM regression
suites remain necessary for the unaffected Thumb-2 path.

## Remaining work

- Generalize instruction legalizing decisions into bounded external policies
  while retaining strict operand validation.
- Add long conditional/unconditional branch sequences and validate their
  layout, scratch registers and relocations.
- Lower divide and remainder to ABI-correct software helpers.
- Implement global/string address materialization, literal pools and matching
  object/link relocations.
- Extend frame, high-register and memory-offset handling with explicit limits.
- Audit and implement wider scalar, floating/complex and ABI cases rather than
  inferring support from the shared frontend.
- Validate any later ARMv8-M baseline profile independently before enabling it.
