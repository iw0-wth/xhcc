# ARM extern symbol lookup and cache capacity

The ARM backend's fixed extern table is an optional fast path, not a complete
record of a translation unit's declarations. Reaching its capacity must not
turn a valid late-declared symbol into address zero.

## Previous failure

In the small compiler profile the table held 128 extern declarations. Header
files could fill it before linker-script symbols such as `_sidata` and
`_sdata` were registered. The frontend symbol lookup and emitted IR still
contained those valid declarations and symbol names, but the ARM cache dropped
later entries. A subsequent cache miss was treated as an unknown object with
an initialized address value of zero. That silently omitted the required
symbol relocation.

This was separate from the behavior of a naked function containing ordinary C
locals: such a function also lacked the compiler's normal frame setup. Fixing
extern lookup does not add general support for arbitrary naked C bodies.

## Lookup contract

[The ARM backend](../src/backend/arm/backend_arm.c) keeps a borrowed pointer to
the current IR program during code generation. It does not copy the program or
allocate a larger static declaration table.

Lookup preserves assigned local/global storage as its first choice. When the
extern fast path misses, it searches the current program's complete declaration
list. Valid extern objects then retain symbolic address materialization and
relocation in standard ELF mode. The fallback also obtains the declared object
width, so a late 64-bit extern is not accidentally loaded as one word. Explicit
absolute-address declarations retain their `has_at` behavior; an intentionally
assigned address zero is not confused with an unknown declaration.

An address with neither a declaration nor assigned storage now produces a
code-generation error and fails the output buffer. It must not be invented as
address zero, and a typo must not be accepted by treating every unknown name as
an implicit external symbol.

The active program pointer and lookup counts are reset for each emission, and
the borrowed pointer is cleared at the end of code generation. A second
translation unit in the same process cannot reuse the first unit's extern
facts. Existing capacity checks for defined-object layout remain unchanged.

## Validation and remaining startup boundary

[The extern-capacity tests](../tests/test_arm_extern_capacity.py) passed against
the production compiler:

- 129 and 257 unused extern declarations precede referenced late declarations.
  Address-taking and data reads, including a 64-bit value, are linked with a
  GNU-generated definition object and executed in Unicorn; the result is 48.
- A private harness uses 270 extern declarations, verifies symbolic output for
  the final entry, and emits a second TU without that declaration. The second
  emission must fail with empty public output rather than retain stale state.

The real startup-profile object also passed its seven-symbol reference gate,
and the xhcc build produced all 140 requested objects. These compilation and
reference results are distinct from executing Reset through initialization in
the newly linked firmware. That runtime validation remains a separate gate;
this document does not claim that general naked C or complete firmware startup
has been verified by the extern tests.
