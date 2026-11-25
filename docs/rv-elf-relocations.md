# RV32 ELF RELA relocation adapter

The standard RV32 adapter is independently implemented from ELF/RISC-V psABI
relocation definitions. It does not reuse private ARKOBJ addend interpretation.
`xh_rv_elf_rela_width()` reports required bytes; `xh_rv_elf_rela_apply()` validates
site/opcode/context, then executes external `ELF_*` formulas through the shared
atomic relocation VM. A failure leaves bytes unchanged.

Supported types: NONE(0), R_RISCV_32(1), BRANCH(16), JAL(17), CALL/CALL_PLT(18/19),
PCREL_HI20(23), PCREL_LO12_I/S(24/25), HI20(26), LO12_I/S(27/28), ADD32(35),
SUB32(39), RELAX(51). Unknown types fail. This adapter does not relax instruction
sequences or add compressed-instruction support. The current core supports the
RV32IM/ILP32 non-RVC profile; fixed instruction sites are word aligned.

RELA addends are explicit signed 32-bit values. Absolute low/high formulas use
the 32-bit address pattern and rounded high20 calculation. Branch and jump
formulas compute S+A-P, enforce signed range and even alignment, preserve the
opcode/register/function fields, and scatter only immediate bits. CALL validates
an AUIPC/JALR pair with matching producer/consumer register, then patches both
words atomically. Arithmetic formulas and writes are package data rather than
ELF relocation-number remappings alone.

PCREL low relocations refer to a local high-site anchor, not directly to the
high relocation's target symbol. The linker core resolves the anchor to one
matching PCREL_HI20 relocation and supplies `pair_symbol`, `pair_addend` and
`pair_place` in `XhRvElfRelContext`, with `has_pair=1`. The adapter requires the
low's own addend be zero and computes the low bits from the high target/addend/
place. Missing/ambiguous anchors are rejected by the core. In contrast, plain
LO12 uses its own S+A, without a high-site pairing requirement.

NONE and RELAX do not write bytes. The linker core validates RELAX marker
association (including symbol/addend constraints); it is not a request to run
native relaxation. Weak-symbol and input-object target validation belong to
the core. Standard `.o` output still emits unresolved relocation records; this
adapter applies them only in an explicit link operation with resolved symbols.

`tests/test_rv_elf_reloc.py` builds the actual adapter/VM as Wasm. It checks
absolute/high/low results including high-bit address patterns, displacement
boundaries, malformed opcodes, mismatched CALL registers, missing low anchors,
truncated buffers, no-op types, and atomic rejection. A full-target Clang RV32
assembly fixture generates real RELA objects; applying all relevant relocations
with this adapter produces the exact `.text` bytes produced by LLVM lld with
relaxation disabled. Mixed-object linking and CPU execution are covered by the
separate generic linker integration tests.
