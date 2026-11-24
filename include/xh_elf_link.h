/* Independently implemented bounded ARM ELF32 executable linker. */
#ifndef XH_ELF_LINK_H
#define XH_ELF_LINK_H
#include <stddef.h>
#include <stdint.h>
#define XH_ELF_LINK_MAX_OBJECTS 32u
#define XH_ELF_LINK_MAX_SECTIONS 512u
#define XH_ELF_LINK_MAX_SYMBOLS 8192u
#define XH_ELF_LINK_MAX_NAMES (128u*1024u)
#define XH_ELF_LINK_MAX_MEMORY (512u*1024u)
#define XH_ELF_LINK_MAX_OUTPUT (512u*1024u)
typedef struct {
    const char *output_path;
    const char *entry_name; /* NULL selects script ENTRY, then _start. */
    uint32_t text_addr; /* Zero selects 0x08000000. */
    uint32_t data_addr; /* Zero selects 0x20000000. */
    uint32_t data_lma; /* Zero places initialized RAM immediately after Flash. */
    size_t max_memory; /* Zero selects 512 KiB; includes all requested heap bytes. */
    const char *script_path; /* Optional bounded MEMORY/SECTIONS script. */
    unsigned output_format; /* 0: ELF; 1: flat binary in Flash LMA order. */
} XhElfLinkOptions;
/* stdout is rejected because it cannot preserve old output.
 * Limits above are aggregate; reader additionally limits each object to 256
 * sections. Inputs are read twice, one at a time. No arena or retained views.
 * Defined FUNC must carry Thumb bit, including ABS FUNC. NOTYPE mapping symbols
 * do not imply Thumb state: a branch target must resolve to a Thumb STT_FUNC.
 * Undefined strong symbols fail only when referenced by an alloc relocation or
 * selected as entry. Undefined weak data resolve to zero; weak branches fail.
 * Returns zero on success, -1 with a bounded diagnostic on failure. */
typedef struct {
    size_t peak_requested_bytes;
    unsigned objects,sections,symbols;
    size_t output_bytes;
} XhElfLinkReport;
/* Report is reset on entry and filled on success or failure. Peak counts the
 * core's requested heap bytes, including input, metadata, script and output;
 * it excludes allocator overhead, stack, filesystem internals and process RSS.
 * Counts describe fully scanned inputs; output_bytes is zero on failure. */
int xh_elf_link_arm_report(const char *const *inputs,unsigned n,
                    const XhElfLinkOptions *options,char *err,size_t errcap,
                    XhElfLinkReport *report);
int xh_elf_link_arm(const char *const *inputs,unsigned n,
                    const XhElfLinkOptions *options,char *err,size_t errcap);
#endif
