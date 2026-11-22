# Relocation formula externalization plan

This is an audit and implementation plan. The current object descriptor layer
externalizes ELF target metadata and relocation wire-type mappings. It does not
externalize the arithmetic or instruction patch formulas described below.

## Boundary between ELF output and local patching

Standard `-c` emits ELF ET_REL objects. External symbol relocations in those
objects remain unresolved and are applied by the external GNU/LLVM linker.
xhcc must not resolve or locally patch them as if their final addresses were
known. A new formula VM should initially replace native patching for resolved
internal assembler references, explicit legacy image linking, and carefully
identified REL placeholder/addend preparation. It must preserve the distinction
between internal relocation keys, ELF wire types, and local patch rules.

The current native reference points are:

- `src/core/compiler.c`, `ld_patch_ref`: absolute words, x86 PC-relative words,
  Thumb BL/B.W/conditional branches and MOVW/MOVT delta calls.
- `src/backend/riscv32/backend_riscv32.c`, `rv_patch_reloc`: AUIPC+ADDI/JALR
  pairs and absolute data addresses.
- `src/backend/arm/backend_arm.c`, `arm_patch_movw_movt`,
  `mov_imm_decode_a`, `mov_imm_encode_a`, and `resolve_refs_a`.
- `src/backend/x86_64/backend_x86_64.c`, `resolve_refs_x`: PC-relative disp32
  and absolute fields with addends.

## Formula inventory

| Family | Calculation | Required checks and fields |
| --- | --- | --- |
| ABS32/ABS64 | S+A, or B+S+A for image-relative legacy symbols | Bounds, explicit width/overflow policy, existing addend when applicable |
| x86 PC32 | S+A-P-4 | Signed displacement range; little-endian 32-bit field |
| RV32 AUIPC pair | d=S+A-P; hi=(d+0x800)>>12; lo=d-(hi<<12) | AUIPC opcode, ADDI/JALR opcode, signed HI20 range, preserved register/opcode fields |
| ARM MOVW/MOVT | Gather two old imm16 values, form imm32, add delta, scatter back | MOVW followed by MOVT, same Rd, valid second halfwords, full eight-byte bounds |
| Thumb BL/B.W | d=S+A-P-4, even byte alignment, S/I/J conversion | Signed branch range, J1/J2 inversion, two halfword writes |
| Thumb conditional B.W | PC+4-relative signed offset | Preserve condition bits, correct shorter signed range, alignment |
| ARM ADR T1 | S-Align(P+4,4) | 0..1020 in steps of four, preserve instruction/register bits |
| ARM CBZ/CBNZ | S-(P+4) | Current supported forward range and even alignment, preserve opcode/register bits |
| ARM narrow branch | (S-P-4)>>1 | Form-specific signed range and opcode preservation |

S is the symbol value, A the explicit or decoded addend, P the site address,
and B the image base where the legacy address convention requires it.
A rule must state which convention it uses; combining an image-relative symbol
with an absolute site by accident is not a valid relocation.

## Proposed bounded VM

Use a separate `XHRELOC 1` format rather than overloading ELF wire mappings.
The existing field encoding VM can supply the final scatter operation, but it
cannot by itself express address arithmetic, opcode validation, old immediate
gathering, or pair consistency checks.

Proposed limits for the first implementation:

- 16 temporary values per rule, 32 straight-line operations, no loops or jumps.
- At most two 32-bit instruction words / eight patched bytes per application.
- Fixed inputs S, A, P, B and OLD0/OLD1; explicit integer widths and signedness.
- Allowlisted operations: constants, add/subtract, bitwise and/or/xor/not,
  logical/arithmetic shifts, extract/gather, preserve-and-scatter.
- Assertions: bounds, masked opcode, signed/unsigned range, power-of-two
  alignment and equality of extracted register fields.
- No arbitrary expressions, pointer values, imports, host calls or raw code.

Illustrative C boundary:

```c
typedef struct {
    int64_t symbol;
    int64_t addend;
    uint64_t place;
    uint64_t image_base;
} XhRelocContext;

int xh_reloc_apply(const char *target, const char *rule,
                   unsigned char *code, int code_length, int site,
                   const XhRelocContext *context);
```

Final widths, error codes and context conventions must be specified before
implementing the first rule set. Do not reuse host pointer width or raw native
struct layout as part of the external rule protocol.

## Atomic patching requirement

Every application first checks the requested span and copies at most eight
bytes into local scratch. Decode, arithmetic, assertions and output scattering
operate only on scratch. Commit all bytes only after every operation succeeds.
Any malformed rule, range violation, wrong opcode, register mismatch or truncated
input leaves the original buffer unchanged. Errors propagate to the assembler
or linker and prevent final output, rather than substituting a zero encoding.

## Incremental implementation and evidence

1. ABS32 and PC32: externalize S+A and S+A-P-4, including explicit field addends.
2. RV32 AUIPC+ADDI/JALR: externalize rounding, signed low displacement and paired
   field preservation; retain the current native implementation as a test oracle.
3. ARM MOVW/MOVT: add validated gather/scatter of split immediates and same-Rd
   assertions.
4. Thumb branches and narrower PC-relative instructions: externalize PC bias,
   range/alignment and J-bit transforms after the arithmetic core is stable.

For each family, compare emitted bytes against the existing native path and
GNU/LLVM reference objects where applicable. Execute linked images at multiple
addresses. Cover positive and negative boundary offsets, addends, overflow,
misalignment, opcode corruption, mismatched registers and truncated buffers.
Failure tests must assert that the entire input buffer remains unchanged.
An external rule variant must change a real local patch result without changing
the compiler binary hash; correct default rules must keep existing mixed-tool
ELF and execution regressions passing.
