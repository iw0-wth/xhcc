# Four-application build with or without Python

Both host build paths are supported. Python is allowed on the host; it is not a
required executable in the direct GNU Make build path. Neither path establishes
that xhcc can yet compile and bootstrap its own full source.

## Python host path

```
python tools/build.py --app all-apps
```

`--app xhcc`, `xhccm`, `xhccn` and `xhlinker` build individually. `--app both`
retains the older xhcc+xhccm convenience selection. The Python path supports the
content-addressed object cache described separately in `build-object-cache.md`.
The top-level Makefile's `all` target currently uses this host path.

## Direct GNU Make path

```
make -f Makefile.nopy all
```

Individual targets are `xhcc`, `xhccm`, `xhccn` and `xhlinker`. The Makefile
finds C sources with GNU Make built-ins and directly invokes Clang, wasm-opt,
wasm-validate, mkdir, mv and cp. It does not invoke Python or require a Python
generator for source lists, flags or recipes. GNU Make and POSIX shell tools are
host prerequisites; they are not part of the MCU application runtime.

On the tested Windows host:

```
D:/msys64/usr/bin/make.exe -f Makefile.nopy all DLL_DIR=D:/miniconda3/Library/bin
```

That DLL_DIR was an already installed directory supplying runtime DLLs needed
by the local WASI SDK. It is not a Python invocation or a requirement to install
Python/Conda. Other machines can use an appropriate VC runtime/DLL directory,
or leave DLL_DIR empty when tool dependencies are already available in PATH.

Overridable variables:

| Variable | Purpose |
| --- | --- |
| WASI_SDK_PATH | WASI SDK root |
| WASI_CC | Clang executable |
| WASM_OPT | Binaryen executable |
| WASM_VALIDATE | WABT validation executable |
| MEMORY_BYTES | Linear memory budget; default 4194304 |
| WINDOWS_BASH | Windows POSIX shell, default local MSYS2 Bash path |
| HOST_TOOLS_DIR | Directory providing mkdir/cp/mv on Windows |
| DLL_DIR | Optional extra host DLL search directory |

Outputs and maps are under `build/nopy/<app>/<app>.wasm` and
`build/nopy/<app>/<app>.map`; validated applications are copied to
`build/wasm/<app>.wasm`. Python builds retain their development maps under
`build/wasm/dev/all/`, with non-xhcc applications in corresponding app subdirs.
Both paths retain WASI Preview 1 / lowered feature validation and the explicit
4 MiB maximum linear memory budget.

## Direct Make incremental dependency contract

The direct path records compile and link profiles with GNU Make's own `file`
functions. A profile is rewritten only when its complete configured text changes;
an unchanged second invocation preserves stamp timestamps and performs no build
actions. No Python or external generator creates these profiles.

Object prerequisites include project `.h`/`.inc`/`.def` files, WASI sysroot and
Clang resource headers of those formats, the compiler executable, and the
compile profile. That profile includes the SDK/compiler paths, ordered CFLAGS,
per-application macro contract and discovered header list. Adding/removing a
header or changing compilation flags invalidates the objects. Link outputs
additionally depend on the link profile, compiler, wasm-opt and wasm-validate
files; changed link/lowering/validation flags or tool timestamps rerun the final
link/lowering/validation boundary.

These are classic GNU Make timestamp dependencies, not the Python cache's
content hashes. Changing a dependency's contents while restoring its original
mtime is outside this incremental contract; use a fresh OUT directory for a
clean rebuild when needed. Header changes conservatively recompile all affected
shared objects rather than attempting compiler-specific include dependency
pruning. Ordinary command-line flags are supported; embedded-newline flag values
are not a portable Make interface.

A tiny actual GNU Make fixture checks compile/link call counts, unchanged-stamp
zero actions, SDK `.def` changes, CFLAGS changes, compiler timestamp changes,
link-only flag changes and optimizer timestamp changes. Separate real SDK
object tests retain the Python cache invalidation and byte-identity gates.
This verifies dependency decisions and does not substitute for the four-application
runtime tests after an actual direct build.

## Application separation

- xhcc: compiler and bounded standard/compatibility linking.
- xhccm: compiler/linking plus the Make entry.
- xhccn: compiler without linking support.
- xhlinker: independent bounded ARM Thumb ELF linker command, without frontend,
  IR generation, code-generation backends or Make entry.

Shared compiler objects are reused by GNU Make. Only the xhccm driver object
receives `XH_DRIVER_NO_MAIN=1`. For xhccn, `XH_WITH_LINKER=0` is applied precisely
to the three consuming translation units: driver, core/compiler and ir/ipo.
Its link source files are omitted. The standalone linker selects only its entry,
ELF reader/linker, script adapter, ARM ELF relocation wrapper, relocation VM and
host filesystem implementation. Link maps and runtime rejection tests verify
these boundaries; a filename alone does not establish isolation.

## Executed verification snapshot

The direct Makefile was executed for all four applications, including a final
incremental rebuild after linker fixes. All four linked, lowered and validated
successfully with no Python executable in the build command chain. The separate
Python verification script `tests/test_four_apps.py` then passed: compiler
object generation, script-driven ELF/bin links through all three linking entries,
identical linked output, and rejection of linking by xhccn. Using Python for this
verification does not introduce Python into the build recipes.

That snapshot produced sizes 943305 / 973590 / 886302 / 288567 bytes for
xhcc / xhccm / xhccn / xhlinker, all with a 4194304-byte declared memory maximum.
These sizes are snapshot evidence, not fixed future specifications. Different
object ordering/output paths between build paths may change Wasm SHA while
preserving tested behavior; reproducible binary identity is not claimed here.

A later dependency-aware direct-build run rebuilt all four applications after
neutral IR consumer changes using the same no-Python GNU Make command above.
All compile/link/lower/validate stages completed successfully. Verification of
those direct-built production files then passed `test_four_apps.py` (1 test)
and `test_x86_rules.py` (15 tests), including the new x86 neutral consumer paths.
This run occurred after compile/link profiles and SDK/tool dependencies were
added; it is separate from the earlier size snapshot. Python was used only to
execute the verification scripts after the build finished, not by the Makefile
or any build recipe.
