# XH Compiler Collection · 曦和编译器

[简体中文](README.zh-CN.md) · English

**Project direction**

> Modular, cross-OS Compiler Collection running on MCUs: multi-backend cross-builds to flashable firmware, drop-in gcc replacement for make/CC, RTOS-harness callable, backends added by copying a directory.

XH Compiler Collection (`xhcc`) currently runs as WASI Preview1 commands, with desktop SD-deployment simulation and bounded make/GCC-compatible interfaces. External packages load configuration, code-generation rules, headers and runtime resources on demand; copying a directory can extend an existing engine, while an arbitrary new ISA can require engine changes. MCU execution and RTOS-harness integration remain deployment goals: NuttX/WAMR hardware memory use is not yet measured. Modern general linking is incomplete; legacy image-generation interfaces are distinct from a complete compile-to-flash firmware workflow.

Two independent applications share one `packages/` directory:

- `xhcc.wasm`: direct compiler driver, without the make implementation.
- `xhccm.wasm`: the same driver plus an in-process, bounded make subset. Compiler recipes run in the same process without a shell or subprocess.

Each application contains the shared C frontend, IR and target engines. There is no separate Wasm module per backend. The default 4 MiB limit applies to each application's **Wasm linear memory**; complete MCU RAM use still requires measurement on the target runtime.

## Build and quick start

Run commands from the repository root unless stated otherwise. Install Python 3, the WASI SDK, Binaryen and WABT; use Wasmtime for desktop execution. Put the tools on `PATH` or configure their locations:

```sh
# Set WASI_SDK_PATH to your WASI SDK directory first.
# Set WASM_OPT and WASM_VALIDATE if those tools are not on PATH.
python tools/build.py --app both --target all
python tools/xhcc.py --list-tools
python tools/xhcc.py arm-none-eabi-gcc -c examples/add.c -o build/add.o
python tools/xhcc.py armcc --cpu Cortex-M4 -S examples/add.c -o build/add.s
python tools/xhcc.py riscv32-unknown-elf-gcc -c examples/add.c -o build/add-rv.o
python tools/xhcc.py x86_64-linux-gnu-gcc -c examples/add.c -o build/add-x64.o
```

The [build script](tools/build.py) defaults to `--app both --target all`. Production outputs are `build/wasm/xhcc.wasm` and `build/wasm/xhccm.wasm`; intermediate builds and maps are under `build/wasm/dev/all/` and its `xhccm/` subdirectory. `--app xhcc` or `--app xhccm` builds one application. Target-specific development builds use `build/wasm/dev/<target>/` and do not replace the production multi-target modules.

With GNU make installed, the root [Makefile](Makefile) also defaults to both applications: `make all`, `make xhcc` and `make xhccm`. Override `PYTHON` and `WASI_SDK_PATH` as needed. The build script accepts `--sdk`, `--wasm-opt` and `--wasm-validate`; it also reads `WASI_SDK_PATH`, `WASM_OPT` and `WASM_VALIDATE`. Its fallback SDK and Windows tool paths are local development defaults, so configure them for your machine.

The Python [desktop adapter](tools/xhcc.py) selects a package and runs the compiler; Python is not required by the Wasm compiler itself. Set `WASMTIME` or pass `--wasmtime` when Wasmtime is not on `PATH`. Direct execution is also supported:

```sh
wasmtime run --dir=. build/wasm/xhcc.wasm arm-none-eabi-gcc -c examples/add.c -o build/add.o --memory-report
```

## SD deployment and make

Build both applications, then deploy:

```sh
python tools/deploy_sd.py
python tests/test_sd.py
```

The [deployment script](tools/deploy_sd.py) defaults to `../sd/xhcc/`; `--sd <directory>` overrides it. It installs sibling `xhcc.wasm` and `xhccm.wasm`, one shared `packages/` tree, generated make examples and an `installation.json` manifest. Run the following **from that deployed directory**, with the directory exposed to the guest as `/`:

```sh
wasmtime run --dir=.::/ --env XHCC_PACKAGE_ROOT=/packages xhcc.wasm arm-none-eabi-gcc -c examples/make-demo/first.c -o examples/make-demo/first.o
wasmtime run --dir=.::/ --env XHCC_PACKAGE_ROOT=/packages xhccm.wasm make -C examples/make-demo all
wasmtime run --dir=.::/ --env XHCC_PACKAGE_ROOT=/packages xhccm.wasm make -C examples/make-demo -B CC=riscv32-unknown-elf-gcc CFLAGS=-O0 all
```

Keep `XHCC_PACKAGE_ROOT` absolute in the guest when using make's `-C`. An optional leading tool in `xhccm make [TOOL] ...` supplies the default `CC`; command-line `CC=...` overrides ordinary makefile assignments; explicit `override` assignments take precedence. A compiler name written explicitly in a recipe selects that named tool. The [SD test](tests/test_sd.py) checks direct compilation, make, multiple sources, incremental rebuilds and switching `CC` through a desktop WASI preopen. This is a deployment simulation, not a NuttX/WAMR hardware validation.

The make subset supports variable assignments, `override`/`undefine` directives and substitution references, includes, bounded conditionals, twenty-four text/control/diagnostic functions (including lazy `if`, scoped `foreach`, `value`, `flavor`, `findstring`, `join`, `wildcard`, `origin`, `error` and `warning`), explicit/pattern/suffix rules, order-only dependencies, automatic variables, dependency files, incremental/forced builds and a few literal file commands. Explicit-target and pattern-specific variables inherit into prerequisites and restore bindings on scope exit. Matching patterns apply from less to more specific (longer to shorter stems), then explicit-target values apply last; command-line values retain priority unless `override` is used. Ordinary multiple explicit targets may share recipe text; each target independently checks its mtime and uses its own `$@` and local variables. Grouped `&:` targets, multi-target variable declarations and multiple-pattern recipes remain unsupported. `-j1` selects serial execution; other job counts fail. There is no shell, recursive make, environment import/export, general linking or full GNU make compatibility. See the [exact syntax, commands and limits](docs/make-subset.md).

## Targets and language scope

Machine-target `-c` emits standard ELF relocatable objects (`ET_REL`). The C backend emits C source for another C compiler.

| Target | Current scope | Main boundary |
| --- | --- | --- |
| ARM | Cortex-M3/M4/M7/M33, Thumb-2, base AAPCS, checked soft/softfp | No hard-float parameter ABI; M0/M0+/M1/M23 need Thumb-1 lowering |
| RV32 | RV32IM, ILP32, software floating-point helper calls | No RV64 or hardware floating-point ABI |
| i386 | ILP32, current scalar SysV subset and xhcc aggregate wire | No complete GNU aggregate ABI guarantee |
| AMD64 | LP64, scalar SysV/SSE and tested variadic cases; opt-in limited small structures and exact 16-byte `long`/`double` pair shapes | Complete native SysV aggregate interoperability is not implemented |
| C output | IR translated to C text | Not a wasm32 code-generation backend |

See [target definitions and architecture limits](docs/targets.md). The compiler runs in Wasm; compiling input C directly into wasm32/WASI is a separate, currently unimplemented backend.

The frontend derives from arkcc's C99 implementation, with ARMCC extensions and a limited C11 keyword subset: `_Alignas`, `_Alignof`, `_Static_assert`, `_Generic`, `_Thread_local` and `_Atomic`. This does not establish full C11 support. Native TLS is rejected; atomics have target/runtime limits. Basic asm and ARMCC asm functions depend on the target. GNU extended asm currently accepts one 32-bit `=r` output, up to two `r/i` inputs and `memory/cc` clobbers; broader constraints are rejected. Details are in the [feature comparison](docs/feature-parity.md) and [object library documentation](object/README.md).

### Experimental AMD64 ABI plans

`-frecord-sysv-abi` records versioned argument/result plans; `-c --private-object --obj-ir` preserves them for inspection. `-fuse-sysv-abi-plan` additionally enables the **experimental caller/callee consumer** for verified scalars and limited naturally aligned structures as named parameters and return values: 1–8-byte integer/pointer structures (including recursively eligible nested structures), single-SSE-fragment pure-FP structures `{float}`, `{float, float}` and `{double}`; and exact naturally aligned 16-byte LP64 pairs `{long, long}`, `{long, double}`, `{double, long}` and `{double, double}` (`long` must be 64-bit `TYPE_LONG`); and exact naturally aligned 8-byte `{int, float}` / `{float, int}` shapes with 32-bit fields at offsets 0 and 4, merged into one INTEGER slot. Both flags remain off by default. Use mode checks one version 2 source-type certificate (`SCALAR_VERIFIED=1`, `SMALL_INTEGER_AGGREGATE_VERIFIED=2`, `SMALL_SSE_AGGREGATE_VERIFIED=4`, `INTEGER_PAIR_AGGREGATE_VERIFIED=8`, `MIXED_PAIR_AGGREGATE_VERIFIED=16` or `SINGLE_SLOT_MIXED_AGGREGATE_VERIFIED=32`), ABI identity and the rule fingerprint. A signature uses its highest required certificate, not a bitwise OR; fragment layout alone does not establish eligibility. These extensions use `abi.plan-int-pair.v1`, `abi.plan-mixed-pair.v1` and `abi.plan-small-mixed.v1`.

Execution tests cover scalar integers, floating-point values, pointers, register/stack spills and scalar variadic cases, plus the limited structures' parameters/results, GP-register exhaustion and pointer fields. Exact-size IR loads/stores preserve the structure's byte extent; page-boundary and canary tests cover 1/2/3/5/6/7-byte cases, including stack spills. Pure-FP structure tests cover parameters/results, spills after all eight SSE argument registers are used, GP/SSE mixtures and canaries. A signature may combine eligible integer and SSE structures. The certified `{int, float}` / `{float, int}` cases preserve negative-zero and qNaN payload bits in identity tests and cover GPR boundaries and canaries; other uncertified small mixed-field shapes remain rejected. The supported 16-byte pairs use two GP/SSE fragments; exhaustion of either required bank rolls the whole argument back to the stack, leaving unused registers available to later arguments. Return-register ordinals are independent per bank (RAX/RDX and XMM0/XMM1), while internal result buffers remain distinct. Tests cover spills, tail registers, multiple live result buffers and canaries. Coverage includes **xhcc caller → Clang callee and Clang caller → xhcc callee**. Packed structures, bitfields, unions, arrays, other structures larger than 8 bytes (including other 16-byte shapes), variadic aggregate arguments and other unsupported aggregate shapes are rejected by use mode. Other aggregate plans remain record-only; this is not complete native SysV aggregate interoperability. IR container v2 persists the plan table and independent references; the reader accepts v1 with no plans. See the [ABI plan contract and test evidence](docs/abi-plan-ir-v2.md).

## Command-line compatibility

The modern driver provides `-E/-S/-c`, syntax-only checks, C language/standard selection, O0/O1/O2, target/CPU/ISA/ABI options, include paths and macros, dependency generation, diagnostics, bounded response files and sequential multi-source compilation. Unknown or unimplemented options fail explicitly.

Tool aliases such as `arm-none-eabi-gcc`, `x86_64-linux-gnu-gcc` and `armcc` select argument interfaces; they do not imply a complete GNU GCC or commercial ARM compiler implementation. General linking, DWARF ELF output and the full GCC option set are not supported. `--ark-compat` retains legacy private-object, IR/LTO, IPO, partial-link, scatter and MDK-import interfaces; these are not GNU linker or GCC LTO compatibility claims.

```sh
wasmtime run --dir=. build/wasm/xhcc.wasm --ark-compat --help
```

## External packages and backend development

[Package manifests](docs/package-protocol.md) register aliases, targets, CPUs, ABI contracts, headers, option mappings and rule resources. Existing engines consume external templates, semantic selection, encodings, CPU/target layouts, ABI data, object descriptors and bounded relocation rules. Rule variants can change emitted instructions and executed behavior while the compiler module's SHA-256 remains unchanged.

All five engines have real external-rule consumers. Some complex selection, ABI lowering, optimization and layout algorithms remain C code in the application. A new package can configure a supported engine; an arbitrary new ISA may require shared-contract and engine changes plus a rebuild. See [externalization status](docs/backend-externalization.md) and the repository's [backend-porting guide](docs/skills/port-backend/SKILL.md).

## Memory and verification

Default builds declare one non-shared wasm32 linear memory with initial = maximum = **4 MiB**, covering static data, the C stack, arena and libc heap. Runtime engine structures, bytecode, host stacks, WASI buffers and filesystem caches add to complete task RAM. Neither that cap nor Wasmtime process RSS proves a 4 MiB MCU deployment. NuttX/WAMR configuration and hardware peak-memory measurements remain necessary.

The default arena is 1 MiB. Source/preprocessed/assembly/machine-code buffers and include depth have explicit bounds; larger inputs can fail with a limit diagnostic. External resources load on demand. Build-time Binaryen lowering and WABT validation restrict Wasm features; see [resource measurement](docs/resource-budget.md) for storage, maps and RAM distinctions.

```sh
python tools/test.py
python tests/test_sd.py
python tools/resource_report.py --self-test --json build/reports/resources.json --markdown build/reports/resources.md
```

Build and deploy before running SD tests. Integration tests use GNU ARM tools, Unicorn, pyelftools and, for relevant mixed-toolchain checks, a suitable Clang/LLVM installation. Reference-toolchain checks can skip when unavailable; a skipped test is not interoperability evidence. The regression entry includes real compilation, ELF checks, execution, rule variants, negative cases and scalar ABI plans. Resource reports describe the artifacts measured when generated, not fixed project specifications.

## License and implementation sources

The initial C frontend, IR and ARM emitter derive from MIT-licensed arkcc. See [source attribution](NOTICE.md) and the retained [MIT license](LICENSE). Target extensions are independently implemented from public ISA, ABI and object-format specifications; compiler compatibility is validated against external tools, without copying GPL compiler implementations into the project.
