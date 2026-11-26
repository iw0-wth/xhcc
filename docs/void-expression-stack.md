# Void and comma expression stack results

The IR generator must discard the representation actually produced by an
expression. A direct call returning `void` produces no evaluation-stack result
on the ARM emitter. An unconditional POP after that call is an underflow, even
when the function epilogue would subsequently restore SP from the frame pointer.

The correction in `src/ir/irgen.c` follows these rules:

- A comma expression discards its left operand using `irgen_discard_result`,
  including its type-dependent representation width. Its resulting
  representation is exactly that of its right operand; statement-level discard
  follows the right operand through nested comma expressions.
- A cast to void evaluates its operand and discards the operand representation
  inside cast lowering. Discarding the resulting void cast does nothing. This
  also handles nested void casts and casts whose operand is a void call.
- A void conditional expression discards the selected branch's representation
  within that branch. Both outgoing branch edges therefore carry zero result
  words, and discarding the whole conditional does nothing.

This reuses the existing target-dependent representation selection in
`irgen_discard_result`, including `g_irgen_64bit_slots`, integer/double pairs,
aggregate address versus register representations, and complex word counts. It
adds no ABI rule and makes no change to the strict ARM alloca effect callback or
CFG validator. Non-void conditional expressions retain their prior lowering.

## Real-source evidence

The original MicroPython sources were not changed. In the active configuration,
`m_del` expands to `((void)(num), m_free(ptr))`, and `m_del_var` has the same shape.
Private ARM diagnostic instrumentation identified these failures:

| Function | Call instruction | Following instruction | Validator result |
| --- | --- | --- | --- |
| `mp_call_method_self_n_kw` in `py/objboundmeth.c` | IR88 `m_free`, entry depth 1, pop 1 / push 0 | IR89 POP at depth 0 | `XH_STACK_UNDERFLOW` |
| `fun_bc_call` in `py/objfun.c` | IR325 `m_free`, entry depth 1, pop 1 / push 0 | IR326 POP at depth 0 | `XH_STACK_UNDERFLOW` |

Both calls had `val=8589934593` (void-return bit 33 and argument count 1),
`aux=0`, and `aux2=0`. This was a generic expression-result bug, not an unknown
opcode, floating-point shape, or incorrect CFG effect.

Raw diagnostics are in `build/privatealloca-debug/objboundmeth.stderr` and
`build/privatealloca-debug/objfun.stderr`. The instrumented private backend was
compiled with the existing expanded build metadata flags and linked with that
build's object list. The private corrected module then replaced the IRgen object
as well. It did not overwrite the formal expanded artifact or its metadata.

The original wrapper invocation arguments for both real translation units were
replayed against `build/privatealloca-debug/fixed.wasm`; only the module path and
output/dependency destinations were changed. Both compilations exited zero.
Their raw output is in `build/privatealloca-debug/objboundmeth-fixed.stderr` and
`build/privatealloca-debug/objfun-fixed.stderr`. These private probes are not a
substitute for the lead's unified build and original make rerun.

## Focused execution regression

`tests/test_void_expression_stack.py` was run using `XHCC_COMMAND` selecting the
private corrected module. It compiled at `-O0` and `-O2`, linked using GNU ARM ld,
and executed with Cortex-M3 Unicorn. Its fixture combines alloca with:

- a comma whose right operand is a direct void call;
- void casts, nested casts, and casts around comma expressions;
- a direct void call in a comma's left operand;
- a long-long call in a comma's left operand;
- a void conditional expression and an assignment cast to void.

Calls with loop counts 1, 41, and 13 returned `23 * n`; every invocation restored
SP exactly without resetting it between calls. The fixture checks semantic side
effects as well as stack balance. Both optimization levels passed.

These results certify this focused ARM Thumb2 regression. They do not establish
that every target or ABI path has passed; broader target claims require the
corresponding builds and execution tests. The source change remained frozen for
unified integration after this focused validation.
