# Temporary bounded project linker profile

This development profile raises fixed capacity limits for large real projects.
It does not implement symbol compaction, undefined-symbol pruning, string
interning or a new symbol lookup algorithm. The normal supported default limits
remain unchanged.

```
python tools/build.py --app xhlinker --profile expanded --linker-profile project --memory-mib 32
```

| Limit | Default | Project |
| --- | ---: | ---: |
| Input objects | 32 | 256 |
| Aggregate input sections | 512 | 2048 |
| Aggregate input symbols | 8192 | 32768 |
| Stored names | 131072 B | 1048576 B |
| Requested linker heap | 524288 B | 4194304 B |
| Output capacity | 524288 B | 2097152 B |

The six XH_ELF_LINK_MAX_* definitions are guarded defaults. Project mode supplies
six explicit compiler definitions and records their exact numeric values in
`build-config.json` under `linker_limits` and `linker_compile_defines`. It adds no
capacity override flags in default mode. The linker uses the configured heap
maximum when `--max-memory` is omitted; a user-requested heap limit may lower,
but not exceed, that configured bound.

Project mode automatically selects isolated expanded output, including when the
caller omits `--profile expanded`. The standalone artifact is
`build/wasm/dev/expanded/all/xhlinker/xhlinker.wasm`; its map and metadata are
beside it. It does not replace `build/wasm/xhlinker.wasm` or deployed SD files.
The compiler arena setting remains common build metadata, but standalone
xhlinker does not link the compiler arena implementation.

The 32 MiB linear memory setting is a temporary development configuration, not a
measured total RAM requirement. `--memory-report` records peak requested core
heap bytes, excluding allocator overhead, stack, filesystem caches and runtime
RSS. Actual object/section/symbol counts and output size must accompany any
claim that an input links. ELF output production does not alone prove target
execution or firmware completeness.

The profile leaves the input algorithm unchanged: it retains symbol metadata
for all input SYMTAB records, including currently unreferenced entries; it reads
objects individually rather than keeping all input file images resident. A
future memory optimization must preserve input-index mappings, weak/COMMON and
strong-definition precedence, relocation references, local anchors and entry
resolution. Simply deleting undefined-symbol records is unsafe.

Build-plan/cache tests validate default flags, project isolation, six bounded
values and invalid profile rejection. Project compilation has completed, but
this document intentionally does not claim that the full 139-object firmware
link has succeeded: the host linker result and its reports are separate evidence.
Target FLASH/RAM/script bounds remain enforced regardless of larger heap/output
capacities.
