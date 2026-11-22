# External encoding rules v1

The interpreter moves RV32 R/I/S/B/U/J bit layout and argument constraints out
of the compiler binary. A second finite selector moves scalar32 arithmetic, bitwise operations,
shifts, comparisons and signed/unsigned DIV/MOD instruction sequences and
operand ordering into `selection.rules`.
C frontend semantics, remaining IR instruction selection, stack management,
register allocation, calling conventions, assembler mnemonics/pseudo instructions
and ELF relocation policy remain C code. A rule file alone does not add a new
ISA or ABI.

`xh_encoding_set_path(target,path)` registers an external file for a target;
`xh_encoding_load(target)` and `xh_encoding_encode(target,rule,args,count,word)`
load and execute it. One active parsed cache is shared across targets, replaced
when a different target loads, so unused target tables consume no parsed RAM.
Up to eight copied target/path registrations are retained. The RV32
`xh_rules_*` APIs remain compatibility wrappers for canonical `riscv32`.
`xh_encoding_release()` drops the active cache. Default legacy paths are
`packages/riscv32/encoding.rules` and
`packages/arm/arm.encoding.rules`; packages supply complete guest paths.
The unsigned encoded word supports 16-bit Thumb values or 32-bit Thumb-2 values
(first halfword in bits31..16); target emission decides byte order/width.
A zero-argument rule may emit a constant BASE without any bit fields.

The parser accepts whitespace-separated records and `#` comments:

```
XHENC 1
rule I 5 0
arg 0 12 1 1
arg 1 5 0 1
arg 2 3 0 1
arg 3 5 0 1
arg 4 7 0 1
bits 0 0 12 20
bits 1 0 5 15
bits 2 0 3 12
bits 3 0 5 7
bits 4 0 7 0
end
```

`rule NAME ARGCOUNT BASE` defines one 32-bit encoded instruction layout.
`arg INDEX WIDTH SIGNED ALIGNMENT` defines operand range and byte alignment;
SIGNED is 0 or 1, and alignment must be a power of two.
`bits ARG SOURCE_LSB WIDTH DEST_LSB` copies a slice of an operand into the
encoded word. Multiple slices can scatter an immediate across nonadjacent
instruction fields. Output bit ranges may not overlap each other or BASE.

Files are at most 65536 bytes, with at most 256 rules, eight arguments per rule,
and 32 bit slices per rule. The cache allocates only the actual rule count on
first use; parsing
scratch is freed after load. Malformed versions, duplicate names/arguments,
overlapping fields, unknown rules, incorrect argument counts, invalid ranges
and misaligned operands fail compilation. There are no branches, loops,
filesystem directives, dynamic imports or arbitrary expressions in the grammar.

## Scalar instruction selection v1/v2

```
XHSELECT 2
select LE_S scalar32
emit slt dst rhs lhs
emit xori dst dst 1
end
```

Version 1 single-line records remain accepted:
`select ADD scalar32 add dst lhs rhs`. Version 2 has explicit `select` / `emit`
/ `end` records with at most eight instructions per selected operation.
`xh_rules_set_selection_path(path)` independently selects the package table.
The default is `packages/riscv32/selection.rules`. Files are limited to 8192
bytes and 32 unique operations. Each instruction chooses an allowlisted
mnemonic and `dst`/`lhs`/`rhs` register selectors; xori additionally accepts
immediate 0 or 1. seqz/snez have two register operands. Every instruction writes
`dst`; no instruction can write stack/frame state or insert arbitrary text.
The current stack emitter supplies `dst=t0`, `lhs=t0`, `rhs=t1`.
Generated instructions then pass through the external field encoder above.

Native mnemonics are add, sub, and, or, xor, mul, sll, srl, sra, slt, sltu, div,
divu, rem, remu, seqz, snez and xori. The default package covers ADD/SUB/MUL,
AND/OR/XOR/SHL, SHR_S/U, EQ/NE, LT/GT/LE/GE_S/U and DIV/MOD_S/U (23 records).
Both IR_BINARY and the separate IR_DIV/IR_MOD operations use this table.
Malformed sequences, duplicate selectors, unknown mnemonics, incorrect arity,
invalid operand kinds, missing end and oversized sequences fail compilation.
An absent operation has an explicit C fallback preserving other backends and
older partial tables. Floating point, 64-bit operations, calls, control flow,
loads/stores, register allocation and ABI remain C code.

Tests copy a package and change ADD from `add` to `sub`; the unchanged compiler
binary compiles and executes the program as 13 and 7 respectively. Malformed
selection rules fail without creating an object. The separate encoding variant
changes a U bit scatter and executes as 4096 and 8192, again without rebuilding.

Run `python tests/test_rules.py` for actual C interpreter Wasm tests covering
all six layouts, signed range/alignment failures, malformed files, and changing
an external layout without recompiling the interpreter. Run
`python tests/test_targets.py` for RV32 ELF linking and Unicorn execution.
