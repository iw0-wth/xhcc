# External encoding rules v1

The interpreter moves RV32 R/I/S/B/U/J bit layout and argument constraints out
of the compiler binary. A second finite selector moves scalar32 ADD/SUB/AND/
OR/XOR/MUL instruction choice and operand ordering into `selection.rules`.
C frontend semantics, remaining IR instruction selection, stack management,
register allocation, calling conventions, assembler mnemonics/pseudo instructions
and ELF relocation policy remain C code. A rule file alone does not add a new
ISA or ABI.

`xh_rules_set_path(path)` selects the package encoding file and invalidates the
single cached ruleset when its path changes. RV32 loads it only on assembly;
other backends never load encoding rules. `xh_rules_release()` drops the cache.
The default path for explicit legacy invocations is
`packages/riscv32/encoding.rules` relative to the preopened working directory.
Package loading should supply the full package-relative guest path.

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

Files are at most 32768 bytes, with at most 32 rules, eight arguments per rule,
and 32 bit slices per rule. The cache is heap allocated on first use; parsing
scratch is freed after load. Malformed versions, duplicate names/arguments,
overlapping fields, unknown rules, incorrect argument counts, invalid ranges
and misaligned operands fail compilation. There are no branches, loops,
filesystem directives, dynamic imports or arbitrary expressions in the grammar.

## Scalar instruction selection v1

```
XHSELECT 1
select ADD scalar32 add dst lhs rhs
select SUB scalar32 sub dst lhs rhs
```

`xh_rules_set_selection_path(path)` independently selects the package's table.
The default is `packages/riscv32/selection.rules`. Files are limited to 8192
bytes and 32 unique operations. Each record chooses a scalar32 operation,
one allowlisted three-register RV32 mnemonic, and `dst`/`lhs`/`rhs` selectors.
The first operand must be `dst`. The current stack emitter supplies `dst=t0`,
`lhs=t0`, `rhs=t1`; these are selectors, never raw inserted strings. Generated
instructions then pass through the external field encoder above.

The allowlisted native mnemonics are add, sub, and, or, xor, mul, sll, srl, sra,
slt, sltu, div, divu, rem and remu. Unknown/malformed records fail compilation.
An absent operation has an explicit C fallback preserving the remaining
backend. This table does not select floating point, 64-bit operations, calls,
branches, loads or stores, and does not define an ABI.

Tests copy a package and change ADD from `add` to `sub`; the unchanged compiler
binary compiles and executes the program as 13 and 7 respectively. Malformed
selection rules fail without creating an object. The separate encoding variant
changes a U bit scatter and executes as 4096 and 8192, again without rebuilding.

Run `python tests/test_rules.py` for actual C interpreter Wasm tests covering
all six layouts, signed range/alignment failures, malformed files, and changing
an external layout without recompiling the interpreter. Run
`python tests/test_targets.py` for RV32 ELF linking and Unicorn execution.
