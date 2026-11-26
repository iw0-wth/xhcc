# Linker script load addresses and ignored metadata declarations

The bounded linker-script subset supports an explicit load address in an output
section header:

```ld
MEMORY {
    FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 4K
    RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 4K
}
SECTIONS {
    .text : {
        *(.text*)
        . = ALIGN(4);
        image_data = . + 16;
    } > FLASH
    .data : AT(image_data) {
        *(.data*)
    } > RAM
    .bss : { *(.bss*) } > RAM
    .metadata 0 : { *(.ARM.attributes) }
}
```

## Header syntax and evaluation order

`AT(expr)` belongs between the section header's colon and opening brace.
Header `ALIGN(...)` and `AT(...)` may occur in either relative order, each at
most once. Trailing `AT>region` keeps its existing meaning and cannot be
combined with header `AT(expr)`. Duplicate AT specifications are rejected.
Trailing `AT(expr)` remains unsupported and is rejected.

The parser stores the expression in `XhLdSection.load_address_expr`, initialized
to `XH_LD_NONE` when absent. Layout processes earlier section bodies before
computing the current section's load address. A symbol assigned at the end of
an earlier section therefore observes that earlier section's captured location
counter. No particular symbol name or script path receives special treatment.

The expression's `.` is the current section's aligned VMA start, after the
section's explicit address, header alignment and input alignment have been
applied. The resulting LMA is exact; it is not silently rounded to input
alignment. Earlier completed sections can supply `ADDR`, `LOADADDR` and `SIZEOF`
values through the normal expression lookup. Expressions use the existing
checked unsigned 32-bit arithmetic rules; unresolved, cyclic and invalid
expressions fail rather than supplying zero.

## Address bounds and output

An explicit load address may lie in any declared MEMORY region, including a
writable RAM region. Region attributes are not used to guess a FLASH region.
The entire initialized file span must fit inside one declared region. A start
outside all regions, a span crossing a region boundary, an overflowing address
or overlapping initialized LMA spans is rejected. This is a deliberate bounded
subset constraint: GNU ld can accept an explicit `AT(expr)` outside every
MEMORY region.

Existing VMA bounds, input alignment and section-overlap checks remain active.
`AT>region` continues to use that region's aligned cursor. With an explicit LMA,
a matching load region's cursor advances when it differs from the VMA region.
The section's VMA region continues to account for its memory extent separately.

Binary output places initialized inputs at offsets from the lowest initialized
LMA, so physical address order can differ from script order. Gaps are zero
filled. NOBITS/BSS inputs contribute no bytes to the binary. ELF PT_LOAD headers
use the section VMA for `p_vaddr` and LMA for `p_paddr`; startup code is responsible
for copying initialized RAM data and clearing BSS as required by the target.

## Regionless metadata declarations

A regionless output declaration is accepted only with an explicit constant zero
address, pure input-pattern events and no section type, load-address or alignment
modifier. KEEP-wrapped patterns are permitted. Symbol assignments, location
counter assignments, ASSERT events and COMMON patterns are rejected in such a
declaration. The zero expression cannot depend on `.`, symbols or section
addresses.

Layout checks every actual input matched by these patterns, including inputs
already claimed elsewhere. Each match must be nonalloc and have ELF type
`SHT_ARM_ATTRIBUTES` (`0x70000003`). Matching an alloc input or a different kind
of nonalloc input fails. Neither output-section names nor input-section names
are hardcoded; acceptance depends on the actual input flags and type. Empty
matches are allowed as a harmless declaration for portable scripts.

These are checked declarations of ignored metadata. They do not claim inputs,
advance the location counter or MEMORY cursors, contribute binary bytes or
create PT_LOAD entries. Input ARM attributes were already ignored by the link
writer; this support does not forward or merge them into the output ELF and does
not claim GNU attribute-merge equivalence.

## Entry selection

The entry selection order remains explicit CLI entry, then script `ENTRY(...)`,
then `_start`. There is no implicit first-global or target-handler fallback. A
script without ENTRY can be used with an explicit caller-selected entry such as
`--entry Reset_Handler`; no handler name is hardcoded into the linker. The entry
must still be a defined function satisfying the target's alignment and Thumb
requirements.

## Verification

Standalone source harness tests passed without rebuilding the production
compiler: seven load-address tests, eight parser tests, seven ARM ELF linker
tests and three RISC-V ELF linker tests (25 total). The harnesses compile the
current linker sources with undefined-behavior trap instrumentation.

The new GNU ARM assembler/ld/objcopy oracle fixture uses two MEMORY regions and
an earlier section-tail symbol consumed by header `AT(expr)`. It verifies an
initialized data LMA of `0x08000018`, the same ELF `p_paddr` as GNU ld, pointer
relocations to that LMA, a 16-byte zero-filled gap and a 32-byte binary that
excludes 12 bytes of BSS. It also checks that the metadata declaration adds no
fourth PT_LOAD and that the Thumb entry is `0x08000001`. Further cases exercise
RAM LMAs, physical order different from script order, header alignment order,
expression dot, duplicate AT, overflow, region bounds, overlap and invalid
metadata inputs.

A read-only acceptance check of the original STM32F405 minimal-port script
parsed all four output declarations, including header `AT(_sidata)` and the
regionless ARM attributes declaration. Its parsed IR occupied 28,604 bytes,
below the 32 KiB parser budget. Parsing and these fixtures do not establish that
a complete MicroPython firmware has linked or runs; full-project undefined
symbols, relocations and runtime behavior require separate integration evidence.
