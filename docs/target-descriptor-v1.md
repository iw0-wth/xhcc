# Target descriptor v1

`target.rules` externalizes target identity, frontend data model, variadic slot
convention, assembly dialect capabilities, object metadata expectations and
package predefines. It does not add a new ISA or ABI. The native engine family
still selects existing code generation and calling convention implementation.

API: `xh_target_rules_set_path(engine,path)`,
`xh_target_rules_get(engine)`, `xh_target_rules_validate(descriptor,engine)` and
`xh_target_rules_release()`. The loader validates the descriptor against the
currently implemented engine contract before returning it.

```
XHTARGET 1
id custom-rv-board
engine riscv32
abi ilp32
pointer 4
long 4
int 4
longlong 8
slot 4
va_mode word_packed
vararg_words 1
complex 1
object_machine 243
object_class 32
endian little
armcc_asm 0
native_asm 1
arm_immediates 0
complex_words 4
slot64_ptrs 0
ir_word_bytes 4
macro __riscv 1
macro __riscv_xlen 32
macro __BOARD_REVISION__ 73
```

Every scalar field is required and unique. Macro records are optional, with
unique C identifier names and single non-whitespace token values. The package
may use a custom `id` while retaining a supported canonical `engine`; the ID
must not replace canonical internal engine markers. A consumer can expose it
as a separate quoted `__XHCC_TARGET_ID__` macro. Arbitrary quoted strings with
spaces or executable macro directives are outside this finite schema.

Supported current contracts:

| Engine | Pointer/long | Slot | Variadic mode | Object |
| --- | --- | --- | --- | --- |
| arm | 4/4 | 4 | word_aligned | ELF32 ARM/EABI5 |
| riscv32 | 4/4 | 4 | word_packed | ELF32 RISC-V/ILP32 |
| x86 | 4/4 | 4 | word_packed | ELF32 i386/SysV ILP32 |
| x86_64 | 8/8 | 8 | sysv | ELF64 AMD64/SysV LP64 |
| c | 4/4 | 8 | ordinal | emitted C source |

`slot` describes the variadic ABI slot, not the IR stack representation.
`ir_word_bytes` is 8 for C and 4 for machine engines. `complex_words` is 1 for C
and 4 for machine engines. `slot64_ptrs` is 1 only for AMD64, which carries an
8-byte pointer in a single stack slot despite its 4-byte integer IR word.
These are required fields, not inferred from target-name strings.

All use 4-byte int, 8-byte long long and little endian in the current WASI
compiler model. ARM/RV/C may enable their existing complex support; x86 scalar
engines cannot claim unsupported complex behavior. ARM enables ARMCC assembly
and ARM immediate syntax, C allows ARMCC parsing but has no native operand
binding, and the other machine engines use their native operand syntax.
Incompatible pointer/long/slot/ABI/dialect claims are rejected, not accepted as
if metadata alone changed generated code.

Files are limited to 16 KiB and 64 macros. Up to eight copied engine/path
registrations are retained. A single active heap descriptor holds copied
strings and macros; switching engines frees the previous descriptor. Files with
unknown fields, duplicates, embedded NUL, invalid numeric fields or unsupported
engine contracts fail loading.

Core integration should establish data layout and IR variadic/assembly flags
before parsing. Preprocessing should obtain width and architecture predefines
from this descriptor, then preserve normal command-line macro override order.
Object emission should cross-check descriptor class/machine with the selected
object serializer contract. The shared native engine registry is still a C
function table; external IDs are package identity, not native function names.

`tests/test_target_rules.py` executes the actual C loader in Wasm for all five
contracts. A custom RV32 ID/predefine file changes loader results with unchanged
VM SHA, while incompatible widths, slot/dialect/ABI changes, duplicate macros,
unknown fields and embedded NUL fail. Compiler-level integration tests remain
the authority for actual layout and emitted instructions.
