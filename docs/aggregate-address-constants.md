# Aggregate address constants

Aggregate initializers can contain certified symbol addresses and narrow string literals, including a narrow string literal wrapped in one or more pointer casts. For example:

```c
extern int anchor;
extern void handler(void);
struct Record {
    int *base;
    unsigned int hash, length;
    const unsigned char *data;
};
const struct Record record = {
    &anchor, 0, 6, (const unsigned char *)(const char *)"a,]b\\c"
};
/* ARM ILP32: unsigned int has the same width as a pointer. */
const unsigned int vector[] = {
    (unsigned int)&anchor, (unsigned int)&handler, 0
};
```

The IR initializer walk records each address and its byte offset. The aggregate byte filler reserves an address slot only when the backend hook certifies that exact offset. Pointer-width integer fields may use the same certification path as pointer fields. A nonconstant expression or an unrecorded address does not gain acceptance through this path.

Integer storage must match the selected target's pointer width: a 32-bit integer qualifies on a 32-bit pointer target, while a 64-bit integer qualifies on a 64-bit pointer target. This rule does not allow narrower integer fields, floating fields, or bitfields to receive an address relocation. Backend relocation support remains target specific.

The string collector strips pointer casts only while recognizing a string literal. Other address expressions retain their original casts so that address arithmetic keeps its original stride. Character arrays initialized by a string continue to receive inline character bytes rather than a pooled pointer. Wide string literals remain unsupported by this aggregate address pooling path, including when wrapped in a cast to a narrow pointer type.

## Evidence and validation

The full original MicroPython run identified two distinct initializer failures hidden by the ARM ELF writer's generic error. In `mp_sys_version_obj`, a cast wrapped the narrow string AST, so the IR collector omitted the data pointer's string reference at offset 12. In `isr_vector`, all eleven symbol references existed, but the aggregate filler rejected the integer slot before consulting the certification hook. Both reached aggregate filling rather than exhausting memory.

An isolated module retained the original 4 MiB arena / 16 MiB memory budget and compiled the actual `modsys.c` and `main.c` with the original flags after the fixes. Both exited successfully, with arena peaks of 1,897,352 and 1,960,744 bytes. These private outputs were written outside the MicroPython project. They establish the two fixes independently of a subsequent full host build.

`tests/test_aggregate_addresses.py` adds a source harness for 4-byte and 8-byte pointer models, rejects mismatched integer widths and floating storage, and compares ARM ELF initializer bytes and relocation offsets/types with GNU ARM. The string check includes embedded comma, closing bracket, backslash, and terminating NUL bytes. The GNU comparison requires two relocations in the sample structure and three in the sample vector.

The rebuilt expanded production module's invocation of this test script passed six discovered tests. Only two are new tests from this script: four existing aggregate constexpr tests were also discovered because the module imports their test classes. This count must not be described as six newly added tests. That production oracle success is separate from the pending formal host retry of the failed translation units; the latter must be reported from its own logs.

Audit command logs, private source copies, generated preprocessing evidence and generated-header baseline hashes are kept under `build/remaining-audit/`. No MicroPython source changes were required.
