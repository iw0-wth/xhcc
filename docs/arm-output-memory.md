# ARM output-buffer memory behavior

The ARM Thumb-2 emitter now writes its main assembly text directly into the
caller-provided output buffer. Previously, the main `AsmBuffer` allocated an
arena buffer starting at 4096 bytes and grew it geometrically; `arm_emit` then
copied the completed text into the caller's already allocated output buffer.
The borrowed fixed buffer removes that separate main text allocation and the
final copy. It does not move the buffer to the heap or increase the configured
assembly-output limit.

## Ownership and capacity

[AsmBuffer](../include/codegen.h) distinguishes dynamic arena storage from
borrowed fixed storage. Existing dynamic initialization remains the default
for other backends and local rewrite buffers. A fixed buffer never calls the
arena allocator or reallocates its caller's storage.

`asmbuf_require` takes the desired text length excluding the terminating NUL.
A capacity of `N + 1` therefore holds exactly `N` text bytes. Capacity failures
are sticky: appends and checked splices stop writing, the already stored text
remains terminated, and the length does not advance past the capacity. The ARM
emitter returns an error and clears the public output when capacity is
insufficient, so a partial program is not published as successful output.
Null output and nonpositive output capacity are rejected without writing.

ARM's direct slice rewrites use one checked splice helper. This includes far
offset correction, peephole rewrites, register allocation, virtual stack,
address folding and CBZ folding. Capacity checks occur before their raw copy
and length updates; a failed reserve cannot be followed by an unchecked write.
Runtime helper pruning only compacts existing text in place.

## What still allocates

This change applies to the main Thumb-2 assembly accumulator. Per-function
rewrite scratch buffers, optimizer structures, assembler label/reference tables
and other compiler data can still allocate arena storage. Existing dynamic
buffers in C, x86 and RV code generation retain their behavior.

The Thumb-1 path still generates its intermediate assembly dynamically, runs
its separate legalizer, and copies the legalized result. It is not included in
the main-buffer memory saving described here.

The fixed capacity must cover the largest intermediate text, not merely the
final optimized result. In particular, an O2 optimization can shrink text after
an earlier larger emission. A buffer large enough for the final result alone
can correctly fail before that shrink occurs. This change does not introduce
streaming assembly or guarantee successful compilation of every input within
any particular RAM budget.

## Validation and measurement limits

[The fixed-buffer test](../tests/test_asmbuf_fixed.py) compiles and runs the
production common-buffer implementation. It checks capacities zero and one,
exact text-plus-NUL fit, overflow, invalid desired lengths, null storage,
sticky failure, untouched surrounding canaries, and zero allocation calls for
fixed buffers. Dynamic growth remains covered separately in that harness.

[The ARM emitter test](../tests/test_arm_emit_buffer.py) compares the private
dynamic reference constructor and the production fixed constructor on the same
IR. Text must match byte for byte at O0 and with the main text optimizations
enabled. Its fixture contains repeated edits and dense branches. It also
checks exact capacity at O0, sufficient intermediate-peak capacity with
optimizations, and small-capacity failures with unchanged canaries and cleared
public output.

These isolated tests establish ownership, bounds and output equivalence.
Expanded attempt 9 also ran the real MicroPython input under the 2 MiB arena /
8 MiB linear-memory configuration. Assembly-text emission completed with arena
usage unchanged at 2,081,144 bytes before and after emission; the recorded high
water was 2,083,344 bytes. Thus the main emission phase had zero net arena
growth in that run.

The following assembly phase still failed: it requested 1,536 bytes when arena
usage was 2,096,936 bytes against the 2,097,152-byte capacity. No object file was
produced. This is evidence that the emission allocation obstacle was removed,
not a claim that this input now compiles successfully or that firmware output
is complete. Assembler and other allocations remain separate limiting factors.
