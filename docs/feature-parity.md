# xhcc / arkcc feature parity

This is an implementation audit and a bounded regression matrix, not a claim of
ISO C conformance or binary compatibility with every compiler named by a driver
alias. The reference is the local `arkcc` source tree. Build integration, target
object formats and runtime packages are maintained separately from language
parity. A compiled-in switch alone is not evidence that a feature was exercised.

## Audited source relationship

Before these changes, every file in `src/ir` and every C frontend file was
identical to its arkcc counterpart, except `type.c` (empty loop braces) and
`preproc.c` (platform buffer sections, resolved relative include paths, forced
includes and dependency paths). No missing parser or IR implementation was found
by that comparison. `xh_small.h` instead selected STM32_MIN and explicitly
removed DWARF, FPU emission, IPO, object IR, inlining and vectorization; STM32_MIN
also removed numerous optional passes indirectly.

The bounded profile now selects HOST_FULL feature defaults while preserving
small buffers. All buffer/backend/profile definitions can be overridden with
`-D`. This restores compilation of the reference implementations; the driver
must still expose the runtime flags and target paths, and tests must exercise
them. The safety changes to IR generation described below deliberately reject
reference paths that silently emitted incorrect code.

## Language and target matrix

| Area | Inherited implementation / current boundary |
| --- | --- |
| C99 subset | Declarations in blocks/for, designated and aggregate initializers, compound literals, VLA, `_Bool`, `restrict`, `inline`, long long, variadic functions, function pointers, struct/union/enum and bitfields are inherited. Regression coverage compiles representative combinations, not all standard corner cases. |
| Floating point and complex | Float/double conversions, floating constant folding, `_Complex` and wide literals are retained. Machine ABI restrictions, helper runtime availability and backend correctness still apply; an accepted source is not proof of arithmetic execution on every ISA. |
| C11 subset | The six inherited keywords are `_Alignas`, `_Alignof`, `_Static_assert`, `_Generic`, `_Thread_local`, `_Atomic`. `-std=c11` selects this subset. It does **not** mean complete C11. `_Noreturn`, the full C11 library, threads and full standard atomic semantics are not established. |
| Alignment | `_Alignas` is checked against natural alignment; the reference explicitly rejects block alignment above 8 bytes. |
| TLS | The parser retains TLS markers and constraints. Machine output currently rejects TLS explicitly because TLS sections/relocations/runtime are not implemented. C output can retain the declaration; no working thread runtime is established. |
| Atomics | `_Atomic` type/qualifier parsing is inherited; ordinary layout is used and operations rely on the target header/runtime. This does not establish lock-free atomics or the complete C11 memory model. |
| GNU extensions | Attribute and keyword parsing is inherited. `-fno-builtin` suppresses implicit libc declarations and ordinary complex-library lowering (`creal`, `cimag`, `conj`, `cabs`, `carg`, `cexp` and f/l variants), preserving declared calls. No `__builtin_*` implementation is present in the audited source. Actual code emission depends on the particular attribute and target; accepting `__attribute__` is not complete GCC compatibility. |
| ARMCC syntax | `__irq`, `__weak`, `__naked`, `__packed`, `__align`, `__at`, `__svc`, `__softfp`, `__forceinline`, arm pragmas, asm functions and other existing keywords remain parsed. They are ARM extensions; other targets do not acquire their ARM semantics merely by parsing them. |
| Preprocessor | Macro expansion, conditional directives, includes, trigraphs, wide literals, macro redefinition checking and dynamic macro bodies are retained. Capacity limits remain explicit below. |

The C emitter's declared ABI supports at most **16 call arguments**. Calls above
that capacity fail with an argument-count diagnostic; the 17th-FP-class regression
therefore tests rejection for C and correct emission for machine targets. C uses
one ordinal stack slot per scalar/complex value, including double and long long;
discarded expressions and small aggregate results follow that single-slot rule.

## Inline assembly: verified scope and reference defects

Basic `asm`, `__asm` and `__asm__` statements retain user text for the selected
backend. Users must supply instructions for that target. `volatile` is accepted;
IR assembly is conservatively treated as a side-effecting statement.

Operand-free extended asm with `"memory"` and `"cc"` clobbers is retained. A new
operand-aware native subset accepts one 32-bit local/parameter `=r` output and
up to two `r`/`i` inputs. Register inputs are 32-bit scalar identifiers or integer
literals; immediate inputs must fold to a signed 32-bit integer constant. IR
evaluates register inputs, the backend binds registers, and the output is stored
back to its local slot. Numeric `%N` placeholders are renumbered after immediate
substitution; `%%` escapes a literal percent. ARM `i` substitutions include
GCC's `#` prefix; an existing `#%N` template avoids a duplicate prefix. The
production output suite uses a standard `%N` template and verifies that the
result includes the ARM immediate prefix. Destination execution remains a
separate CPU test.
Assembly syntax remains native to
the selected ISA. C emission rejects operand binding because its stack-slot
locals are not original C identifiers.

`+r`, early-clobber, tied/named operands, register clobbers, multiple outputs,
memory operands, output globals/static locals, 64-bit operands and nontrivial
register-input expressions remain explicitly rejected. The supported subset is
not full GCC constraint allocation. `memory`/`cc` clobbers and volatile forms are
accepted; ARM functions containing asm skip text register optimizations so these
optimizations cannot move values across the opaque user statement.

The original ARM/RISC-V emitters merely substitute `%0` through `%3` with
`r0` through `r3` / `a0` through `a3`, ignoring constraints and never evaluating
inputs or writing outputs. This can return a successful status with wrong
machine code. x86 rejects numbered placeholders but does not implement operand
binding. The C emitter preserves raw operand source strings even though lowering
has replaced C locals with stack slots, so its apparent pass-through is not a
valid implementation of arbitrary operands either. xhcc rejects these unsafe
paths outside the implemented operand subset before backend emission.

ARMCC `__asm` function bodies are emitted by ARM, and passed through as raw
assembly by the C emitter (requiring a compatible downstream assembler). x86 and
RISC-V lack that body-emission path; xhcc now rejects them instead of silently
emitting empty functions. Basic statement assembly remains available on those
targets. The C emitter's assembly pass-through does not translate ARM mnemonics
to a host ISA.

The new common GNU `=r`/`r`/`i` subset parses operand expressions from a replay
lexer and restricts their shape before emitting IR. Expanding it further requires
full semantic typing and general lvalue lowering, safe register/clobber allocation,
and preservation through optimization and IPO/object IR. Changing template
substitution alone does not satisfy these requirements.

## IR call metadata compatibility

The argument wire table keeps its existing serialized 32-bit integer records.
Low 16 bits carry the wire byte count; bits `0x10000` and `0x20000` retain
alignment and pointer-slot semantics. The new `0x40000` bit identifies scalar
float/double ABI class, including arguments after index 15 where the legacy
2-bit argument mask has no capacity. Such later FP arguments force a side-table
record. Current readers use the new flag together with the legacy low-16-argument
mask. The ARKIR1 byte layout/version remains unchanged, but an old consumer that
ignores this semantic flag cannot safely execute a call with later FP arguments;
transport/API capability negotiation must require matching call-class semantics.
This is additive record encoding, not a claim of semantic compatibility with all
historical compiler binaries.

## Optional SysV ABI plan recording and certified native use

IR container version 2 persists independently indexed call/function plans and
accepts v1 payloads with no plans. Canonical plan codec v2 certifies scalar-only
plans with flags=1, supported small-integer-structure plans with flags=2, and
single-SSE floating-structure signatures with flags=4, and certified two-long
integer-pair signatures with flags=8, and exact LD/DL/DD pair signatures with
flags=16, plus certified IF/FI single-INTEGER signatures with flags=32.
Only 0/1/2/4/8/16/32 are accepted, mutually exclusive values; OR combinations
such as 3/5/6/7/9/17/31 and unknown values are rejected. Old canonical v1 plans decode
with flags=0 and cannot drive native use. `-frecord-sysv-abi` records supported
classifications without changing machine ABI. `-fuse-sysv-abi-plan` is a separate
x86_64-only opt-in that validates the rule fingerprint and consumes certified
locations. Defaults retain the previous calling path.

Scalar integers, float/double, pointers, register/stack spills and scalar
variadic access are covered. The aggregate subset is nonempty structures of
1 through 8 bytes with integer/pointer fields or recursively certified nested
structures, proper natural alignment and in-bounds fields, no packing/bitfields
and no aggregate variadic actuals. Natural pure-floating F4 {float}, F8
{float,float} and D8 {double} are also supported when the classifier proves
exactly one SSE fragment (`abi.plan-small-sse-struct.v1`). Flags=4 may combine
separate eligible scalar/integer-structure/SSE-structure parameters, but mixed
integer/float fields inside one structure remain unsupported. An additional
`abi.plan-int-pair.v1` subset supports exactly two naturally aligned TYPE_LONG
fields of 8 bytes at offsets 0 and 8 in a 16-byte struct. Its argbytes entry
uses `IR_ARG_BYVALUE_ADDRESS=0x80000`, object size 16 and optional alignment only;
the backend reads two by-value fragments from the single IR address. The existing
CALL bit36 reserves an internal caller result buffer, while plan.sret=0 means
no ABI hidden RDI pointer: RAX/RDX hold the result and are copied into that buffer.
`abi.plan-mixed-pair.v1` additionally certifies only the natural 16-byte layouts
{long,double}, {double,long} and {double,double}, exact TYPE_LONG/TYPE_DOUBLE
fields at offsets 0 and 8. INTEGER/SSE argument and result banks are allocated
independently; insufficient capacity in either bank rolls the entire object to
the stack without partially consuming either bank. DD returns in XMM0/XMM1;
LD/DL use independent RAX/XMM0 locations. `abi.plan-small-mixed.v1` adds only
natural 8-byte {TYPE_INT,TYPE_FLOAT}/{TYPE_FLOAT,TYPE_INT} at offsets 0/4,
4-byte fields and alignment 4, with source proof and a single INTEGER
classification. Their float field travels as raw GPR/RAX bits, preserving
negative zero and NaN payloads. Short/float and arbitrary mixed types are not
implied. Unions, arrays, other 16-byte/larger
forms, arbitrary mixed layouts and MEMORY/sret remain unsupported in use mode,
though some can still be recorded.
The `abi.plan-small-int-struct.v2` capability covers exact partial-byte access:
`IR_AUX_AGGREGATE_EXACT=0x40000` loads only the real 1–8 object bytes while
retaining the one/two-word IR shape; stores and parameter copies preserve exact
extents. Older 4/8-only consumers must not claim this capability.

Eleven codegen tests passed on root 911911, including integer struct1–8,
F4/F8/D8, two-long struct16, exact LD/DL/DD and IF/FI args/returns, independent GPR/SSE
budget exhaustion and whole-object rollback/spills, pointer fields, multiple
live/nested return buffers and unchanged adjacent canaries, with xhcc/xhcc and
both Clang mixing directions. The legal function name `dl` remains in the fixture;
an exposed assembler symbol/register ambiguity was fixed without renaming it. Scalar two-TU/FP/variadic tests, unsupported aggregate
rejections and stale-fingerprint LTO rejection also passed. No Clang test was
skipped. This proves the tested small-integer-structure subset, not complete
SysV aggregate ABI interoperability.
See [ABI plan IR v2](abi-plan-ir-v2.md) for the wire fields, capacities and exact
compatibility/verification boundaries.

## Restored optional implementations and runtime controls

| Implementation | Reference runtime controls / limits |
| --- | --- |
| Constant fold and linear DCE | `-O1` or higher, `-fopt-constfold`, `-fopt-dce`; disabled at `-O0`. |
| CFG DCE | `--dce-cfg`, full pipeline and at least `-O1`. |
| Inlining | `--autoinline`, `-fopt-inline`, `-O2`; full pipeline, minimum `-O2`. |
| SIMD pattern vectorization | `--vectorize`, `-fopt-vectorize`, `-O2`; limited inherited pattern recognition and backend SIMD support, not a general vectorizer for all ISAs. |
| Loop optimizations | `--licm`, `--unroll`; full pipeline, minimum `-O1`, MIR required. |
| Block sharing | `--tailmerge`, `--xblock`; inherited legality and size thresholds, full pipeline. |
| Global constant propagation | `--const-global`; useful after inlining / whole-program merge, at least `-O1`. |
| ARM code emission / assembly passes | `-fopt-regalloc`, `-fopt-regalloc-near`, `-fopt-vstack`, `-fopt-addr`, `-fopt-strength`, `-fopt-cbz`, `-fopt-branch`, `-fopt-imm`, `-fopt-peep`, `-fopt-cbranch`, `-fopt-copyprop`, `-fopt-live`, `--reg-top`, `--tailcall`, `--divconst`. ARM-specific passes do not imply equivalent implementations on other ISAs. |
| Diagnostics | `--warn-mem`, `--warn-enum`, `--warn-unused-result`, `--warn-unreachable`, `--warn-unused-static`, `--warn-recursion`; independently opt-in. |
| Debug information | `-g` / `-gdwarf-2`, `-fdwarf` / `-fno-dwarf` through configuration; backend/object-writer support must be verified for the selected output path. |
| IPO | `--ipo` / `-flto`; multiple translation units must remain in the arena simultaneously. Driver integration and capacity are separate from feature compilation. |
| Object IR and LTO | `-c --obj-ir`, `-link --lto`; inherited private ARKOBJ2 serialization, not GCC/LLVM LTO or automatically compatible with public ELF conversion. |
| Partial link / relocation / scatter | `-r`, `--reloc-data`, `--reloc-text`, `--scatter`; object/linker target support and driver plumbing must be checked separately. |
| Pipeline introspection | `--print-pipeline`, `--pipeline=full|lite`, `--size-weight=N`; allows inspection of compiled-in and runtime-gated stages. |

## Target selection and RAM

Production uses one all-target main Wasm containing a single copy of the common
C frontend and IR implementation. Version 3 configuration packages select the
compiler-name interface, target, ABI and backend rules; they do not load child
compiler Wasm modules. The earlier adapter/frontend/backend staged prototype is
not the production deployment. Its historical tests below are labeled separately.

The backend rules engine is being integrated incrementally. All five backends
now consume external packages: ARM, i386, AMD64 and C use a shared bounded
template engine; ARM and RV32 also use the shared field encoder, and RV32 uses
external scalar instruction sequences. See backend-externalization.md for the
consumed rules, execution evidence and remaining compiled algorithms. A configuration package alone does
not establish that every target's instruction selector and encoder have moved
out of the main module. The code owner records the rules actually externalized;
remaining compiled backend logic must not be described as arbitrary downloadable
compiler support.

IR is target dependent: parsing fixes structure layout, pointer width, long width
and calling-convention details. The runtime must select a consistent target/ABI
before frontend execution and keep it through emission. ARKIR1 object payloads
retain strict opcode/name/asm validation. The prototype XHIR stage envelope is
not a production requirement for in-process frontend/backend dispatch.

The shared header defaults to `XH_TARGET_ALL=0` for the production all-target
artifact. Optional development target builds use `-DXH_TARGET=N`: 1 ARM,
2 RISCV32, 3 x86 (i386), 4 x86_64, 5 C.
`XH_TARGET_ARM`, `XH_TARGET_RISCV32`, `XH_TARGET_X86`, `XH_TARGET_X86_64`,
`XH_TARGET_C` are readable symbolic equivalents. `XH_MODULE_TARGET` is a build
and driver identity string, separate from this numeric selection. The production
module enables all backends and runtime configuration chooses one per invocation.
Optional slim development builds must reject unavailable targets; disabling ARM
also requires resolving the core assembler/object API coupling. Target data
models are selected before each frontend run:
`type_set_long_size(8)` for x86_64 LP64 and 4 for the ILP32 machine targets;
`type_set_ptr_size` selects pointer width separately. Long alignment, integer
literal candidates, arithmetic conversions and wide integer IR paths follow
this data model. `g_irgen_armcc_asm` must be set per translation unit to permit
ARMCC assembly functions only on ARM/C. Individual `ARKCC_BACKEND_*` overrides remain available for
advanced builds, but disabling ARM requires resolving the core coupling first.

The RAM goal is approximately 4 MiB per process/module, with explicit build
memory overrides where needed. The former fixed 3 MiB maximum was a Wasm linear
memory limit, **not** a module file-size limit. Restoring optional code can grow
the compiler file independently of RAM. Buffer defaults are:

| Resource | Default |
| --- | ---: |
| Arena `ARK_POOL_SIZE` | 1 MiB |
| Source `ARK_SRC_BUF_SIZE` | 32 KiB |
| Assembly `ARK_ASM_BUF_SIZE` | 128 KiB |
| Code / link buffers | 32 KiB each |
| Object `ARK_OBJ_BUF_SIZE` | 64 KiB |
| Preprocessed text `PP_BUF_SIZE` | 64 KiB |
| Include buffers | 8 levels × 16 KiB |
| Macro body hard cap | 16 KiB |
| ARM symbols / relocations / globals | 512 / 512 / 128 |
| Link symbols / relocations / objects | 64 / 64 / 16 |
| Include directories / predefined macros | 32 / 64 |

These bounds can reject otherwise supported programs. Increase the corresponding
`-D` values and Wasm memory cap together; the arena must hold at least twice the
link buffer. IPO/LTO, large AST/IR and debug data compete for the same arena.
The ARM liveness pass originally had approximately 1.5 MiB of static workspace.
The target integration allocates it with `malloc` only when requested and frees
it after the pass, so ordinary compilation does not carry that static RAM cost.
Large liveness workloads still consume the module's linear memory and may need a
larger configured memory cap; the final build map and runtime peaks remain the
evidence for the approximately 4 MiB goal.

## Regression evidence and outstanding validation

`tests/test_language.py` compiles C99 constructs, all six C11 subset keywords,
checks a failing static assertion, checks `_Generic` selected constant output,
compiles long long / variadic / indirect calls, inspects basic and operand-free
extended asm output, verifies unsupported operands/register clobbers fail,
checks ARMCC asm function target restrictions and TLS rejection/preservation,
checks LP64 long layout/literal rank, and feeds real IR to each optional high-level
pass. A common IR guard rejects unsupported machine TLS before code emission.
The current production main-module run passed all 18 cases on each of ARM,
RISC-V, i386, x86_64 and C: **90/90**, through compiler-name selection and version
3 configuration packages. Each invocation loads the same main Wasm once. This
includes machine TLS rejection, LP64 layout/literal rank, GNU asm operand
binding with the corrected ARM `i` prefix, and `-fno-builtin` preservation of
declared calls and rejection of undeclared library names. Three added cases
adapt representative reference layout/bitfield and function-typedef/compound
literal semantics, and verify unsupported atomic read-modify-write is rejected
instead of lowered nonatomically. These are compilation/output checks;
destination-ISA execution is a separate target test suite. It accepts `XHCC_COMMAND` and `XHCC_TEST_TARGET` to
exercise every built module via Wasmtime. It never starts a build itself.

`tests/test_legacy_parity.py` exercises the original `--ark-compat` path with
bounded real sources: private object `ARKIR1` payloads, IPO cross-file inline
output, LTO byte-identical assembly, partial-link image identity, actual memory
lint warnings, scatter reset-vector relocation and explicit rejection of LTO
objects lacking IR. These gates adapt the reference `run_objir_tests.py` and
`run_partiallink_tests.py` output checks; they do not infer behavior from macros.
The current production artifact `build/wasm/xhcc.wasm` passed all **7/7** legacy
gates through Wasmtime using its explicit `--ark-compat` interface. The seventh gate additionally round-trips
bound GNU asm through private object IR/LTO, checks byte-identical direct IPO
assembly, and corrupts the serialized output metadata to confirm explicit
rejection. They run the same unified main artifact; the explicit legacy driver
retains original private-object/link options in addition to the modern interface.

## Repaired: full-lexeme floating-literal rounding in WASI/full profile

The previous lexer parsed exponent-free decimal floats with repeated binary
multiplication/addition, producing the incorrect historical results below.
The WASI/full-profile scanner now converts the complete decimal or hexadecimal
lexeme with `strtod`, or `strtof` for a float suffix, and validates its end pointer.
It does not allocate a truncated 64-byte copy. The old accumulation reproduced:

| Literal | Current accumulation: binary64 hex | Correct binary64 hex | Error |
| --- | --- | --- | ---: |
| `9223372036854775808.0` | `43e0000000000001` | `43e0000000000000` | +2048 |
| `9223372036854774784.0` | `43dffffffffffffe` | `43dfffffffffffff` | -1024 |

`tests/test_float_literals.py` now links and executes ARM and AMD64 results for
21 IEEE cases, including these values, exponent/no-exponent consistency,
long mantissas, hexadecimal floats, subnormals and direct float-suffix rounding.
Integer scanning uses checked unsigned accumulation; invalid syntax and values
above ULLONG_MAX are rejected. Floating tokens do not first overflow an integer
prefix or convert their value to an out-of-range integer token field.

The separate STM32_MIN manual conversion path is not covered by this WASI
rounding guarantee and still needs a bounded rounding-correct implementation.
These tests establish the stated cases, not complete ISO decimal conversion
conformance under every locale or environment.

Pending final integration results must distinguish output inspection from
execution on the destination ISA. No claim here establishes full C99/C11,
complete GCC/Clang/ARMCC command compatibility, unrestricted source capacity,
arbitrary GNU operand assembly beyond the tested subset, all-target DWARF, public ELF LTO, or complete
cross-target atomic/TLS/runtime support. Those are explicit remaining limits,
not capabilities restored by selecting a macro.
