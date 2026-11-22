# XHRELOC v1 finite relocation VM

Implemented scope: ABS32, ABS64, PC32, PC64 and legacy 32-bit site-addend
variants. The legacy linker uses external formulas for internal types 4/5/8/9.
Standard ELF external references are still emitted as relocation records and
resolved by GNU/LLVM; this VM does not pre-resolve standard `.o` externals.
RV32 AUIPC pairs and ARM instruction patch formulas remain future work.

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

Limits: 16 KiB file, 32 rules, 32 operations per rule, 16 temporary values,
1..8-byte patch span. Only one target's table is active; up to eight target/path
registrations are copied. Tables are loaded on demand. Rules are straight-line;
there are no branches, cycles, native expressions, raw pointers or host calls.
Reading a temporary before its first assignment is rejected while loading.

Operations:

- `set rN S|A|P|B`, `const rN UINT64`.
- `read rN BYTE_OFFSET WIDTH` and `write rN BYTE_OFFSET WIDTH`, little endian,
  WIDTH in 1/2/4/8; both must fit the declared patch span.
- `add/sub/and/or/xor DST SRC SRC`, `shl/asr DST SRC SHIFT` with shift 0..63.
- `range SRC BITS signed|unsigned` with BITS 1..64.
- `align SRC POWER_OF_TWO`.
- `checkmask SRC MASK EXPECTED`.

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
