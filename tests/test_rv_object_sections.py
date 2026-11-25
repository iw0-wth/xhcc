"""Inspect production RV32 object sections; deliberately never builds the app."""
from pathlib import Path
import io
import os
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'build/test-deps'))
from elftools.elf.elffile import ELFFile

COMPILER = Path(os.environ.get('XHCC_WASM', str(ROOT / 'build/wasm/xhcc.wasm')))
WASMTIME = os.environ.get('WASMTIME', r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
SOURCE = r'''
extern int external;
int data_value = 0x12345678;
int zero_explicit = 0;
int bss_value;
static int local_value = 9;
int *pointer = &external;
int *local_pointer = &local_value;
char *message = "hello";
struct Pair { int *p; int n; } aggregate = { &data_value, 17 };
int aligned_value __attribute__((aligned(16))) = 3;
int custom_value __attribute__((section(".settings"))) = 5;
static int helper(int x) { return x + local_value; }
int entry(void) { bss_value = 7; return helper(*message) + *local_pointer + data_value; }
'''

class RvObjectSections(unittest.TestCase):
    def test_real_sections_and_relocation_sites(self):
        (ROOT / 'build').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='rv-object-sections-', dir=ROOT / 'build') as tmp:
            out = Path(tmp)
            source = out / 'sections.c'
            obj = out / 'sections.o'
            source.write_text(SOURCE, encoding='utf-8')
            result = subprocess.run([
                sys.executable, 'tools/xhcc.py', '--compiler', str(COMPILER),
                '--wasmtime', WASMTIME, 'riscv32-unknown-elf-gcc', '-O0',
                '-c', str(source.relative_to(ROOT)), '-o', str(obj.relative_to(ROOT))
            ], cwd=ROOT, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            elf = ELFFile(io.BytesIO(obj.read_bytes()))
            self.assertEqual(elf['e_machine'], 'EM_RISCV')
            self.assertEqual(elf['e_type'], 'ET_REL')
            self.assertEqual(elf['e_flags'], 0)
            text, data, bss = [elf.get_section_by_name(n) for n in ('.text', '.data', '.bss')]
            self.assertEqual((text['sh_type'], text['sh_flags']), ('SHT_PROGBITS', 6))
            self.assertEqual((data['sh_type'], data['sh_flags']), ('SHT_PROGBITS', 3))
            self.assertEqual((bss['sh_type'], bss['sh_flags']), ('SHT_NOBITS', 3))
            syms = {s.name: s for s in elf.get_section_by_name('.symtab').iter_symbols()}
            def section(name):
                return elf.get_section(syms[name]['st_shndx'])
            for name in ('data_value', 'zero_explicit', 'local_value', 'pointer', 'local_pointer', 'message', 'aggregate', 'aligned_value'):
                self.assertEqual(section(name).name, '.data', name)
            self.assertEqual(section('bss_value').name, '.bss')
            self.assertEqual(section('custom_value').name, '.settings')
            self.assertEqual(section('helper').name, '.text')
            self.assertEqual(syms['helper']['st_info']['bind'], 'STB_LOCAL')
            self.assertEqual(syms['local_value']['st_info']['bind'], 'STB_LOCAL')
            self.assertEqual(syms['external']['st_shndx'], 'SHN_UNDEF')
            self.assertEqual(syms['aligned_value']['st_value'] % 16, 0)
            self.assertGreaterEqual(data['sh_addralign'], 16)
            self.assertEqual(struct.unpack_from('<I', data.data(), syms['data_value']['st_value'])[0], 0x12345678)
            self.assertIn(b'hello\0', elf.get_section_by_name('.rodata').data())
            rel = elf.get_section_by_name('.rela.data')
            self.assertEqual(elf.get_section(rel['sh_info']).name, '.data')
            targets = {}
            symtab = elf.get_section(rel['sh_link'])
            for r in rel.iter_relocations():
                self.assertEqual(r['r_info_type'], 1)  # R_RISCV_32
                self.assertLess(r['r_offset'] + 3, data['sh_size'])
                targets[r['r_offset']] = symtab.get_symbol(r['r_info_sym']).name
            self.assertEqual(targets[syms['pointer']['st_value']], 'external')
            self.assertEqual(targets[syms['local_pointer']['st_value']], 'local_value')
            self.assertEqual(targets[syms['aggregate']['st_value']], 'data_value')
            for rsec in elf.iter_sections():
                if rsec['sh_type'] != 'SHT_RELA':
                    continue
                site = elf.get_section(rsec['sh_info'])
                for r in rsec.iter_relocations():
                    self.assertLess(r['r_offset'] + 3, site['sh_size'])
            for sec in elf.iter_sections():
                self.assertNotEqual(sec['sh_flags'] & 7, 7, sec.name)

if __name__ == '__main__':
    unittest.main()
