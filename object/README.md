# ARM ELF object output

With `XHCC` defined, `compiler.c` sets `g_xhcc_elf_mode` from `!ArkOptions.private_object`. The default compile-only output calls `xhcc_arm_write_elf(path, code, length)`. `--private-object` retains the original ARKOBJ2 emitter and fixed-image backend behavior.

`arm_elf.inc` is included by the ARM backend (not a separately compiled source). It reads the actual assembler label/reference tables and the translation unit IR. It does not translate or encapsulate the lossy ARKOBJ2 symbol table.

Objects are little-endian ARM ELF32 ET_REL, EABI5 with ARMv7-M / Thumb-2 attributes, real `.data` PROGBITS and `.bss` NOBITS, local/static/global/weak symbol bindings, Thumb function values, `.rel.text` and `.rel.data`. Supported relocations are R_ARM_ABS32, R_ARM_THM_CALL, R_ARM_THM_JUMP24/JUMP19, and R_ARM_THM_MOVW_ABS_NC / R_ARM_THM_MOVT_ABS. Internal local branches resolve within the text section. Named calls remain relocatable even when the target is defined in the same unit, so a GNU strong definition can override a weak definition.

Initialized scalars, arrays, aggregate byte data, address constants with addends, string pointers and function pointers become actual data and ABS32 relocations. The ELF path omits the fixed SRAM startup initializer. The final GNU linker script and target startup own placement, copying `.data`, and zeroing `.bss`. The bundled memcpy helper is weak. Floating-point operations use base AAPCS soft-float helpers from a target runtime such as GNU libgcc.

Scratch memory is allocated from the compiler arena in proportion to labels, references, globals and output bytes. No new large static buffer is introduced.

Current limitations:

- Code and read-only literals share the allocated executable `.text` section; `.rodata` is not separated.
- ELF TLS and function aliases are rejected. ARM custom data sections and constructor/destructor arrays are supported and tested.
- No generated unwind or debug sections, COMDAT groups, common symbols, or per-function sections.
- The public driver accepts C input only. Raw assembly has no supported IR data-layout contract.
- GNU assembly output uses `arm_listing.inc` to emit actual IR-derived data definitions and address expressions. It must be verified with GNU as/link after changes to code generation.
- Explicit private mode is legacy ARKOBJ2; GNU linkers cannot consume it.

## External object descriptor v1

Each machine package selects an `object.rules` descriptor through
`xh_object_rules_set_path(target,path)`. The bounded shared reader accepts
`XHOBJECT 1`, target name, class (32/64), machine, little endianness, flags,
ABI name, attribute bytes as hex (or `-`), and up to 64 `relmap INTERNAL ELF`
records. Files are limited to 8192 bytes and attributes to 128 bytes. There is
one active allocated descriptor, with at most eight copied path registrations.

The internal relocation keys retain their historical numeric identifiers from
the existing emitters. They are independent of the ELF wire types emitted by
the writer: every relocation must have an external mapping. For example,
`relmap 10 10` maps the historical ARM call key to R_ARM_THM_CALL. A modified
mapping changes the actual relocation section without rebuilding the compiler.
Changing a wire type alone may deliberately make linking invalid; object tests
inspect that change and separately execute the correct default descriptors.

Serializer section layout, addend preparation, Thumb bits and relocation site
semantics remain shared/native algorithms. Rules do not redefine the ABI or
make an incompatible ELF class valid. Machine/class/ABI must match the selected
emitter; big-endian and malformed/duplicate mappings are rejected. Attributes
are length-checked AEABI byte records supplied by the package. Flags are copied
from the descriptor. Current ARM files declare EABI5/base AAPCS; x86/i386 and
RV32 descriptors declare their corresponding data models.

## Verification

From the xhcc root:

```text
python -m pip install --target build/test-deps unicorn pyelftools
python object/tests/test_elf.py
```

The test compiles C through the WASI module, compiles a second unit with GNU ARM GCC, links with GNU ARM ld and executes the linked Thumb instructions using Unicorn's Cortex-M mode. It tests two independent Flash/SRAM placements, calls in both toolchain directions, GNU strong-over-weak resolution, static functions and objects, initialized data and BSS, aggregate/external/string/function pointers, and soft-float calls through GNU libgcc. It checks ET_REL identity, local bindings, Thumb bits, relocation types, legacy private output and rejection of unsupported data sections.

Expected executed results at both placements: `xh_entry=115`, `gnu_entry=13`, `aggregate_entry=146`, `soft_entry=7`. Successful GNU link plus execution is required; merely finding an ELF magic header does not pass.
