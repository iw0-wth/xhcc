# XHRELOC v1 finite relocation VM

Implemented scope: ABS32, ABS64, PC32, PC64 and legacy 32-bit site-addend
variants. The legacy linker uses external formulas for internal types 4/5/8/9.
Standard ELF external references are still emitted as relocation records and
resolved by GNU/LLVM; this VM does not pre-resolve standard `.o` externals.
RV32 AUIPC+ADDI/JALR pairs and absolute image-base words now use external
`RV_AUIPC_PAIR`/`RV_ABS32` formulas. ARM MOVW/MOVT delta pairs and wide Thumb
BL/B.W/conditional branch formulas are also supplied in the ARM package and
applied by its validated wrappers, including known local wide-branch emission.
The x86 assembler applies external `LOCAL_ABS32`/`LOCAL_ABS64` and PC formulas
to resolved legacy references, carrying explicit addends and writing full widths.
Standard ELF mode preserves relocation records and does not prepatch these sites.

API: `xh_relocation_set_path(target,path)`, `xh_relocation_load(target)`,
`xh_relocation_apply(target,rule,code,length,site,context)` and release.
Context fields are 64-bit bit patterns: S=symbol, A=addend, P=actual patch site
address, B=image base. Negative addends use two's-complement uint64 values.
PC rules themselves subtract their pipeline bias (currently four); callers do
not subtract that bias. Legacy symbol/site coordinates are both image-relative,
so their common image base cancels. Absolute targets are supplied as appropriate
by the legacy linker before applying the rule.

```
XHRELOC 1
rule PC32 4
set r0 S
set r1 A
add r0 r0 r1
set r1 P
sub r0 r0 r1
const r1 4
sub r0 r0 r1
range r0 32 signed
write r0 0 4
end
```

Limits: 32 KiB file, 32 rules, 128 operations per rule, 16 temporary values,
1..8-byte patch span. Only one target's table is active; up to eight target/path
registrations are copied. Tables allocate only the actual number of rules and
load on demand; unused target tables are freed. Rules are straight-line;
there are no branches, cycles, native expressions, raw pointers or host calls.
Reading a temporary before its first assignment is rejected while loading.

Operations:

- `set rN S|A|P|B`, `const rN UINT64`.
- `read rN BYTE_OFFSET WIDTH` and `write rN BYTE_OFFSET WIDTH`, little endian,
  WIDTH in 1/2/4/8; both must fit the declared patch span.
- `add/sub/and/or/xor DST SRC SRC`, `shl/shr/asr DST SRC SHIFT` with shift 0..63.
- `not DST SRC`, `eq SRC SRC` (alias `equal`) as an equality assertion.
- `range SRC BITS signed|unsigned` with BITS 1..64.
- `align SRC POWER_OF_TWO`.
- `checkmask SRC MASK EXPECTED` and
  `checkanymask SRC MASK FIRST_EXPECTED SECOND_EXPECTED`.

All arithmetic is explicitly modulo 2^64. Arithmetic right shift is implemented
using unsigned shifts and sign fill, avoiding signed-shift assumptions. Range
checks interpret signed values as two's-complement patterns. A write without a
preceding range assertion deliberately truncates to its declared width; this
preserves the legacy site-addend wrapping behavior. Rules requiring stricter
semantics must include the appropriate range/align assertion.

Every apply copies the entire declared span into an eight-byte scratch buffer.
Reads and writes occur only within scratch. A bounds, range, alignment or opcode
assertion failure leaves every original byte unchanged, including when a write
preceded the failed assertion. Only full successful evaluation commits bytes.
Load or apply errors propagate as compiler diagnostics and prevent image output.

`tests/test_relocation_rules.py` builds the actual C VM as Wasm. It covers
absolute and PC-relative 32/64-bit results, negative addends, exact signed PC32
limits, malformed/uninitialized expressions, mask checks, truncated sites, and
atomic failure after a scratch write. It also compiles two legacy x86 objects,
links their cross-object call and executes the result (20) in Unicorn. Copying
the external rules and changing PC bias from four to eight changes the real
linked image bytes while compiler SHA256 and private objects remain unchanged.
The intentionally incorrect variant is inspected as bytes, not presented as a
valid executable. Correct default formulas retain the execution regression.

RV32 pair rules check the AUIPC first opcode and ADDI/JALR second opcode,
compute `d=S+A-P`, `hi=(d+0x800)>>12`, `lo=d-(hi<<12)`, assert signed HI20/LO12
ranges and preserve all non-immediate fields. Both instruction words commit
atomically. Tests compare 28 signed boundary/interior cases to the original
native arithmetic oracle, reject opcode corruption/truncated input/out-of-range
pairs without byte changes, and execute a legacy RV32 cross-object call returning
20. Changing only the copied pair rule's P expression changes real linked bytes
while keeping compiler SHA and input objects unchanged. Embedded NUL bytes in
rule files are rejected rather than truncating parser input.
