# Content-addressed compiler object cache

`tools/build.py` uses `build/object-cache` by default. `--cache-dir PATH` selects
another cache and `--no-cache` compiles every translation unit. Linking,
Binaryen lowering, validation and production artifact copying still run on every
build. The cache changes build work, not compiler behavior or package formats.

Each key includes compiler executable path/SHA/version, the complete ordered
translation-unit flags, absolute source path/content SHA, project `.h`/`.inc`
contents (including object writer includes), WASI SDK system/resource header
contents, and relevant compiler environment variables. Project package headers
and generated build/test dependency directories are excluded because they are
not inputs to compilation of the compiler itself. This is a conservative global
header snapshot: changing any compiler header invalidates every TU, avoiding a
stale hit when a source begins including a newly added header. This cache is
scoped to this build script's explicit flags and SDK/header search setup, not a
universal substitute for dependency discovery in arbitrary C build systems.

Cache entries store an object and a JSON key/object digest. Hits validate the
object digest before atomically copying it to the established output path.
Misses compile to that same output path, publish the object with a same-directory
temporary replacement, then publish metadata. Failed compilation never creates
a valid entry. Concurrent identical publishers can do redundant work but expose
only complete objects; a mismatched object/metadata pair becomes a cache miss.
The linker's input paths remain unchanged, preserving readable map boundaries.

The two applications share object entries when their source and flags match.
`XH_DRIVER_NO_MAIN` is applied only to the driver translation unit for xhccm;
repository search confirms no other compiler TU consumes it. Make sources remain
selected only for xhccm. This avoids rebuilding the shared compiler for a macro
that previously affected the command line of every TU without affecting them.

`tests/test_build_cache.py` compiles small real WASI translation units and tests
cold/hit behavior, source/header/flag/tool identity changes, corruption rejection,
and exact object equality with cache disabled. Full application cold/warm/
`--no-cache` SHA equivalence is a separate integration gate run only while all
source writers are frozen. A build invoked while source/header files change is
not a supported reproducibility snapshot; coordinate builds with agents first.

One frozen-source Windows integration run built both applications sequentially:

| Mode | Hits | C compilations | Elapsed host seconds |
| --- | ---: | ---: | ---: |
| New empty cache | 58 | 62 | 93.29 |
| Warm cache | 120 | 0 | 21.61 |
| Cache disabled | 0 | 120 | 65.40 |

All three runs produced identical per-application SHA256 values:

- xhcc, 906911 bytes: `ab7e1e93d86c8c0f460f1cdc5e01022e4dfc715ffb43715d5f591a6a86ea8bd6`
- xhccm, 933179 bytes: `9d578c4b70942bee79b6c141dd39240394f4211cea9ca127e9f8e13c4d06e2d9`

This is a snapshot, not a fixed future artifact size or an MCU performance
claim. The cold cache run was slower than the no-cache run; disk/compiler
warming and cache publication cost affect wall time. The direct correctness
results are identical artifacts and zero C compilations in the warm run.
