"""Standalone AT(expr)/checked ignored metadata tests; never rebuild production.

Fixtures are independently written. GNU permits LMAs outside MEMORY; this
bounded subset deliberately requires a complete span inside a declared region.
"""
from pathlib import Path
import struct
import subprocess
import unittest
import test_elf_link as core

ROOT = core.ROOT
OUT = ROOT / 'build/ld-load-address-tests'
SCRIPT = '''ENTRY(Reset_Handler)
MEMORY { FLASH(rx) : ORIGIN = 0x08000000, LENGTH = 4K
 RAM(rwx) : ORIGIN = 0x20000000, LENGTH = 4K }
SECTIONS {
 .text : { *(.text*) . = ALIGN(4); _etext = .; _sidata = _etext + 16; } > FLASH
 .data : AT(_sidata) { _sdata = .; *(.data*) _edata = .; } > RAM
 .bss : { *(.bss*) } > RAM
 .metadata 0 : { *(.ARM.attributes) }
}
'''
ASM = '''.syntax unified
.thumb
.section .text.entry,"ax"
.global Reset_Handler
.thumb_func
Reset_Handler: bx lr
.balign 4
.word _sidata
.section .data.value,"aw"
.word 0x12345678
.word _sidata
.section .bss.zero,"aw",%nobits
.space 12
'''


class LoadAddressTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        (OUT / 'harness.c').write_text(core.HARNESS, encoding='utf-8')
        cls.cmd([core.SDK / ('bin/clang' + core.EXE), '--target=wasm32-wasip1',
                 '-std=c99', '-O1', '-Wall', '-Wextra', '-Werror',
                 '-fsanitize=undefined', '-fsanitize-trap=all', '-Iinclude',
                 '-include', 'include/xh_small.h', OUT / 'harness.c',
                 'src/link/elf_link.c', 'src/link/elf_read.c',
                 'src/link/arm_elf_reloc.c', 'src/link/rv_elf_reloc.c',
                 'src/link/ld_script.c', 'src/backend/rules/relocation.c',
                 '-o', OUT / 'link.wasm'])
        (OUT / 'input.s').write_text(ASM, encoding='ascii')
        cls.cmd([core.GNU / ('arm-none-eabi-as' + core.EXE), '-mcpu=cortex-m3',
                 '-mthumb', OUT / 'input.s', '-o', OUT / 'input.o'])

    @staticmethod
    def cmd(args, ok=True):
        p = subprocess.run(list(map(str, args)), cwd=ROOT, capture_output=True,
                           text=True, timeout=120)
        if (p.returncode == 0) != ok:
            raise AssertionError(p.stdout + p.stderr)
        return p

    def link(self, script=SCRIPT, binary=False, ok=True):
        path = OUT / 'input.ld'
        path.write_text(script, encoding='ascii')
        output = OUT / ('output.bin' if binary else 'output.elf')
        p = self.cmd([core.WASMTIME, 'run', '--dir=.', '--env',
                      'XH_TEST_LINK_SCRIPT=' + path.relative_to(ROOT).as_posix(),
                      (OUT / 'link.wasm').relative_to(ROOT), output.relative_to(ROOT), '@script', '0', '0', '0', '0',
                      str(int(binary)), (OUT / 'input.o').relative_to(ROOT)], ok)
        if not ok:
            self.assertNotIn('wasm trap', p.stderr)
        return output, p

    def test_gnu_oracle_binary_relocations_bss_and_paddr(self):
        output, _ = self.link()
        elf = core.elf_bytes(output)
        segments = list(elf.iter_segments())
        self.assertEqual(len(segments), 3)  # metadata is not PT_LOAD
        data = next(s for s in segments if s['p_vaddr'] == 0x20000000)
        self.assertEqual(data['p_paddr'], 0x08000018)
        self.assertEqual(data.data(), struct.pack('<II', 0x12345678, 0x08000018))
        bss = next(s for s in segments if s['p_vaddr'] == 0x20000008)
        self.assertEqual((bss['p_filesz'], bss['p_memsz']), (0, 12))
        self.assertEqual(elf['e_entry'], 0x08000001)
        binary, _ = self.link(binary=True)
        actual = binary.read_bytes()
        self.assertEqual(len(actual), 32)
        self.assertEqual(actual[4:8], struct.pack('<I', 0x08000018))
        self.assertEqual(actual[8:24], bytes(16))
        self.cmd([core.GNU / ('arm-none-eabi-ld' + core.EXE), '-T', OUT / 'input.ld',
                  OUT / 'input.o', '-o', OUT / 'gnu.elf'])
        self.cmd([core.GNU / ('arm-none-eabi-objcopy' + core.EXE), '-O', 'binary',
                  OUT / 'gnu.elf', OUT / 'gnu.bin'])
        self.assertEqual(actual, (OUT / 'gnu.bin').read_bytes())
        oracle = core.elf_bytes(OUT / 'gnu.elf')
        gd = next(s for s in oracle.iter_segments() if s['p_vaddr'] == 0x20000000)
        self.assertEqual(data['p_paddr'], gd['p_paddr'])

    def test_ram_load_address_and_out_of_order_lma(self):
        script = SCRIPT.replace('AT(_sidata)', 'AT(ORIGIN(RAM)+128)')
        output, _ = self.link(script)
        data = next(s for s in core.elf_bytes(output).iter_segments()
                    if s['p_vaddr'] == 0x20000000)
        self.assertEqual(data['p_paddr'], 0x20000080)
        # Script order differs from physical order; flat placement uses LMA.
        script = SCRIPT.replace('AT(_sidata)', 'AT(ORIGIN(FLASH))').replace(
            '.text :', '.text ORIGIN(FLASH)+64 :')
        output, _ = self.link(script, binary=True)
        self.assertEqual(output.read_bytes()[:4], struct.pack('<I', 0x12345678))

    def test_rejections(self):
        variants = [
            SCRIPT.replace('AT(_sidata)', 'AT(_sidata) AT(_sidata)'),
            SCRIPT.replace('} > RAM\n .bss', '} > RAM AT> FLASH\n .bss'),
            SCRIPT.replace('AT(_sidata)', 'AT(0xffffffff+1)'),
            SCRIPT.replace('AT(_sidata)', 'AT(0xfffffffc)'),
            SCRIPT.replace('AT(_sidata)', 'AT(0x09000000)'),
            SCRIPT.replace('AT(_sidata)', 'AT(ORIGIN(FLASH)+LENGTH(FLASH)-4)'),
            SCRIPT.replace('AT(_sidata)', 'AT(ORIGIN(FLASH))'),
            SCRIPT.replace('*(.ARM.attributes)', '*(.text*)'),
            SCRIPT.replace('*(.ARM.attributes)', '*(.strtab)'),
            SCRIPT.replace('.metadata 0', '.metadata .'),
            SCRIPT.replace('.metadata 0', '.metadata 1'),
            SCRIPT.replace('*(.ARM.attributes)', 'x=0; *(.ARM.attributes)'),
            SCRIPT.replace('*(.ARM.attributes)', '.=0; *(.ARM.attributes)'),
            SCRIPT.replace('*(.ARM.attributes)', 'ASSERT(1,"x") *(.ARM.attributes)'),
            SCRIPT.replace('.metadata 0 :', '.metadata 0 : AT(0)'),
            SCRIPT.replace('.metadata 0 :', '.metadata 0 (NOLOAD) :'),
        ]
        for script in variants:
            with self.subTest(script=script):
                self.link(script, ok=False)

    def test_existing_at_region_and_empty_metadata(self):
        self.link(SCRIPT.replace(': AT(_sidata)', ':').replace(
            '} > RAM\n .bss', '} > RAM AT> FLASH\n .bss'))
        self.link(SCRIPT.replace('*(.ARM.attributes)', '*(.missing.attributes)'))

    def test_header_alignment_and_dot(self):
        for header in ['ALIGN(16) AT(_sidata)', 'AT(_sidata) ALIGN(16)']:
            self.link(SCRIPT.replace('AT(_sidata)', header))
        output, _ = self.link(SCRIPT.replace('AT(_sidata)', 'AT(.)'))
        data = next(s for s in core.elf_bytes(output).iter_segments()
                    if s['p_vaddr'] == 0x20000000)
        self.assertEqual(data['p_paddr'], data['p_vaddr'])

    def test_metadata_spoofed_alloc_flags(self):
        path = OUT / 'input.o'
        original = path.read_bytes()
        elf = core.elf_bytes(path)
        index = next(i for i, s in enumerate(elf.iter_sections())
                     if s.name == '.ARM.attributes')
        modified = bytearray(original)
        struct.pack_into('<I', modified, elf['e_shoff'] + index * elf['e_shentsize'] + 8, 2)
        try:
            path.write_bytes(modified)
            _, p = self.link(ok=False)
            self.assertIn('unsupported alloc section flags/type', p.stderr)
        finally:
            path.write_bytes(original)

    def test_original_structure_no_entry_override(self):
        script = SCRIPT.replace('ENTRY(Reset_Handler)\n', '').replace(
            '_sidata = _etext + 16', '_sidata = _etext')
        _, p = self.link(script, ok=False)
        self.assertIn('entry symbol not found: _start', p.stderr)
        # Explicit caller entry, not a special script symbol/path rule.
        path = OUT / 'input.ld'
        self.cmd([core.WASMTIME, 'run', '--dir=.', '--env',
                  'XH_TEST_LINK_SCRIPT=' + path.relative_to(ROOT).as_posix(),
                  (OUT / 'link.wasm').relative_to(ROOT), (OUT / 'explicit.elf').relative_to(ROOT), 'Reset_Handler',
                  '0', '0', '0', '0', '0', (OUT / 'input.o').relative_to(ROOT)])


if __name__ == '__main__':
    unittest.main()
