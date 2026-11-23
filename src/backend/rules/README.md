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

## Scalar instruction selection v1/v2/v3

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
Version 3 additionally declares an external operand profile before any rules:

```
XHSELECT 3
register dst t0
register lhs t0
register rhs t1
allow add rrr
allow seqz rr
allow xori rri
select EQ scalar32
emit add dst lhs rhs
emit seqz dst dst
end
```

The example illustrates grammar, not an arithmetic-equivalent EQ definition.
`rr`, `rrr` and `rri` describe operand arity and register/immediate categories.
Immediate values are the finite language constants 0 and 1. Mnemonic and
register tokens are lexical identifiers; no whitespace, newline or comma
insertion is allowed. Every instruction destination selector must be `dst`.
The generic v3 engine contains no RV mnemonic whitelist or register spelling.
`xh_sequence_set_path(target,path)` / `xh_sequence_select(target,operation,
bindings,...)` support up to eight path registrations and a single active table
allocated for its actual rule count. Optional bindings must match the declared
profile. `xh_sequence_release()` discards the parsed table. Files are limited
to 16384 bytes, 32 operations, 64 allowed mnemonics and eight steps per operation.

The historical `xh_rules_*` APIs remain RV32 wrappers. Versions 1/2 retain an
explicit legacy RV32 profile for compatibility; version 3 production package
spells its registers and allowed instructions in the external file. The wrapper
checks its existing t0/t0/t1 calling contract against that profile. This does not
make another register convention or ISA executable through the native RV engine.
Generated instructions still pass through the assembler and field encoder.

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
