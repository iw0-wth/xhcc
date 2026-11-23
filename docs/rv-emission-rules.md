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

## Indexed wide and complex memory selection

Seventeen additional records select complete aligned/packed 64-bit integer
loads and stores, aligned complex-float/double indexed loads and stores, and
complete scalar indexed store actions. The adapter normalizes width/type and
expected stack effect rather than choosing the target sequence in C. Address
formation and element scaling remain shared mechanisms. Existing high/low word
and complex real/imaginary order is preserved. Packed integer64 and complex accesses use byte sequences. Complex-double
reads assemble each 32-bit word from bytes and writes split each word to bytes,
retaining real/imaginary and stack word order. Complex-float loads preserve the
unaligned address across both float-to-double helper calls in bounded scratch;
stores preserve the original internal double value, narrow both parts with
bounded scratch, then write the converted float bytes. The store expression
result is retained from the original IR value, never reconstructed by a lossy
float round trip. No unaligned lw/sw touches the packed object.
Tests cover indexed integer64, misaligned packed integer64, complex-double
word order, packed complex offsets 0/1/2/3, retained expression low words, and
incompatible selected stack effects. Conversion fixtures validate known-bit ABI
marshalling rather than general IEEE conversion correctness.

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

## Bitfield, copy and dynamic allocation selection

Five further records own signed/unsigned word bitfield extraction, bitfield
read-modify-write, dynamic alloca and word-plus-byte-tail memcpy sequences.
The adapter validates bit offset 0..31, width 1..32 and offset+width<=32 before
computing masks, including a separate all-ones width32 case to avoid undefined
C shifts. Negative or unrepresentable memcpy counts fail rather than silently
becoming a zero copy. Record stack effects are checked for load 1->1, store
2->0, alloca 1->1 and copy 2->0. Native code supplies normalized offsets/masks
and byte counts; the package selects and emits the full target action.
Tests exercise signed and unsigned fields, exact preserved memory bits, width32,
VLA allocation/epilogue and an 11-byte aggregate copy with a byte tail. Redirecting
the signed field record to the unsigned sequence changes -1 to 127 while compiler
SHA remains unchanged.

## External register aliases and caller register names

`isa.register_count` declares the supported 32-register namespace;
`isa.register.aliases` contains the full name/number table, bounded to 64 unique
lexical aliases. `abi.arg_register.0` through `.7` select the names used by caller
and callee parameter emission. The assembler actually consults the external
alias table, including aliases added by a copied package. Canonical `xN` parsing
remains a primitive with its bound read from the declared register count.

The adapter verifies existing required ABI names (zero/ra/sp/fp/a0..a7 and the
scratch/saved registers used by templates) retain their native numbers. Argument
names may be alternate aliases only when they map to x10..x17. Register count
changes, duplicate aliases, invalid tokens or wrong ABI mappings fail before
emission. The constraints preserve the existing native engine contract; they
are not a mechanism for inventing a new register file.
A same-SHA test adds `argfirst 10`, uses it both as argument-register spelling
and in inline assembly, and executes the compiled result as 42.

## External R-format mnemonic descriptors

`isa.mnemonics.r` maps mnemonic tokens to three-argument encoding rule keys
and the finite `rr_r` operand shape (rd, rs1, rs2). The 18 standard R-format
instructions now have dedicated package encoding rules whose base contains
opcode/funct3/funct7. The assembler's former R mnemonic/field table is removed;
its shared three-register parsing mechanism passes normalized arguments to the
selected external encoder. Generic R encoding remains for existing pseudo
instruction lowering.

The descriptor is bounded to 64 rows/16 KiB and allocated for actual rows.
Duplicate names, fixed pseudo/other-format name collisions, unknown operand
shapes, absent encoding keys and incompatible encoder argument counts fail
loading. The table may add a new spelling for an existing supported encoding;
it does not invent an ABI or change the native parser's operand grammar.
Tests check all 18 emitted words against independent ISA field calculations and
add `plus R_ADD rr_r` to a copied package; unchanged compiler execution returns
42 using the new mnemonic. Six immediate and three shift-immediate instructions additionally use external
`rr_i` / `rr_shamt` descriptors and dedicated three-argument encodings. Their
fixed opcode/function fields and signed12 or unsigned5 immediate constraints
are in `encoding.rules`; C only parses rd/rs1/immediate and checks that the
normalized input fits the encoder's integer API. Tests compare all nine words
against independent ISA fields, execute a new `newplusimm` spelling as 42 with
unchanged compiler SHA, and reject immediate boundary violations. Pseudo
lowering still uses the generic encoding helpers. Five load, three store and six branch mnemonics additionally use external
`rd_mem`, `rs_mem` and `rs_rs_label` descriptors with dedicated three-argument
encoders. Their signed12 memory offsets and signed13/even branch displacements
are checked by external encoding rules. Symbol lookup, local numeric label
resolution and unresolved-cross-object branch diagnostics remain native generic
mechanisms; the selected encoder supplies fixed opcode/function fields at the
resolved site. Tests inspect all 14 words against independent field formulas,
exercise new load/store/branch aliases together to return 42 with unchanged
compiler SHA, and reject offsets and unresolved labels. LUI/AUIPC/JAL/JALR additionally use `rd_imm20`, `rd_label` and `rd_jumpmem`
descriptors with external fixed fields and scatter rules. JALR accepts both
offset(base) and register-plus-immediate forms. JAL retains local symbol
resolution and range diagnostics, then uses the selected encoder. Upper
instructions preserve the historical 32-bit input pattern/mask-to-high20
semantics. Tests inspect four actual words, execute new upper/jump/return
aliases together as 42, and reject missing labels/invalid immediates.
Fixed pseudo lowering (call/la/j/jr/ret and similar forms) and system instruction
constants remain native pending later migrations.

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
