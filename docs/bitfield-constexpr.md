# Bit-field width expressions

The C parser accepts a supported subset of integer constant expressions for struct and union bit-field widths. It consumes the complete expression after `:`, rather than requiring one numeric token. This allows the MicroPython declaration pattern:

```c
typedef unsigned int size_t;
struct Qstr {
    size_t total_prev_len : (8 * sizeof(size_t) - 1);
    unsigned int flag : 1;
};
```

On the ARM package, `sizeof(size_t)` in this example is four, so the width is 31. The change addresses the parser failure at these expressions; it does not establish that an entire MicroPython translation unit or application compiles.

## Supported expression subset

Supported operands are integer literals, enumerators, expanded macro expressions, parentheses, and `sizeof` values whose operand type the parser can resolve at parse time. A deferred `sizeof` whose type still requires semantic analysis is rejected in a bit-field width.

Supported operators are unary `+`, `-`, `!`, `~`; binary `+`, `-`, `*`, `/`, `%`, `&&`, `||`; integer casts; and `?:`. Integer promotions and usual arithmetic conversions use the selected target's type sizes. Casts truncate or extend according to the destination integer width and signedness, and `_Bool` casts normalize to zero or one. With floating constant folding enabled, an immediately cast floating literal is also supported; arbitrary floating arithmetic is outside this subset.

For width evaluation, `sizeof` has an unsigned integer type following the selected package's pointer width. In particular, the ARM and AMD64 packages can produce different values for `sizeof(unsigned long)` even when the field itself uses a 32-bit base type.

Signed arithmetic overflow and evaluated division by zero are diagnosed. Unsigned arithmetic wraps at the target integer width. Conditional expressions derive their common integer result type from both branches before converting the selected value.

Logical operators and `?:` evaluate only the required operand or branch. The parser still checks the supported constant-expression structure of every branch: `1 ? 3 : (1 / 0)` is accepted, while `1 ? 3 : variable` is rejected. An unselected branch does not make an unsupported operator acceptable.

Binary shifts, binary bitwise operators, comparisons, and equality operators are currently outside this subset and produce a diagnostic. This implementation is not a complete C99 integer constant-expression evaluator. Variables, function calls, assignments, comma expressions, and increment/decrement are not supported width operands.

## Width and storage constraints

The declared base must be an integer type of at most four bytes. The current IR bit-field load/store representation uses a 32-bit container; wider base types are explicitly rejected even when the requested field width is at most 32. For example, AMD64 `unsigned long value:31` is rejected because that package's `unsigned long` is eight bytes. An ARM four-byte `unsigned long` is within this storage limit.

A width must be nonnegative and must not exceed the declared base type's bit count. `_Bool` fields have a maximum width of one. Named zero-width fields are rejected. Unnamed zero-width fields use the existing separator layout behavior; under ordinary unpacked layout they end the current allocation unit, and packed layout retains its existing separator rules. Invalid declarations are diagnosed without silently clamping the width or registering an ordinary field in its place.

## Validation evidence

The six tests in [test_bitfield_constexpr.py](../tests/test_bitfield_constexpr.py) passed against the newly built compiler artifact in the parent integration run: **6/6 passed in 3.224 seconds**. The test prints SHA-256 identities for selected executable/Wasm artifacts and accepts `XHCC_COMMAND` to choose the compiler explicitly; otherwise it uses the existing project test command. These results were reported by the integration owner after the production parser change, rather than inferred from an older compiler.

Coverage includes:

- ARM `size_t` widths 31, 30, and 29, with enum, macro, and arithmetic expressions and layout assertions.
- ARM four-byte versus AMD64 eight-byte `unsigned long`, used in width expressions on a 32-bit field base.
- Unnamed zero-width separation and named zero-width rejection.
- Negative widths, signed overflow, evaluated division by zero, excessive widths, invalid base types, unsupported shifts, casts, and conditional result conversion.
- Short-circuit and conditional expressions containing unevaluated division by zero, with rejection of variables in unselected branches.
- Actual generated C compiled and linked by GNU ARM GCC, then executed as ARM instructions in Unicorn through the existing test runner.

The execution case expects return value **73**. It checks a 31-bit unsigned field beside the top flag bit, preserves the flag while rewriting the field, preserves the field while clearing the flag, sign-extends a four-bit signed field containing `-8`, preserves the neighboring 28-bit tail through read-modify-write updates, and reads the minimum signed 32-bit value from a full-width field. Its failure paths return 11 through 15.

Run the focused tests only after building the intended compiler:

```text
python -m unittest discover -s tests -p test_bitfield_constexpr.py -v
```

The compiler test does not rebuild artifacts. The execution test requires GNU ARM GCC and the existing Unicorn/ELF Python test dependencies; `XH_C_ARM_CC` can select the GNU compiler.

The real preprocessed MicroPython input was inspected read-only and includes the `qstr`, `mpz`, and traceback width-expression patterns. The subsequent full `obj.c` attempt3 result was pending when this document was written. No complete MicroPython build result is claimed here.
