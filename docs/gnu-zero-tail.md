# GNU zero-length struct tails

The C frontend supports a limited GNU extension needed by trailing `state[0]`
fields in MicroPython's `py/bc.h`:

```c
struct State {
    unsigned count;
    void *state[0];
};
```

An explicit integer constant zero bound is accepted only on the outermost array
type of the last struct member. A constant expression such as `[1 - 1]` is also
accepted. The parser validates the member's position after reading the complete
member list; a trailing static assertion does not introduce another member.

The frontend records explicit `[0]` with `Type.is_zero_array`, distinct from an
unspecified or flexible `[]`. The marked array is not a VLA. Its size is zero,
its natural alignment is its element type's alignment, and its member offset
follows the ordinary field alignment rules, including supported packing rules.
The containing struct still receives its normal alignment and tail padding, so
arrays of the struct retain the corresponding stride. `sizeof` of the marked
member is the constant zero, including an unevaluated expression such as
`sizeof(((struct State *)0)->state)`. Constructing this member type does not mark
or resize other instances of its element type.

Standard ARM ELF object output supports this layout. The C source backend
preserves the explicit `[0]` spelling instead of emitting `[]`. Private IR
serialization explicitly rejects this extension; its type codec does not yet
preserve the zero-array marker. It must not silently round-trip `[0]` as `[]`.

## Deliberate boundaries

This is not general GNU zero-length-array support. The following remain rejected:

- Zero-length local or global object arrays and zero-length array typedefs.
- Standalone type names such as `sizeof(char[0])`.
- Zero-length union members and non-final struct members.
- Zero-length inner dimensions such as `int tail[2][0]`.
- A pointer-to-zero-array field such as `int (*tail)[0]`.
- Negative constant array bounds, including negative tail bounds.

A supported multidimensional tail can have a zero outer bound and positive
fixed inner bounds, for example `unsigned tail[0][3]`. Existing flexible `[]`
member behavior is separate from this extension.

## Validation

Run `tests/test_gnu_zero_tail.py` after a coordinated compiler build. The suite
uses `test_driver.command()` and its `XHCC_COMMAND` JSON argv override; it never
rebuilds the compiler. GNU Arm tools, pyelftools and Unicorn are required for the
object execution checks.

All six tests passed against the coordinated expanded development compiler
(916438-byte `xhccn.wasm`). Coverage includes seven GNU ARM layout comparisons
under `-std=c99 -Wall -Werror`, checking size, alignment, member offset and struct
array stride; constant member `sizeof`; element-type isolation; the two real
MicroPython code-state structure shapes; C backend preservation and GNU execution;
and diagnostics for the rejected forms above. GNU acceptance here is extension
acceptance without `-pedantic`, not a claim that zero-length arrays are ISO C99.

The original runtime failures at `py/bc.h:253` and `:266` were the two trailing
`state[0]` fields. This change addresses that parser restriction without modifying
MicroPython sources. It does not by itself establish that the entire runtime
translation unit or firmware builds successfully; other compilation requirements
remain independently validated by the engineering build.
