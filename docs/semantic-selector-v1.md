# Shared finite semantic selector

The shared selector in `src/backend/rules/semantic.c` consumes a value from the
existing XHTEMPLATE VM, so parsing, file loading and text expansion are not
reimplemented by each target. A semantic table is finite data rather than a
native callback or executable expression.

```
XHSEMANTIC 1
unary neg 32 v stack unary.neg.i32 1 1
memory load 8 u local semantic.load.u8 0 1
```

Eight fields are family, operation, width, sign, form, template key, pop count,
and push count. Exact tuple matching selects the template and effect. No wildcard
priority, control flow or expression evaluation exists. CSELECT1 and CSELECT 1
seven-field records are accepted with the form normalized to `-`, allowing the
C backend to use the same parser. Width and sign are normalized values supplied
by the IR adapter, not target-name pattern matching.

The VM allows at most 384 records in a 32 KiB value, copies parsed strings into
an allocation sized for the actual table, and keeps one target/table active.
Template generation invalidates the cache after reload/path/target changes;
steady-state selection does not copy or compare the whole source. Duplicate
normalized tuples, incorrect column counts, invalid key characters and effects
outside 0..16 fail parsing. A query returns 1 on match, 0 when absent and -1 on
malformed input. Target adapters decide whether an absent record is unsupported
or an explicit fallback, and must check the selected stack effect against the
normalized IR operation before emitting any sequence.

RV32 uses `semantic.records` for scalar32 unary, narrowing/extension casts,
local byte/halfword/word loads and stores, aligned/packed indexed memory forms,
frame address sign and indexed address scaling. The selected complete sequences
are emitted by the shared template VM. Native code still normalizes IR flags,
computes frame offsets, handles complex/64-bit cases not yet in the table and
implements ABI boundaries. Moving one subset does not imply a new ISA or ABI
can be invented by editing these records.

Tests change only a record's selected template from NEG to NOT, or signed cast
to unsigned cast, without modifying template bodies or compiler SHA. Linked
execution must change accordingly. Wrong effects and duplicate records must
fail. Existing memory, unary/cast, large frame, ABI and target regressions must
keep passing with the default table.
