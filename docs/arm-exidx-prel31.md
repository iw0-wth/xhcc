# ARM PREL31 and retained CANTUNWIND exception indexes

This is a bounded ARM ELF32 little-endian REL extension, not full ARM EHABI
unwinding support. It does not change the compiler's exception model.

## Relocation policy remains external

The ARM byte adapter recognizes `R_ARM_PREL31` (42), decodes its REL addend as
`sign_extend(input_word & 0x7fffffff, 31)`, and supplies the relocation context
to `ELF_PREL31` in [the ARM relocation rules](../packages/arm/relocation.rules).
The shared bounded relocation VM computes:

```
X = ((S + A) | T) - P
output = (input_word & 0x80000000) | (X & 0x7fffffff)
```

`S` is the symbol address without the Thumb state bit when the symbol is a
Thumb FUNC, `T` is that explicitly determined state bit, and `P` is the
relocated word's address (no instruction PC bias). Odd data addresses are not
silently treated as Thumb functions. Thumb state is ORed **after** addition.
`X` must fit signed 31 bits: `[-2^30, 2^30-1]`. The nonrelocated bit31 is
preserved. The adapter does not implement the arithmetic policy in C.

Decode errors preserve the output-parameter sentinels. Apply errors preserve
all destination bytes; the relocation VM stages writes in its bounded scratch
buffer and publishes them only after every assertion succeeds.

## Checked exception-table subset

The reader accepts ARM `SHT_ARM_EXIDX` only with exactly
`SHF_ALLOC | SHF_LINK_ORDER` (`0x82`), alignment at least four, size divisible
by eight, zero `sh_info`, and `sh_entsize` zero (as emitted by the audited GNU
runtime) or eight. `sh_link` must reference a valid, different section in the
same object: allocatable executable, nonwritable `SHT_PROGBITS`. Other uses of
`SHF_LINK_ORDER`, including on RV32 input, remain rejected.

The linker further requires:

- Every eight-byte index record has second word exactly `EXIDX_CANTUNWIND`
  (`1`). Compact unwind descriptors, personality routines, and nonempty
  `.ARM.extab*` are unsupported and diagnosed, not discarded.
- Each record has exactly one PREL31 relocation at its first word. Other
  relocation types or offsets into an index record are rejected.
- Every relocated first word denotes an address inside its associated code
  section. Thumb state is retained in the encoded relocation, but cleared for
  code-range and function-order validation.
- The nonempty index sections form one contiguous address interval, with
  strictly increasing function addresses in final table order. Unsupported
  script reordering is rejected; the linker does **not** sort or rewrite
  already-relocated records, synthesize entries, coalesce CANTUNWIND records,
  or implement section garbage collection.

Output retains each input index section's type, flags, name, entry-size field,
and `sh_link` remapped to its associated **output** code-section index. As in
the existing linker's architecture, input sections retain separate section
headers even when a script places them in one output group. An explicit
exception-index group has a readable `PT_LOAD` mapping. No `PT_ARM_EXIDX` is
emitted. This subset therefore does not claim compatibility with loaders that
require that program header or provide a complete unwind runtime.

All validation precedes output publication. Malformed inputs, unsupported
records, invalid association, and nonmonotonic order leave an existing output
file untouched. This is prepublication validation atomicity, not a new promise
of transactional filesystem behavior on an actual write failure.

## Explicit application profile, not implicit orphan placement

Unmatched nonempty allocatable sections still fail. No general GNU orphan
algorithm, implicit placement based on MicroPython symbol names, metadata
stripping, or discard-by-default policy was added to the core.

The separate SD engineering profile named `stm32f405-xhcc.ld` must be selected
explicitly in place of `stm32f405.ld` for this configuration. The original
script is retained as the read-only comparison baseline; the SD run artifacts
include a unified diff and both files' SHA256 values. The profile declares
separate `.ARM.extab` and `.ARM.exidx` groups after code and before the Flash
load image of initialized RAM data. It retains `__exidx_start` and
`__exidx_end`, uses `.data >RAM AT>FLASH`, and defines
`_sidata = LOADADDR(.data)`. Thus startup's source address follows the actual
initialized-data load address rather than the earlier `_etext`. Vector-table
placement, RAM copy boundaries, and existing startup symbols remain explicit
script responsibilities; no symbol-name special case is implemented in C.

This is an engineering-profile configuration change, not a claim that the
unmodified upstream script accepts exception-table orphans. Passing the
standalone tests is also not evidence that a firmware build has linked or run
on hardware; those integration results must be recorded separately.

At the initial 147-object integration attempt (139 application objects plus
8 external runtime objects), input scanning and explicit script layout passed,
but linking stopped on an application object's out-of-section local symbol
`.L_arkcc_mcpy_done` before completion. That attempt produced no successful
firmware-link result and does not prove the runtime's final relocated table.
The symbol-layout issue is separate from PREL31 support; existing section-range
checks must not be relaxed to conceal it. See subsequent run artifacts for any
later integration outcome rather than treating this initial attempt as a pass.

## Evidence and regressions

[PREL31 tests](../tests/test_arm_prel31.py) exercise the production adapter and
VM, GNU assembler/linker byte oracles at two placements, section and Thumb FUNC
symbols, positive and negative addends, bit31 preservation, signed-range
boundaries, and unchanged bytes on overflow. They also reuse the existing ARM
relocation regression harness.

[Exception-index tests](../tests/test_arm_exidx_link.py) compile the production
link core independently of the main application. Real GNU objects exercise
retained section types, mapped `sh_link`, ordering, actual `.data` physical
load addresses, missing script matches, malformed headers/association/record
payloads, and unchanged old output on rejection. An optional local audited GNU
runtime member is copied byte-for-byte into the test directory when present;
it is explicitly external runtime input, not an xhcc-generated object.

GNU may coalesce adjacent CANTUNWIND entries (observed also with
`--no-merge-exidx-entries` in the installed toolchain). xhcc deliberately retains
input entries, so output sizes and `_sidata` need not equal GNU's. Tests compare
index semantics and each linker's `_sidata` against its own actual data LMA,
not an invalid universal byte-identity expectation for the whole image.

The original ELF reader, ARM link, and RV32 link regression suites remain
applicable. The supported subset does not imply acceptance of every libgcc
archive member.

External GNU runtime provenance and copying/license files must accompany the
runtime distribution. The presence of copied license files alone is not a
legal determination or an assertion of unrestricted redistribution rights;
consult the recorded vendor/runtime provenance and applicable terms.
