# GNU builtin lowering: clz and expect

This frontend supports the two reserved builtin names required by the current
MicroPython host-build inputs. It does not yet implement the broader GNU
clz/ctz/popcount family.

## Implemented signatures

```c
int  __builtin_clz(unsigned int value);
long __builtin_expect(long expression, long expected);
```

The signatures describe intrinsic semantics; no source declaration, external
helper, or fake library definition is required. These reserved names remain
intrinsics with `-fno-builtin`. Ordinary function names such as `clz` are not
intercepted. This implementation requires integer arguments and diagnoses an
incorrect argument count, including inside `sizeof`.

### `__builtin_clz`

The single argument is converted to the target's 32-bit `unsigned int` and
is evaluated exactly once. The result is an `int` counting leading zero bits:
`1` gives `31`, `0x80000000U` gives `0`, and `-1` converts to `UINT_MAX` and
gives `0`. Wider integer inputs are converted to unsigned int before counting.

GNU specifies undefined behavior for a zero input. The current implementation
happens to return `32`; this is an implementation regression-test choice, not
a defined-input GNU result, a supported semantic guarantee, or a promise for
future optimizations.

The frontend emits a target-neutral IR loop with private frame locals: store
the converted operand, initialize a count to 32, and repeatedly decrement the
count and logically shift the operand right until it is zero. Nested calls,
recursion, and live surrounding expression operands do not share scratch
state. The loop uses existing local-load/store, binary, label, and branch IR
operations; it adds neither an IR wire opcode nor an external helper.

ARM Thumb-2 already has a CLZ assembler encoder and package templates, but this
lowering deliberately does not select a native CLZ instruction. A future
native optimization must retain the shared-IR policy and preserve the frontend
signature and single-evaluation behavior. The generic lowering does not assume
CLZ hardware support.

### `__builtin_expect`

Both operands are converted to the target's signed `long`, and the returned
value has type `long` and equals the converted first operand. Target long
width is used rather than the build host's width: ARM uses 4 bytes and the
xhcc x86-64 target uses 8 bytes.

The second operand is not discarded as an unevaluated hint. GCC accepts a
nonconstant second operand and executes its side effects; this implementation
likewise evaluates each operand exactly once. It follows the compiler's usual
right-to-left call-argument evaluation choice: evaluate and discard the second
operand with the existing target-aware discard helper, then evaluate the first
operand and retain its result. Programs must not rely on a portable ordering
between ordinary call arguments. No branch probability optimization or
external call is emitted.

`sizeof` retains these intrinsic expressions until semantic checking, so result
types and argument diagnostics are correct without executing operand side
effects. In particular, `sizeof(__builtin_expect(...))` is `sizeof(long)`, not
an assumed `sizeof(int)`.

## Validation and scope

[test_builtin_bitops.py](../tests/test_builtin_bitops.py) covers:

- Argument-count and noninteger-argument diagnostics, including `sizeof`.
- ARM, RISC-V32, x86, x86-64, and C output at `-O0` and `-O2`, with
  `-fno-builtin` and no external references to either builtin.
- ARM CPU execution and stack restoration, including bit 31, signed/wide input
  conversion, nested clz, surrounding live operands, both expect operand side
  effects, result types, unevaluated sizeof, and an ordinary `clz` function.
- Generated C compiled and executed with host GCC at both optimization levels.

The focused suite passed all four test methods using both the expanded compiler
and the default full compiler. A fresh host GNU make build of all 139 current
firmware C objects also passed, with zero remaining relocations referencing
`__builtin_clz` or `__builtin_expect`.

Those results establish frontend lowering and object-compilation coverage, not
a fully linked or running firmware. Runtime-object integration and the remaining
independent ARM backend/link validation are outside this builtin change.
