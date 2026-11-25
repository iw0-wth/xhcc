# Isolated expanded development budgets

The supported production defaults remain a 1 MiB compiler arena and a 4 MiB
Wasm linear-memory ceiling. Temporary development budgets are allowed to expose
the next correctness issue on large translation units; they are not a claim that
the production memory target has been met for those inputs.

```
python tools/build.py --profile expanded --arena-kib 2048 --memory-mib 8 --app xhccn
python tools/build.py --profile expanded --arena-kib 2048 --memory-mib 8 --app xhccn --print-artifacts
```

`--arena-kib` is a compile-time ARK_POOL_SIZE override. `--memory-mib` controls
both the Wasm initial and maximum memory. Any nondefault budget automatically
selects the isolated expanded profile, even without `--profile expanded`.
Explicit expanded mode is also isolated when budgets equal production defaults.
The arena must be positive and smaller than the declared memory; actual static,
stack and dynamic headroom is checked by linking and execution, not estimated
from a fixed margin. Compiler stack and other buffer sizes are unchanged.

Expanded outputs live at `build/wasm/dev/expanded/<target>/<app>/<app>.wasm`.
They are never copied over `build/wasm/<app>.wasm` or installed to SD. Native
builds use the analogous `build/native/dev/expanded` path. The planned artifact
JSON reports exact paths, budgets, effective profile and publication status
without compiling or creating cache entries.

Each completed build writes `build-config.json` next to its artifact, including
compiler identity, ordered flags, link command, budgets, artifact SHA/size, map
path and publication status. These are configuration facts. Capture actual
`--memory-report` arena peaks and linker requested-heap peaks separately for each
representative input; do not treat metadata budgets as measured peak RAM or RSS.
Expanded arena flags participate in object-cache identity, preventing accidental
reuse of an object compiled with the production pool size.

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
