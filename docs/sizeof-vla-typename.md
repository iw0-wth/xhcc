# `sizeof` of a variable-length array type-name

The C frontend supports a block-scope `sizeof` array type-name with one outer
variable bound and fixed-size elements. For example:

```c
unsigned bytes(int n) {
    return sizeof(int[n]);
}
```

This computes the bound once, converts it to the target's unsigned `size_t`
type, and multiplies by the fixed element size. It does not create an array or
emit an `IR_ALLOCA` instruction. A bound with side effects is evaluated once:

```c
int n = 3;
unsigned bytes = sizeof(int[n++]); /* bytes == 12, n == 4 on a 32-bit int target */
```

The result type follows the selected target data model: unsigned int for a
32-bit pointer target, unsigned long for LP64, and unsigned long long when the
pointer is 64 bits but long is 32 bits. Constant and deferred expression forms
of `sizeof` use the same result-type selection.

The parser keeps the bound on the frontend type and transfers it to an explicit
dynamic `sizeof` AST payload. Semantic analysis checks the integer bound and
complete element size. Lowering uses the ordinary integer conversion and
multiplication IR paths. The bound AST is frontend-only; it is not part of the
serialized IR type format. Existing `sizeof` of a declared local VLA continues
to read the byte count saved by that declaration, rather than reevaluating the
original bound.

## Address comparisons in assertion expressions

An expression such as this can have a variable-length array type even when the
application intends the predicate to describe a permanent object relationship:

```c
extern int a, b;
void check(void) {
    ((void)sizeof(char[1 - 2 * !(&a != &b)]));
}
```

The compiler preserves the comparison and computes the size at runtime. It does
not infer that different symbol names necessarily have different addresses.
Different external names can resolve to one address through a linker or GNU
alias extension; unresolved weak symbols can also share an address.

The constant evaluator folds equality or inequality between two pure address-of
expressions resolving to the same object symbol. Thus `&a != &a` is known false,
and the resulting constant negative array bound is rejected. Explicit constant
zero and negative bounds are rejected as well. Unknown runtime bounds are not
replaced by a one-element array. A nonpositive bound that remains a runtime value
violates C's VLA bound requirements; this implementation does not add a runtime
trap for it.

ARM GCC 15.2.1 was checked with `-std=c99 -O0 -pedantic-errors`. It accepted the
block-scope extern-address example and generated a runtime address comparison.
For distinct objects defined in the translation unit it folded the size to one.
It diagnosed `sizeof(char[1 - 2 * !(&a != &a)])` as a negative array size, and
rejected an extern-address variable bound at file scope. A GNU alias case was
accepted as a VLA expression and returned unsigned minus one when both names
resolved to the same address. Acceptance therefore does not prove that an
extern-address comparison is an ISO integer constant expression, or that such a
macro always diagnoses a false predicate during compilation.

The regression test links xhcc-generated code against GNU-generated distinct and
aliased objects. This checks that external identity remains a runtime comparison
instead of being guessed from the symbol spelling.

## Supported boundary and validation

The added type-name support is limited to a single outer variable dimension and
fixed-size elements, in block scope. It does not add VLA typedef support, inner
variable dimensions, pointers to VLAs, or file-scope variably modified arrays.
Those retain the frontend's existing unsupported diagnostics. Pointer-to-VLA
`sizeof` and its potentially unevaluated bound semantics are outside this change.
No MicroPython source macro changes or symbol-name special cases are needed.

Run `tests/test_sizeof_vla_typename.py` after a coordinated compiler build. It
uses `test_driver.command()` and accepts the `XHCC_COMMAND` JSON argv override;
it never rebuilds the compiler. Its six tests cover ARM execution, side effects,
element-size scaling, nested `sizeof`, external address identity including GNU
aliases, invalid bounds, and target result-type selection. ARM execution needs
GNU Arm tools, pyelftools and Unicorn, as used by the repository's other execution
tests. The six tests passed against the expanded development compiler during
this change. The corresponding real MicroPython `obj.c` retry no longer reported
the previous VLA errors, but exhausted the configured 2 MiB arena before emitting
an object; this change alone does not establish that the full translation unit or
firmware builds successfully.
