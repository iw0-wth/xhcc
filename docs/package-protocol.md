# xhcc configuration package protocol, version 3

The desktop host runs one shared `xhcc.wasm` containing the compiler frontend and supported backend implementations. External packages contain target configuration, command dialect/options and headers. They do not provide executable backend Wasm modules. The host chooses an installed tool alias, passes its raw arguments to the shared compiler, and waits for that one process to exit.

This architecture deliberately keeps supported backend code in the shared module. Selecting a configuration does not unload the other compiled backend implementations or prove that their code/tables consume no RAM. There is no launcher callback, staged worker lifecycle, per-package module loading, CompileRequest queue or external IR transport in the production version 3 host.

## Flat manifest

Each package is an immediate child directory of the package root and contains UTF-8 `package.ini`:

```ini
version=3
id=arm
tools=arm-none-eabi-gcc,armcc
target=arm
cpu=cortex-m3
abi=aapcs-soft
include=include
dialect=gcc
options=options.ini
```

The required fields are `version`, `id`, `tools`, `target`, `cpu`, `abi`, and `include`. Optional `dialect` is `gcc` or `armcc`; optional `options` names the package's option mapping file. Optional `engine` selects an engine understood by the shared compiler, `encoding` names its external encoding-rule file, `selection` names its external IR selection-rule file, and `templates` names a code-generation template file such as `arm.rules`, `x86.rules` or `c.rules`. For example, a RISC-V package can use `engine=rv32-rule-v1`, `encoding=encoding.rules` and `selection=selection.rules`. The compiler owns interpretation of engines, rules and option mappings. The Python host accepts the engine identifier without interpreting its contents and validates the rule files' package-relative paths; it does not parse encoding/selection rules, compiler option definitions or compiler argument positions.

Optional `requires` declares compiler capabilities, for example `requires=encoding.fields.v1,selection.scalar32.v2`. It is a comma-separated list of unique nonempty ASCII names matching `[a-z][a-z0-9]*(?:[._-][a-z0-9]+)*`; whitespace within the list, empty entries, duplicates and malformed names are rejected. Omitting the field preserves legacy behavior; an explicitly empty field is invalid. The host checks syntax only and accepts unknown syntactically valid capabilities. The shared compiler decides whether the selected package's requirements are supported and diagnoses unsupported requirements. This avoids binding package acceptance to the Python host's release or a host-maintained capability allowlist.

Current compiler capability names are `package.options.v1`, `encoding.fields.v1`, `selection.scalar32.v1`, `selection.scalar32.v2`, `object.elf32`, `object.elf64`, `codegen.templates.v1`, `object.descriptor.v1`, `relocation.vm.v1`, `cpu.descriptor.v1`, `target.descriptor.v1`, `semantic.selector.v1`, `abi.decision.v1`, the opt-in metadata capability `abi.plan-record.v1` and the certified native scalar-plan capability `abi.plan-scalar.v1` and the versioned narrow structure-plan capabilities `abi.plan-small-int-struct.v1` and `abi.plan-small-int-struct.v2`. Selection versions declare the corresponding rule VM contract, not universal IR/ABI support. A newer package can require a newer capability explicitly; a package without `requires` receives no new compatibility guarantee from omission. Requirements are capability contracts independent of product version numbers.

`semantic.selector.v1` declares the bounded shared semantic selector contract:
`XHSEMANTIC 1` and legacy `CSELECT 1` / `CSELECT1` headers, normalized
family/operation/width/sign/form matching, template references, and validated
pop/push effects. It does not mean arbitrary IR lowering or every family is
implemented by every backend. A package needs its selected backend's operand
and effect contract as well as the shared parser. Unknown semantic record
versions fail even when the package requires the supported v1 capability.

`abi.decision.v1` declares the finite, source-ordered first-match ABI decision
relation and its validated argument/return inputs and result fields. It does
not authorize changing a frontend-coupled wire representation independently,
and does not claim complete SysV aggregate interoperability. The production
C/ARM/RISC-V/x86 packages declare the capabilities their active records consume;
adding a requirement alone does not make an inactive library a production API.
The optional `aggregate=aggregate.rules` path supplies planning rules shared by
two separate x86_64-only opt-ins. `-frecord-sysv-abi` and its
`abi.plan-record.v1` capability record typed IR plan metadata only; recording
does not switch the machine calling ABI. The separate `-fuse-sysv-abi-plan`
option records and consumes certified scalar caller/callee plans. A package
that requires this native-use feature should declare `abi.plan-scalar.v1`;
that requirement must be accepted by the compiler's capability allowlist before
use. Neither a capability declaration nor a rule file enables either flag by
default. The host validates resource paths and capability-name syntax only.

Scalar use requires canonical plan version 2 with `SCALAR_VERIFIED`, the correct
ABI identity and a matching rule-file fingerprint. It covers the implemented
integer, float/double and pointer placements, including register/stack spills
and scalar variadic calls. Missing certification and stale fingerprints are
rejected. General aggregates remain record-only; the separately certified
structure subset below is the only current native aggregate-use extension.
Default compilation retains its existing lowering. The
[IR plan contract](abi-plan-ir-v2.md) records the codec, IR references and
validation. Native test results are bounded by the specific signatures checked;
they do not establish all SysV ABI cases, SD deployment refresh or NuttX
hardware support.

`abi.plan-small-int-struct.v1` retains the original naturally aligned 4/8-byte
integer/pointer structure contract. `abi.plan-small-int-struct.v2` extends that
contract to naturally aligned structures of every size from 1 through 8 bytes,
containing only supported integer/pointer fields. Packages that depend on the
1/2/3/5/6/7-byte extension require v2; v1 remains accepted for the original
subset and is not redefined to promise the new sizes. Canonical plan version 2
flag `SMALL_INTEGER_AGGREGATE_VERIFIED=2` comes from builder verification of the
original type graph; a single INTEGER placement fragment is not proof of
eligibility. Use still requires explicit `-fuse-sysv-abi-plan`. Packed,
floating-field, bitfield, union, array and variadic aggregate shapes remain
excluded. This does not add general <=16-byte or mixed INTEGER/SSE aggregate
support and does not change default lowering.

`IR_AUX_AGGREGATE_EXACT` marks an exact aggregate transfer length of 1..8 bytes.
Its serialized value and consumption are guarded: only the x86_64 plan-use path
may consume it; malformed lengths and cross-engine use are rejected. Consumers
must preserve exact byte counts instead of rounding an irregular structure to
an eight-byte read or write. Plan certification, the exact-transfer marker and
the capability version are separate checks, not substitutes for one another.

The actual root artifact of 906421 bytes passed all seven
[ABI plan codegen tests](../tests/test_abi_plan_codegen.py) with Clang enabled.
The suite includes the original 4/8-byte parameter/return, GPR-spill and pointer
fixtures plus irregular 1/2/3/5/6/7-byte structures, page-guard and canary checks
and GPR spills. Native mixed-toolchain routes include xhcc-built caller/callee,
Clang caller with xhcc callee, and xhcc caller with Clang callee. Packed,
bitfield, floating-field, union, array, oversized and variadic aggregate cases
remain negative coverage. These results validate this <=8-byte subset rather
than complete native SysV aggregate interoperability; they are separate from
deployment or hardware evidence.

Standalone type-classification experiments remain separate;
`typeclass.v1` is currently unsupported in `requires`.

Capability major versions are independent of the v3 manifest format and the
compiler product version. New incompatible semantic/effect or ABI record
meanings require a new capability/schema version; unknown requirements fail
before preprocessing or output. Omitting `requires` only preserves legacy
acceptance and does not negotiate newer behavior. The Python host continues to
validate capability-name syntax only; the compiler owns support decisions.

The format is one `key=value` per line, with no sections, interpolation or nested data. Blank lines and whole-line `#`/`;` comments are ignored. Duplicate and unknown fields fail validation. `id` must match the containing directory. `tools` contains comma-separated unique aliases; empty aliases, whitespace, path separators, `:`, aliases starting with `-`, and duplicates across packages are rejected.

`include`, `options`, `encoding`, `selection`, `templates`, `object`, `relocations`, `cpus`, `target_description` and `aggregate` are POSIX paths relative to the package, confined to its directory after symlink resolution. Absolute/drive paths, backslashes, empty path segments and `.`/`..` segments are rejected. The host reads manifests and validates paths; the shared compiler reads configuration contents and headers as needed.

Optional `cpus=cpus.rules` names an external CPU table (`cpu.descriptor.v1`), allowing package-defined CPU configurations and custom aliases within an already supported ISA. Optional `target_description=target.rules` names a target layout descriptor (`target.descriptor.v1`). The compiler strictly validates the target layout and its compatibility with the selected engine. These descriptors cannot introduce arbitrary ISAs or incompatible data models merely by changing a name. The host validates only the package-relative paths; descriptor syntax, CPU features, layout and engine compatibility belong to the compiler.

Optional `object=object.rules` names an external target-object descriptor for the shared object writer (`object.descriptor.v1`). It describes ELF machine/class, target flags and attributes, and the mapping from compiler-internal relocation kinds to ELF relocations. The Python host only validates its package-relative path; the compiler validates and interprets the descriptor. This externalizes those target-object properties, not instruction encoding or a complete object-format implementation. Descriptor support does not establish that every backend encoding or algorithm is external.

Optional `relocations=relocation.rules` names package metadata for the shared `XHRELOC1` relocation-rule VM (`relocation.vm.v1`). The host validates only its relative path and the syntax of declared capability names; the compiler validates rule contents and VM support. The VM contract provides bounded, atomic patch operations for the implemented relocation rules. This metadata declaration does not establish that all target relocation formulas are implemented or externalized. Standard external ELF relocations remain for the GNU/LLVM toolchain to resolve; the host does not link them or interpret relocation expressions.

Code generation templates are shared across target families through the common template VM (`codegen.templates.v1`), rather than being restricted to the RISC-V rule engine. ARM, x86 and C-output packages can provide their corresponding template files. The shared compiler interprets the selected package's templates; the Python host does not parse template instructions or choose instruction patterns. External templates represent the extracted emission operations and do not establish that every target algorithm is external: remaining instruction-selection decisions, register allocation, ABI handling and other compiled algorithms still require support in the shared compiler.

A copied package can register new aliases without any Wasm file: change its directory/id and assign distinct `tools`. Configurations may select or customize supported compiler targets, CPUs, ABIs, dialects and options. Registering a new name alone does not implement a wholly new instruction set. The `rv32-rule-v1` engine uses external instruction encoding rules and a selection-rule VM for the supported scalar 32-bit RV32 `IR_BINARY` operations ADD, SUB, AND, OR, XOR and MUL, including their register selection. This covers that class of IR selection; it does not establish a complete externally defined ISA selector or general register allocator. Other IR operations, ABI handling, relocations and object support remain compiler capabilities. An ISA absent from those capabilities requires backend implementation changes and rebuilding the shared module as applicable. Configuration packages cannot inject arbitrary new executable compiler code.

## Host command and argument ownership

Host options appear only before the tool:

```text
python tools/xhcc.py --package-root packages --compiler build/wasm/xhcc.wasm --trace-load armcc -c source.c -o source.o
```

`--compiler` defaults to `build/wasm/xhcc.wasm`. `--wasmtime` selects the Wasmtime CLI. `--list-tools` (also `--list`) lists aliases, `--dry-run` prints an execution plan without starting a process, `--json` emits machine-readable plans, and `--trace-load` emits one JSON execution event to stderr for the selected shared module. A dry-run trace uses `event=plan` and `worker_count=0`; actual dispatch uses `event=execute` and `worker_count=1`. Trace records dispatch intent, not measured resident memory or guaranteed runtime instantiation success.

Everything after the tool is opaque package argv, including flags spelled like host options and a literal `--`. The runtime receives the shared module followed by `[TOOL, *original_arguments]`. The host does not recognize `-c`, `-o`, source file positions or command dialect semantics. It never injects a package-specific command-line option. The shared compiler independently discovers/reads package configuration and interprets the command.

The runtime preopens the current working directory and maps the host package root to `/xh-packages`. It receives WASI standard streams and these environment variables:

| Variable | Guest value |
| --- | --- |
| `XHCC_PACKAGE_ROOT` | `/xh-packages` |
| `XHCC_PACKAGE_DIR` | `/xh-packages/<selected id>` |

The host starts exactly one Wasmtime CLI process and propagates its exit status, stdout and stderr. It does not read, compile or launch per-package Wasm files. Missing shared compiler files fail clearly; listing installed aliases does not require a built compiler.

## Legacy manifests and compatibility limits

Version 1 manifests with the original fields remain accepted, including their optional historical `module` field. That field is inert metadata: the host never resolves, checks, opens or executes it. The selected tool always executes the installation's shared compiler. A legacy package still needs target configuration understood by that compiler.

Version 2 packages from the abandoned staged-module design are rejected with an explicit instruction to upgrade to version 3. The host does not silently run their adapters, backends or launcher. Version 3 contains no `module`, command protocol, stage or transport fields. Unknown future manifest versions are rejected. Compatibility is based on supported configuration schema and compiler target capabilities, not an xhcc product version string. Future incompatible schemas require an explicit version and implementation update.

## Embedded runtime contract

The Python program is a desktop reference host. A NuttX integration must supply WASI argv, environment, stdio and filesystem access required by the shared compiler, instantiate its selected shared module once, invoke the command entry, propagate failure and release the instance, linear memory, tables and runtime allocations after completion. Unsupported imports must fail with the module and import namespace/name rather than silent stubs. The desktop implementation delegates import diagnostics to Wasmtime and preserves its stderr and return status.

These requirements do not establish completed NuttX hardware support or measured MCU RAM usage. The current shared-module design includes supported backend implementations in the common compiler; memory budgeting must measure that artifact and its runtime.

## Verification

Run `python -m unittest discover -s tests -p test_host.py -v` from the xhcc directory. Tests verify one shared process, raw argv ownership, preopen/environment paths, copied configuration packages with no Wasm files, inert legacy module metadata, obsolete v2 rejection, aliases/schema/path checks, listing without a compiler and missing-compiler errors. When the version 3 shared compiler and Wasmtime CLI are available, a copied package is exercised through actual compilation to an ELF object.
