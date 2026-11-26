# ARM naked basic-asm contract

A naked ARM function has no compiler-generated frame. Its basic assembly owns
entry registers and stack handling. The compiler must therefore not emit its
ordinary parameter homing stores for such a certified function: an instruction
such as `STR r0, [r6, #0]` would address the caller's frame, not a callee frame.
Normal, non-naked function parameter handling and ABI behavior are unchanged.

## Certified subset

Before emitting a naked body, the ARM backend checks a bounded, frame-free IR
subset: function markers, parameter declarations (which emit no homing), labels
and basic asm without operand bindings. The established literal-return fallback
is also accepted, but only as a complete chain:

```
PUSH_INT (ordinary one-word form)
[ optional TRUNC: width 8, 16 or 32; signedness 0 or 1 ]
RETURN
```

The optional truncation is necessary because an unsigned literal return can be
lowered with an explicit conversion. It is not permission for arbitrary C in
naked functions. Parameter reads, local-variable loads/stores, compiler-frame
addresses, calls and other unsupported IR operations are diagnosed instead of
being silently emitted against an uninitialized frame pointer. Operand-bound
asm in a naked body is outside this certified subset.

This preserves a historical frame-free literal return tail used by some source
compatibility branches. Such a tail may be unreachable after an assembly tail
branch; the compiler does not infer arbitrary assembly control flow or promise
general mixed naked C support.

## Observed failure and focused validation

The pre-fix real firmware reached its REPL prompt and echoed `print(1+2)`, but
passed incorrect input parameters to the lexer. The same UART model with the
GNU firmware produced `3`.

Read-only disassembly and execution hooks identified the first corruption at
`nlr_push` entry. The function was naked, yet its first generated instruction
homed r0 to `[r6, #0]`. r6 still pointed at the caller's frame, where the caller
had saved its source pointer. The store replaced that source pointer with the
NLR buffer address. Subsequent vstr-field reads therefore interpreted the NLR
return PC as the input buffer and the saved return value as length zero. The
NLR tail function's frame restoration was not the first failure.

The correction applies to every certified naked/basic-asm body, not to a named
NLR function. Neither source port logic, UART input, nor emulated CPU registers
were changed to hide the error.

[The focused tests](../tests/test_arm_naked_parameters.py) passed against the
new compiler. A named-parameter naked asm leaf returns 7; its caller's local
value 31 is preserved, yielding 38, and SP is restored. The leaf's first bytes
are the intended `MOVS r0, #7`, not a parameter homing store. Separate tests
require diagnostics and no object for naked C parameter access and local
variables. The unsigned literal-return case retains its optional TRUNC chain.

Host attempt 11 passed the fresh real NLR object gate: `nlr_push` now starts
with bytes `c460` (the intended store of r4), without the old r6-based parameter
homing store; the seven startup symbol references also remain present. A fresh
firmware REPL run is still a separate pending gate, and no REPL success is
claimed here. No additional naked C features or RAM changes are part of this fix.
