# ARM assembly string layout contract

ARM assembly layout prediction and byte emission now share one string decoder.
The decoder is used by both passes for `.ascii` and `.asciz`, so decoded byte
counts and emitted bytes follow the same contract. The first pass includes the
NUL for `.asciz` and then aligns the absolute output position to two bytes,
matching the byte writer. Labels following strings therefore use the same
positions during planning and emission.

## Compatibility boundary

This change preserves the established byte-emission behavior rather than
introducing a new full GNU assembler string language:

- `\n`, `\t`, `\r`, escaped quote and escaped backslash retain their values.
- `\0` produces a zero byte; full multi-digit octal decoding is not implemented.
- `\x` consumes at most two hexadecimal digits. The existing empty-hex behavior
  and unknown escaped-character behavior are retained.
- A terminal backslash, missing opening quote or unterminated string is now an
  explicit assembly error. Planning fails before output publication rather
  than guessing a length or reading beyond the source.

A decoded NUL is data, not the end of the assembly source string. `.ascii` does
not append a NUL; `.asciz` does. Both retain this backend's existing two-byte
alignment behavior. This is a supported subset, not a claim that all GNU string
syntax or padding policies are implemented.

## Failure evidence

A sealed pre-fix `pyexec.o` had a local `.L_arkcc_mcpy_done` symbol value of
5668 while its `.text` size was 5658. That symbol was genuinely outside the
section, not merely equal to its end. The linker correctly rejected it;
neither symbol clamping nor relaxed linker range validation is a fix.

An independent private probe of assembly generated from the same translation
unit identified the first layout divergence at `.asciz "\x04"`. The old
planning pass counted more characters than the emitting pass decoded: the
following string label was planned at 16 but emitted at 14. Repeated affected
strings accumulated a 14-byte difference in that listing probe.

The private listing and the sealed object's section layout are not identical
representations. The probe's cumulative 14-byte difference must not be reported
as an exact explanation of every offset in the sealed object's 10-byte excess.
It identifies the shared underlying decoder mismatch. The original object and
its metadata remain evidence; they were not overwritten by the private probe.

## Validation

[The string-layout test](../tests/test_arm_string_layout.py) uses the production
assembler implementation in an isolated harness. A minimal GNU assembler oracle
checks the decoded bytes and label sequence `0, 2, 4, 12, 14`. Additional adjacent
hex strings, alignment and a branch across string data are compared, and the
resulting code executes with result 42. Exact-capacity and one-byte-short output
cases check surrounding canaries. Invalid terminal escapes and unterminated
strings must fail without published symbols or writes.

The test chooses explicit equal-width branch and move instructions for byte
comparison. This is important: GNU `LDR =symbol` may use a PC-relative literal
pool, while this backend's established pseudo-instruction lowering uses a
MOVW/MOVT pair. GNU's branch/move default width and code-alignment fill can also
differ. A whole generated listing is therefore not automatically a byte-for-
byte GNU oracle without accounting for those pseudo-instruction contracts.

The shared decoded count also feeds the existing machine-code capacity
preflight. It avoids rejecting a sufficient buffer because of an inflated
string prediction. Capacity checks and failure semantics are not relaxed.

The isolated test passed after this fix. Host attempt 9 then passed the fresh
real `pyexec` bound gate: `.L_arkcc_mcpy_done` is now 5654 in a `.text` section
of 5658 bytes, and the object's allocated-section symbols have no overrun. The
section size remains 5658, as in the sealed pre-fix object; the corrected label
planning did not require enlarging or clamping the section.

The full 139-object bound audit and project link remain separate subsequent
gates. Their results are not pre-claimed by the passing `pyexec` gate.
