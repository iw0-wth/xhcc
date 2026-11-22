# xhcc configuration package protocol, version 3

The desktop host runs one shared `xhcc.wasm` containing the compiler frontend and supported backend implementations. External packages contain target configuration, command dialect/options and headers. They do not provide executable backend Wasm modules. The host chooses an installed tool alias, passes its raw arguments to the shared compiler, and waits for that one process to exit.

This architecture deliberately keeps supported backend code in the shared module. Selecting a configuration does not unload the other compiled backend implementations or prove that their code/tables consume no RAM. There is no launcher callback, staged worker lifecycle, per-package module loading, CompileRequest queue or external IR transport in the production version 3 host.

## Flat manifest

Each package is an immediate child directory of the package root and contains UTF-8 `package.ini`:

```ini
version=3
id=arm-cortex-m3
tools=arm-none-eabi-gcc,armcc
target=arm
cpu=cortex-m3
abi=aapcs-soft
include=include
dialect=gcc
options=options.ini
```

The required fields are `version`, `id`, `tools`, `target`, `cpu`, `abi`, and `include`. Optional `dialect` is `gcc` or `armcc`; optional `options` names the package's option mapping file. Optional `engine` selects an engine understood by the shared compiler, `encoding` names its external encoding-rule file, and `selection` names its external IR selection-rule file. For example, a RISC-V package can use `engine=rv32-rule-v1`, `encoding=encoding.rules` and `selection=selection.rules`. The compiler owns interpretation of engines, rules and option mappings. The Python host accepts the engine identifier without interpreting its contents and validates the rule files' package-relative paths; it does not parse encoding/selection rules, compiler option definitions or compiler argument positions.

Optional `requires` declares compiler capabilities, for example `requires=encoding.fields.v1,selection.scalar32.v2`. It is a comma-separated list of unique nonempty ASCII names matching `[a-z][a-z0-9]*(?:[._-][a-z0-9]+)*`; whitespace within the list, empty entries, duplicates and malformed names are rejected. Omitting the field preserves legacy behavior; an explicitly empty field is invalid. The host checks syntax only and accepts unknown syntactically valid capabilities. The shared compiler decides whether the selected package's requirements are supported and diagnoses unsupported requirements. This avoids binding package acceptance to the Python host's release or a host-maintained capability allowlist.

Current compiler capability names are `package.options.v1`, `encoding.fields.v1`, `selection.scalar32.v1`, `selection.scalar32.v2`, `object.elf32` and `object.elf64`. Selection versions declare the corresponding rule VM contract, not universal IR/ABI support. A newer package can require a newer capability explicitly; a package without `requires` receives no new compatibility guarantee from omission. Requirements are capability contracts independent of product version numbers.

The format is one `key=value` per line, with no sections, interpolation or nested data. Blank lines and whole-line `#`/`;` comments are ignored. Duplicate and unknown fields fail validation. `id` must match the containing directory. `tools` contains comma-separated unique aliases; empty aliases, whitespace, path separators, `:`, aliases starting with `-`, and duplicates across packages are rejected.

`include`, `options`, `encoding` and `selection` are POSIX paths relative to the package, confined to its directory after symlink resolution. Absolute/drive paths, backslashes, empty path segments and `.`/`..` segments are rejected. The host reads manifests and validates paths; the shared compiler reads configuration contents and headers as needed.

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
