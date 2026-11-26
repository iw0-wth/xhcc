# ARM Thumb-2 verified dynamic stack allocation

Evaluated `__builtin_alloca` is enabled only for the ARM engine. Its ARM
Thumb-2 consumer verifies the function's target-word value stack before
emission. Other targets retain the explicit lowering guard; the initial
Thumb-1 subset rejects dynamic allocation. This is not a claim of dynamic
stack support across all ABIs or every legacy IR operation.

## Verified stack contract

[The generic CFG helper](../include/xh_ir_stack.h) uses a target callback to
obtain exact pop/push effects, including taken edges. It validates labels,
branch targets, underflow and identical depths at joins and cycles. It does
not mutate the IR. ARM activates this validation only for functions containing
`IR_ALLOCA`; functions without allocation keep their existing path.

The ARM callback certifies integer/pointer operations, supported scalar memory
shapes and calls whose wire sizes and return convention are understood.
Unknown effects, complex/floating effects, raw ASM, unverified packed memory
forms, naked/IRQ allocation functions and unsupported call encodings are
rejected. The allocation function currently has a 65,536-instruction and
4,096-value-word validation budget. This conservative subset can reject a
program that another backend could support; it must not guess a zero effect.

After popping the allocation byte count, let `S` be SP and `L` the byte extent
of the still-live expression values. The emitted external sequence computes:

```
P = align_down(S + L - requested_bytes, 8)
new_SP = P - L
```

It copies the live words forward from the old expression stack to the new
one, preserving their order, and pushes `P` as the result. The object starts
above the moved expression values, so an existing argument or assignment
address is not separated from the stack by allocation padding. The fixed frame
pointer remains unchanged, and the function epilogue restores SP from that
frame. Runtime stack exhaustion is not detected or converted into a successful
allocation result.

The sequence is selected through the shared semantic selector and stored in
[arm.rules](../packages/arm/arm.rules). Its copy loop keeps the end pointer in a
low register; it does not depend on an unimplemented high-register CMP form.
The compiler module did not need rebuilding for that external rule correction.

Functions containing allocation disable reg-top caching, tail calls and the
uncertified post-emission stack/text optimizations. These are function-local
choices; configuration is not globally toggled, and subsequent ordinary
functions keep their selected optimization settings.

## Memory cost

The generic validator allocates nothing itself. The ARM caller temporarily
allocates exactly three `int` arrays with one entry per IR instruction, using
heap workspace for depths, worklist and resolved targets, and releases them
when that function's emission is complete. On the current 32-bit execution
profile this is `12 * instruction_count` bytes, in addition to other compiler
memory. This is extra transient RAM, not an arena-memory saving. Removing the
main assembly buffer's arena copy does not remove this workspace requirement.

## Validation

[ARM execution tests](../tests/test_arm_alloca.py) compile production C at O0
and O2, link with GNU ARM ld, and execute live arguments, a live assignment
address, multiple live values, multiple allocations, conditional allocations,
zero-size alignment, runtime sizes, loops and void calls. Ten entry points
are each executed 30 times at each optimization level, with SP checked after
every return. Examples include `sink(alloca(9), 17)` and an array-element LHS
that stays live across allocation. Thumb-1 and unverified ASM cases must fail
without an object file.

[Intrinsic contract tests](../tests/test_alloca_builtin.py) retain count/type
errors, unevaluated `sizeof` behavior across targets and explicit rejection on
uncertified targets. The new generic CFG tests separately cover malformed
labels, effects, joins and underflow.

The production package passed the two ARM execution tests and seven intrinsic
contract tests with the same expanded compiler module. At that checkpoint the
compiler SHA256 was
`e47f6d8f4a629756bde519e569a6a4311d9f0dd3da160d92418250534b3501e5`, and the
corrected ARM rule resource SHA256 was
`970217e38ad2f7f170a9a7ca82164e9b196271c0f1a480c0c0c57c4477e80a02`.
These results establish the tested execution subset; they do not claim that a
whole firmware build or arbitrary IR function now succeeds within its RAM
budget.
