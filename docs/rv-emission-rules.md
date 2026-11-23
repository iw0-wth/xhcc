# RV32 external operation and emission rules

The RV32 package now selects `rv.rules` through the shared XHTEMPLATE 1 VM.
The adapter `xh_rv_rule`/`xh_rv_rule_args` propagates missing/malformed rule
failures to the translation unit, preventing object emission. Rule parameters
use bounded `${N}` expansion, including frame-offset materialization; external
text is never used as a C printf format.

Semantic operation rules include complete scalar32 unary negation, bitwise not,
logical not and identity sequences; signed/unsigned 8/16-bit truncation and
extension sequences; signed/unsigned memory-width instruction choices; and
positive/negative frame-address formation. For example `cast.trunc.s8` includes
load, left shift, arithmetic right shift and store of the stack result. Changing
the selected sequence changes the real machine instructions, not merely a
displayed mnemonic.

The initial table also extracts the remaining constant emission fragments into
numbered `emit.fixed.*` and `emit.choice.*` records. They cover existing 64-bit,
soft-float, complex, stack and calling-sequence text. Those records do not by
themselves move native control decisions or ABI implementation into data.
Native C still interprets IR, selects width/type branches, validates unsupported
operations, allocates frames/register argument locations, chooses runtime
helpers and preserves the stack contract. Binary scalar32 operation selection
continues through the separate XHSELECT v2 sequence table, and instruction
encoding/relocation formulas continue through their own external VMs.

## Finite ABI and frame policies

The same package table now includes required numeric contracts: word size,
argument-register count, packed argument words, frame stack alignment, variadic
register-image offset, saved ra/s0 offsets, local header size, indirect aggregate
threshold, and scalar/wide/indirect word classifications. The C adapter parses
bounded decimal values and validates the supported ILP32 contract before any
code generation. Caller register allocation and argument word classification,
callee variadic named-word counts, frame sizing/alignment and return/sret size
classification consume the parsed values.

Current supported values remain word=4, argument registers=8, packed words=1,
frame alignment=16, argument image offset=32, saved ra=36, saved s0=40, local
header=48, and indirect threshold=8. Their classifier outputs are 1/2/1 words.
Metadata cannot silently change this native calling convention.
`frame.extra_padding` is a safe internal layout policy, 0..256 in multiples of
16: it inserts a gap between saved state and local storage while all local
address calculations use the resulting frame size. It does not move the
variadic image or caller-visible stack parameters. Changing this policy can
change generated frame instructions without changing function ABI.

The compiler's existing call lowering is retained. In particular, validating a
16-byte frame alignment policy is not a claim that every intermediate call-site
stack shape is fully interoperable with arbitrary external psABI code. Native
argument normalization, aggregate lowering and frontend IR still bound the
supported convention. The runtime self-call/long-argument/variadic/sret tests
cover that implemented convention; external-tool ABI interoperability needs
separate target-runtime gates.

## Scalar soft-float and conversion selection

The shared semantic table additionally supplies 38 complete scalar soft-float
sequences: float/double ADD/SUB/MUL/DIV, six comparisons at both widths,
signed/unsigned int32/int64 conversions in both directions, and float/double
narrow/widen. Native code normalizes the IR operation and type flags; helper
names, argument pop/shuffle, result pushes and comparison-to-boolean sequences
come from selected package templates. Complex selection now uses 16 complete package sequences: ADD/SUB/MUL/DIV,
EQ/NE, ABS/EXP, conjugation, real/imaginary/argument extraction, and signed/
unsigned int32 or float/double construction. Native helpers that previously
composed these operation sequences were removed; the adapter normalizes the IR
kind and effect, while the package sequence includes scratch stack layout,
soft-helper/libm calls and result assembly. Complex memory/argument lowering
outside these operation records still uses native IR/ABI control. A subsequent
64-bit integer selector externalizes complete existing pure-RV sequences for
ADD/SUB/MUL/AND/OR/XOR/SHL/SHR/DIV/MOD, six comparisons and three unary
operations (35 signed/unsigned records). Division/modulo retain the existing
64-round shift/subtract implementation rather than inventing a runtime helper.
The native adapter only normalizes IR operation/sign and checks stack effects;
the selected full sequence determines instruction behavior. Tests use direct
64-bit register arguments/results and integer oracles, including carry/borrow,
negative values, wide products, shifts across the 32-bit boundary and division.

Soft-float ABI tests provide integer helper fixtures that return observable
floating-point bit patterns, so they validate helper choice and register/stack
marshalling. They are not an IEEE arithmetic runtime and do not establish
numerical correctness for arbitrary floats, NaNs, infinities or rounding modes.
A copied package redirects only the FADD semantic record to the FSUB sequence;
unchanged compiler/helper code then returns the different fixture bit pattern.
Real libgcc/libm interoperability requires a separate RISC-V runtime gate.

## Control, argument, call and return protocol selection

A further 21 normalized records select local labels, unconditional/zero/nonzero
branches, register/stack argument sources, indirect payload pointer sources,
caller argument register loads, direct/indirect invocation, scalar/wide/complex
call results, corresponding return values, and fixed/dynamic frame epilogues.
The selected templates own the complete protocol action, including stack
movement and register loads/stores. Native code retains label construction,
argument iteration, validated frame offsets and aggregate copy loops; it does
not infer a new ABI from arbitrary template text.

Runtime tests exercise branches/loops, existing long-argument/variadic/function
pointer/sret calls, and return paths. A same-SHA package variant redirects only
the zero-branch record to the nonzero-branch sequence and reverses observable
branch results. Incompatible argument or return stack-effect claims fail.

This is not an arbitrary new-ISA or new-ABI loader. Incorrect rule modifications
can deliberately generate invalid machine code; packages must be validated for
their native engine family. The shared template VM enforces syntax, bounds,
parameter references and missing-key errors, not semantic equivalence to the
original instruction sequence.

`tests/test_rv_templates.py` executes scalar unary/cast edge cases, byte/halfword
signedness, local/indexed stores, and frame offsets beyond 2 KiB. A copied package
changes unary NEG to NOT while keeping compiler SHA256 unchanged, and real
linked execution changes accordingly. Removing a required operation key fails
compilation. Other regressions remain the authority for aggregate, complex,
variadic and ABI behavior.
