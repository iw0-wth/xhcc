# Isolated expanded development budgets

The supported production defaults remain a 1 MiB compiler arena and a 4 MiB
Wasm linear-memory ceiling. Temporary development budgets are allowed to expose
the next correctness issue on large translation units; they are not a claim that
the production memory target has been met for those inputs.

```
python tools/build.py --profile expanded --arena-kib 2048 --memory-mib 8 --app xhccn
python tools/build.py --profile expanded --arena-kib 2048 --memory-mib 8 --app xhccn --print-artifacts
```

`--arena-kib` is a compile-time ARK_POOL_SIZE override. `--asm-kib` overrides
ARK_ASM_BUF_SIZE (default 128 KiB). `--code-kib` overrides ARK_CODE_BUF_SIZE
(default 32 KiB); default values omit override flags so the
production compilation cache keys remain unchanged. `--memory-mib` controls
both the Wasm initial and maximum memory. Any nondefault budget automatically
selects the isolated expanded profile, even without `--profile expanded`.
Explicit expanded mode is also isolated when budgets equal production defaults.
Arena, assembly-text and machine-code buffer sizes must be positive, each
bounded to 1 GiB, and their sum must be smaller than the declared linear memory. This is only a
minimum feasibility check: other static buffers, stack and dynamic headroom are
validated by linking and execution, not estimated from a fixed margin. Compiler
stack and other buffer sizes are unchanged.

For an emit-stage assembly-text capacity failure with arena headroom, use a
separate budget rather than blindly growing the arena, for example:

```
python tools/build.py --profile expanded --arena-kib 4096 --memory-mib 16 --asm-kib 512 --app xhccn
```

This example configures 524288 assembly bytes; it does not itself establish that
a particular TU compiles or that the full runtime memory budget is sufficient.
For a confirmed machine-code output capacity failure, the next isolated probe
can add `--code-kib 256` to that command, keeping arena and linear memory fixed.
Assembly-text and machine-code capacities are separate limits; successful emit
alone does not prove that assembly fits. The object transport buffer is not
changed by this option, and standard ELF output uses its own checked allocation.
Configured `code_bytes` is recorded alongside `asm_bytes` in plan/build metadata.
Actual assembly success and output validation must be measured after building;
parameter tests alone do not prove that a large translation unit compiles.
The no-Python Makefile buffer-budget interface is not changed in this phase.

Expanded outputs live at `build/wasm/dev/expanded/<target>/<app>/<app>.wasm`.
They are never copied over `build/wasm/<app>.wasm` or installed to SD. Native
builds use the analogous `build/native/dev/expanded` path. The planned artifact
JSON reports exact paths, budgets (`arena_bytes`, `asm_bytes`, `memory_bytes`), effective profile and publication status
without compiling or creating cache entries.

Each completed build writes `build-config.json` next to its artifact, including
compiler identity, ordered flags, link command, budgets, artifact SHA/size, map
path and publication status. These are configuration facts. Capture actual
`--memory-report` arena peaks and linker requested-heap peaks separately for each
representative input; do not treat metadata budgets as measured peak RAM or RSS.
Expanded arena flags participate in object-cache identity, preventing accidental
reuse of an object compiled with the production pool size.

## Bounded project linker profile

`--linker-profile default|project` selects a fixed capacity set, independently
of compiler arena/text/code budgets. Default limits remain 32 objects, 512 input
sections, 8192 input symbols, 128 KiB names, 512 KiB requested linker heap and
512 KiB output. Default mode adds no linker override flags.

The development `project` profile sets 256 objects, 2048 sections, 32768 symbols,
1 MiB names, 4 MiB requested linker heap and 2 MiB output. It automatically uses
expanded isolation and never publishes over production root or SD applications.
All six macros are passed consistently to translation units and recorded in
`linker_limits` / `linker_compile_defines` metadata. Existing link algorithms,
input symbol retention and target validation are unchanged; this is a bounded
capacity increase, not a memory optimization or proof that a project links.

```
python tools/build.py --app xhlinker --profile expanded --linker-profile project --memory-mib 32
```

The standalone linker does not link the compiler arena, even though common
build metadata records the configured arena default. Measure its requested
heap report on actual inputs, and separately validate its declared linear
memory and executable output. Script FLASH/RAM bounds remain enforced; a larger
output capacity does not expand the target firmware memory map.

## Recorded development validation

The 2048 KiB arena / 8 MiB linear-memory xhccn was actually built in the isolated
path. Wasm memory inspection confirmed initial=max=8 MiB; a simple compile
reported arena capacity 2,097,152 bytes. Production root and deployed SD file
hashes remained unchanged. Build-profile/cache tests passed 5/5.

MicroPython `py/obj.c` attempt-4 used the same source, verified generated headers
and explicit MCU flags as the default-budget attempt. It no longer exhausted the
arena, but was rejected at function-local `sizeof` of a variably modified type
used by `MP_STATIC_ASSERT_NONCONSTEXPR`; no object was produced. This is progress
to a new diagnostic, not a successful MicroPython compile or a production-memory
result. Per-attempt source, module and tool records remain in the SD evidence.

Attempt-5 then used the same 2 MiB/8 MiB budgets with the supported function-local
VLA `sizeof` correction. The standalone language tests passed, but real `obj.c`
again reached arena exhaustion without an object. That build had no stage/request
statistics, so the failure cannot yet be attributed to parser, sema, IR or output.
RAM optimization is now a parallel priority: add non-allocating stage/request
reports, repeat the fixed TU, and reduce duplicate storage and overlapping
lifetimes before claiming the production budget can handle it.

Attempt-7 provided stage reports and located the failure in `ir.unreachable`.
After reclaiming its temporary CFG scope, attempt-8 completed opt at the same
budgets (used 2,080,792; high 2,083,344 bytes). It then failed during emit while
requesting 16,384 bytes, with no object. The real stage progression confirms an
optimization benefit without claiming complete compilation; see the detailed
[resource measurements](resource-budget.md).

Attempt-9 then removed the Thumb-2 main assembly-text arena duplicate and completed
emit at the same budget, but assembly still exhausted the arena. Attempt-10 used
a separate 4096 KiB arena / 16 MiB linear configuration with the same source and
MCU profile and first produced an object (compile exit 0). Its arena peak was
2,154,808 bytes, 57,656 above 2 MiB. ELF validation is recorded separately; object
production alone is neither execution verification nor a complete firmware build.
The 16 MiB declaration is a development budget, not measured total MCU RAM.

A future reduction effort should retain the exact source/configuration and
stage failure/peak records from the expanded attempt, then reduce live AST/IR,
source buffering or rule heaps before lowering the arena again. A successful
expanded compile alone does not establish that the input fits a 1 MiB arena.
