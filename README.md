# XH Compiler Collection · 曦和编译器

[简体中文](README.zh-CN.md) · English

**Project direction**

> Modular, cross-OS Compiler Collection running on MCUs: multi-backend cross-builds to flashable firmware, drop-in gcc replacement for make/CC, RTOS-harness callable, backends added by copying a directory.

XH Compiler Collection (`xhcc`) currently runs as WASI Preview1 commands with external target packages, a bounded make/GCC-compatible interface and a shared ARM/RV32 standard-ELF linker. Desktop execution and simulated SD deployment are tested. MCU deployment and RTOS-harness integration remain goals; complete NuttX/WAMR memory use has not been measured. Copying a package directory extends a supported engine; an arbitrary new ISA can require engine development.

## Four applications

| Application | Contents and purpose |
| --- | --- |
| `xhcc.wasm` | Multi-target C compiler with bounded standard ARM/RV32 ELF linking and legacy compatibility interfaces |
| `xhccm.wasm` | Compiler/linker plus an in-process make subset; compiler recipes call the driver without a shell or subprocess |
| `xhlinker.wasm` | Independent standard ARM Thumb/RV32 ELF32 linker, without the C frontend, IR generation, code-generation backends or make |
| `xhccn.wasm` | Multi-target compiler with linking compiled out; rejects link requests |

The compiler applications share the frontend, IR and target-engine sources. All four are sibling commands deployed beside one `packages/` tree, with no per-backend child Wasm. Each default build has a **4 MiB Wasm linear-memory cap**, not a proven 4 MiB total MCU RAM footprint.

## Build and quick start

Run commands from the repository root unless stated otherwise. The host build needs the WASI SDK, Binaryen and WABT; desktop execution uses Wasmtime. Configure tool locations for your machine rather than relying on the scripts' local Windows fallback paths.

The Python host path remains supported:

```sh
# Set WASI_SDK_PATH to your SDK directory.
# Set WASM_OPT and WASM_VALIDATE if those tools are not on PATH.
python tools/build.py --app all-apps --target all
python tools/xhcc.py --list-tools
python tools/xhcc.py arm-none-eabi-gcc -c examples/add.c -o build/add.o
python tools/xhcc.py armcc --cpu Cortex-M4 -S examples/add.c -o build/add.s
python tools/xhcc.py riscv32-unknown-elf-gcc -c examples/add.c -o build/add-rv.o
python tools/xhcc.py x86_64-linux-gnu-gcc -c examples/add.c -o build/add-x64.o
```

The [build script](tools/build.py) defaults to `--app all-apps --target all`. Individual selections are `xhcc`, `xhccm`, `xhccn` and `xhlinker`; `--app both` retains the older xhcc+xhccm selection. Production outputs are `build/wasm/<app>.wasm`. Python development outputs/maps are under `build/wasm/dev/all/`, with non-xhcc applications in their own app subdirectories. Target-specific development builds do not replace the production multi-target modules.

The root [Makefile](Makefile) defaults to all four applications: `make all`, or select `make xhcc`, `make xhccm`, `make xhccn`, `make xhlinker`. Override `PYTHON` and `WASI_SDK_PATH` as needed. The script accepts `--sdk`, `--wasm-opt`, `--wasm-validate` and corresponding `WASI_SDK_PATH`, `WASM_OPT`, `WASM_VALIDATE` environment settings.

A tested build path invokes no Python executable:

```sh
make -f Makefile.nopy all
```

[Makefile.nopy](Makefile.nopy) uses GNU make, the WASI SDK, Binaryen, WABT and POSIX shell tools directly. On Windows use MSYS2 GNU make/Bash and configure its tool paths; `DLL_DIR` optionally supplies SDK runtime DLLs. Outputs/maps are under `build/nopy/<app>/`, and validated modules are copied to `build/wasm/`. See [builds with and without Python](docs/no-python-build.md). This is a host build path, not xhcc self-hosting.

The Python [desktop adapter](tools/xhcc.py) discovers packages and runs the compiler; Python is not required by the Wasm commands themselves. Set `WASMTIME` or pass `--wasmtime` when needed. Direct execution also works:

```sh
wasmtime run --dir=. build/wasm/xhcc.wasm arm-none-eabi-gcc -c examples/add.c -o build/add.o --memory-report
```

## ARM/RV32 ELF linking and image output

Compile first, then link explicit objects. This minimal example sets `add` as the ELF entry to demonstrate linking; it is not a board startup program:

```sh
python tools/xhcc.py arm-none-eabi-gcc -c examples/add.c -o build/add.o
python tools/xhcc.py arm-none-eabi-gcc build/add.o --entry add --text-address 0x08004000 --data-address 0x20002000 -o build/add.elf
wasmtime run --dir=. build/wasm/xhlinker.wasm --entry add --text-address 0x08004000 --data-address 0x20002000 --oformat=binary build/add.o -o build/add.bin
```

`xhcc`, `xhccm` and `xhlinker` share one bounded linker core for little-endian ARM Thumb and RV32 ELF32 relocatable objects, producing executable ELF or flat binary. For RV32, select `riscv32-unknown-elf-gcc` in the compiler driver or `--target=riscv32` in `xhlinker`; the supported profile is RV32IM/ILP32 soft-float with ELF flags zero, without RVC. Its RELA handling includes PC-relative anchor pairs and validated RELAX markers without performing relaxation. `-T script.ld` accepts a checked MEMORY/SECTIONS subset with Flash/RAM layout, data VMA/LMA, BSS/COMMON and selected startup-symbol assignments. Flat output follows load addresses rather than filling the Flash-to-SRAM address gap. GNU ld comparisons cover two layouts and actual execution, local/weak/strong/COMMON symbols, relocations and image bytes.

Automatic `.c` compile-and-link, mixing `.c` and `.o`, archives/`-l`, i386/AMD64 or other unsupported-target standard linking and full GNU linker-script compatibility remain unsupported. Startup code, vector tables, data copy/BSS clearing and runtime libraries must be supplied by the project. Producing a binary does not establish that it boots on an arbitrary board. Legacy private ARKOBJ linking, IR/LTO and partial image-output paths remain separate interfaces under `--ark-compat`; they are not standard ELF or GCC LTO compatibility claims. RV32 objects preserve RX text, writable data, NOBITS BSS and section-specific RELA; mixed xhcc/Clang objects execute in two tested layouts. See the [linker contract](docs/elf-linker-first-stage.md), [RV32 relocations](docs/rv-elf-relocations.md), [RV32 object sections](docs/rv-elf-sections.md) and [progress and roadmap](docs/progress-and-roadmap.md).

File output uses a same-directory temporary file and replaces the destination after successful writes on supporting filesystems. This does not promise fsync/crash durability; stdout is not atomic.

## SD deployment and make

Build all four applications, then deploy:

```sh
python tools/deploy_sd.py
python tests/test_sd.py
```

The [deployment script](tools/deploy_sd.py) defaults to `../sd/xhcc/`; `--sd <directory>` overrides it. It installs four sibling applications, shared packages, generated make examples including `memory.ld` and `Makefile.link`, and an `installation.json` manifest. Run these commands **from the deployed directory**, mapped to guest `/`:

```sh
wasmtime run --dir=.::/ --env XHCC_PACKAGE_ROOT=/packages xhccm.wasm make -C examples/make-demo all
wasmtime run --dir=.::/ --env XHCC_PACKAGE_ROOT=/packages xhccm.wasm make -C examples/make-demo -f Makefile.link all
wasmtime run --dir=.::/ --env XHCC_PACKAGE_ROOT=/packages xhlinker.wasm -T examples/make-demo/memory.ld --oformat=binary examples/make-demo/first.o examples/make-demo/second.o examples/make-demo/entry.o -o examples/make-demo/firmware.bin
```

Keep `XHCC_PACKAGE_ROOT` absolute in the guest when using make's `-C`. `xhccm make [TOOL] ...` supplies a default `CC`; command-line `CC=...` overrides ordinary assignments, while explicit `override` assignments take precedence. A compiler explicitly named in a recipe selects that tool. `LD=xhlinker` also dispatches in-process through the same shared linker CLI; ARM and RV32 two-source compile-to-object/link recipes have dedicated execution tests. Pass `--target=riscv32` for RV32 and keep the linker package root absolute after `-C`.

The make subset supports 24 text/control/diagnostic functions, variables and substitution references, `override`/`undefine`, includes, bounded conditionals, explicit/pattern/suffix rules, order-only dependencies, dependency files and incremental/forced builds. Automatic variables include `$?`, `$|` and `$+`; `$+` retains repeated normal prerequisites in GNU declaration order, with the recipe declaration first. Target and pattern variables inherit into prerequisites and restore on scope exit; patterns apply from less to more specific, then explicit-target values. Multiple explicit targets may share a recipe but build independently with their own mtime, `$@` and variables.

`-j1` is serial; other job counts fail. Grouped `&:`, multi-target variable declarations, multiple-pattern recipes, environment import/export, recursive make and shell scripts are unsupported. Supported compiler/linker recipes run in-process; literal file commands are bounded. `.DELETE_ON_ERROR:` explicitly enables removal of new/mtime-changed regular targets after non-ignored failures; unchanged files, directories and `.PRECIOUS` matches are preserved. This uses mtime snapshots, not content hashes. See the [exact make subset](docs/make-subset.md). SD tests are desktop deployment simulations, not hardware validation.

## Targets and language scope

Machine-target `-c` emits standard ELF `ET_REL`; C output emits source for another compiler.

| Target | Current scope | Main boundary |
| --- | --- | --- |
| ARM | Cortex-M3/M4/M7/M33 Thumb-2; initial M0/M0+/M1 Thumb-1 local-scalar subset | No hard-float parameter ABI; Thumb-1 limits below; M23 unsupported |
| RV32 | RV32IM, ILP32, software FP helpers | No RV64 or hardware FP ABI |
| i386 | ILP32, scalar SysV subset and xhcc aggregate wire | No complete GNU aggregate ABI guarantee |
| AMD64 | LP64, scalar SysV/SSE, tested varargs and opt-in limited aggregates | Not complete native SysV aggregate interoperability |
| C | IR translated to C text | Not a wasm32 output backend |

The initial Thumb-1 path uses low registers, byte-built constants, local scalar operations, short control flow and direct calls. Production `-S` has GNU Cortex-M0 assembler/CPU execution checks; `-c` emits Thumb-1 objects with ARMv6-M attributes. Signed/unsigned integer division and remainder lower to AEABI helper calls, tested with GNU Cortex-M0 assembly, an external ARMv6-M libgcc and CPU execution. Provide the helper runtime explicitly; built-in archive extraction is not supported. Raw SDIV/UDIV instructions remain invalid on M0; globals, strings, floating point and long branches outside this subset fail explicitly. This is not complete Thumb-1 support or a claim that every ARM architecture has an integrated firmware workflow. Thumb-2 retains base AAPCS and checked soft/softfp support.

The frontend derives from arkcc's C99 implementation with ARMCC extensions and a limited C11 keyword subset: `_Alignas`, `_Alignof`, `_Static_assert`, `_Generic`, `_Thread_local`, `_Atomic`. Native TLS is rejected and atomics have target/runtime limits. Basic/ARMCC asm depends on the target. GNU extended asm supports one 32-bit `=r` output, up to two `r/i` inputs and `memory/cc` clobbers. See [targets](docs/targets.md), [feature limits](docs/feature-parity.md) and [object documentation](object/README.md).

Tool aliases and GCC/armcc-style options provide bounded compatibility, not complete GNU GCC or commercial ARM compiler implementations. The modern driver supports preprocessing, assembly/object output, diagnostics, dependencies, bounded response files and sequential multi-source compilation. Unknown/unimplemented options fail; full GCC options and DWARF ELF output are not provided.

### Experimental AMD64 ABI plans

`-frecord-sysv-abi` records argument/result metadata; `-fuse-sysv-abi-plan` records and consumes verified plans. Both remain off by default. Supported named parameters/results include scalars, natural 1–8-byte integer/pointer structures, selected pure-FP structures (`float`, two floats, double), exact LP64 16-byte LL/LD/DL/DD pairs, and natural 8-byte `{int,float}` / `{float,int}`. Packed, bitfield, union, array, variadic aggregate and other uncertified shapes are rejected by use mode; other supported recorded plans remain inspection-only.

Tests cover Clang interoperability in both directions, register-bank exhaustion with whole-object stack rollback, exact-size boundary/canary checks, result buffers and private-IR save/reload through legacy LTO. Versioned source-type eligibility, ABI identity, rule fingerprints and retained function signatures are checked; equivalent plans may have different indices. Call-site actual types are not fully retained, so this is not a security guarantee for arbitrary IR. IR v2 persists plans; v1 reads with no plans. See the [ABI contract and execution evidence](docs/abi-plan-ir-v2.md).

## External backends, memory and self-hosting

[Packages](docs/package-protocol.md) register tool aliases, CPUs, ABI contracts, headers, options and rules. All five engines consume external rules; same-module-hash variants can change real generated/executed behavior. Third parties can develop and distribute independent backend packages: for an existing engine, copy a package and adapt supported templates, instruction encodings, ABI descriptions and relocation rules. A new ISA requires engine support; arbitrary targets cannot be implemented through INI configuration alone. Complex lowering, optimization and layout policies still partly live in C. See [externalization status](docs/backend-externalization.md) and the [backend-porting guide](docs/skills/port-backend/SKILL.md).

Each default module declares one non-shared wasm32 linear memory with initial = maximum = 4 MiB, including static data, C stack, arena and libc heap. Runtime structures, bytecode, host stacks, WASI and filesystem buffers add to total task RAM. Neither this cap nor desktop RSS proves MCU feasibility. See [resource measurement](docs/resource-budget.md).

There is currently **no wasm32 output backend and no completed Wasm self-hosting**. Host Clang builds these Wasm applications; frontend/sysroot/runtime and memory work remain. See the [Wasm backend and bootstrap plan](docs/wasm-bootstrap-plan.md).

The default XHCC file path now streams source/includes through a bounded preprocessor sink to a host/SD filesystem spool and a window-backed lexer. The four-application compatibility regression has passed with this path enabled; targeted tests also compile >1MiB main files/includes with executable ARM/RV32 tails and verify >1MiB preprocessing output. Writable temporary storage is required; token, macro and asm-span limits remain, and AST/IR memory still grows with program complexity. Large-TU support is a prerequisite for the highest-priority goal: building MicroPython through its original Makefiles. That complete repository workflow is not yet supported and is under audit; host-side Python generation is allowed where the project requires it. See the [streaming frontend and original-Makefile acceptance plan](docs/streaming-frontend-plan.md).

## Verification and license

```sh
python tools/test.py
python tests/test_four_apps.py
python tests/test_sd.py
python tools/resource_report.py --self-test --json build/reports/resources.json --markdown build/reports/resources.md
```

Build before execution tests and deploy before SD tests. GNU ARM tools, Unicorn, pyelftools and suitable Clang/LLVM tools are needed for relevant checks. Unavailable reference checks can skip; skips do not prove interoperability. Reports describe measured artifact snapshots, not fixed size specifications.

Original contributions added by xhcc are licensed under the [MIT license](LICENSE), `Copyright (c) 2026 Taohan Wang <tianqi2021_001@163.com>`. The initial frontend, IR and ARM emitter derive from MIT-licensed arkcc; its original copyright and complete permission notice are preserved in [LICENSES/arkcc/LICENSE](LICENSES/arkcc/LICENSE) and must accompany distributions of that software or substantial portions of it. See [attribution and license scope](NOTICE.md) and the [license directory](LICENSES/README.md). Independent third-party backend packages may use their own licenses. Merely reading a package's data or rules does not by itself change the core's MIT license; obligations for a particular combination depend on code copying, actual linking and distribution relationships and the applicable licenses. Target extensions are independently implemented from public ISA/ABI/object specifications; external compilers serve as validation tools, without copying GPL compiler implementations into the project.
