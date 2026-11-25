# RV32 standard ELF object sections

The RV32 `-c` path writes ELF32 little-endian ET_REL objects for the
RV32IM/ILP32 profile (machine 243, ELF flags zero). The serializer consumes the
real assembler label, section and relocation tables. Initializer bytes come
from the assembled image; the writer does not parse or regenerate source
initializers. Typed IR supplies each global object's extent, initialization
state, alignment and requested section.

The RV-specific implementation is in [rv_elf_sections.inc](../object/rv_elf_sections.inc),
selected by [target_elf.inc](../object/target_elf.inc) when included by the RV
backend. This selection uses the backend's existing `RV_SEC_COUNT` macro.
The shared x86 serializer keeps its existing behavior.

| Output section | ELF type | Flags | Contents |
| --- | --- | --- | --- |
| `.text` | PROGBITS | ALLOC, EXECINSTR | Original assembler text bytes |
| `.rodata` | PROGBITS | ALLOC | Original assembler read-only constants and strings |
| `.data` | PROGBITS | ALLOC, WRITE | Globals with initializers, including explicit zero initializers |
| `.bss` | NOBITS | ALLOC, WRITE | Globals without initializers |
| Custom data sections | PROGBITS | ALLOC, WRITE | Globals assigned to those sections |
| `.init_array` / `.fini_array` | INIT_ARRAY / FINI_ARRAY | ALLOC, WRITE | Explicit array objects and constructor/destructor pointers |

Text and read-only constants retain their assembler section offsets. Keeping
text intact preserves assembler-resolved local branch distances and text
constants. Globals move from the assembler's data storage into their output
sections; BSS occupies memory without storing its zero bytes in the object.
The writer translates original image offsets into output section numbers and
section-relative offsets for symbols and relocation sites. It preserves local
bindings for static functions and static globals, and leaves external symbols
undefined.

Absolute pointer initializers use `R_RISCV_32` with explicit RELA addends. Their
relocations belong to `.rela.data` or the actual custom destination section.
AUIPC pairs use `R_RISCV_PCREL_HI20` and `R_RISCV_PCREL_LO12_I`; the low relocation
names a local anchor at the high instruction's output section and offset.
Each relocation section's `sh_info` identifies the section containing its
sites. The writer rejects invalid or cross-section relocation words and
relocations into NOBITS storage.

The current assembler accepts power-of-two alignment values from 1 through 16.
Global emission has a minimum alignment of 4, honors supported explicit
alignment, and propagates the required alignment to its ELF section. An
`aligned(32)` request fails during assembly before the ELF writer runs.
TLS, absolute data placement and aliases remain unsupported. Custom globals
requesting `.text`, `.rodata` or `.bss` are rejected; writable custom section
names, `.data`, `.init_array` and `.fini_array` are supported. Linker scripts must
place the allocated custom sections or the linker can reject the unmatched
section.

Validation uses the existing production Wasm compiler and never builds the
application from the tests themselves:

```sh
python tests/test_rv_object_sections.py
python tests/test_rv_elf_link.py
```

The object test inspects section types and permissions, initialized values,
NOBITS storage, local bindings, constants, supported explicit alignment,
custom data and pointer relocation sites. The mixed-object integration tests
link production XH RV objects with external Clang objects, check data/BSS and
function pointers, and execute two independent memory layouts under Unicorn.
The tested mixed entry returns 149 in both layouts. The standard linker keeps
its strict rejection of writable executable input sections.
