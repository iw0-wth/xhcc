"""Bounded executable parity gates for the original --ark-compat interface.

The IPO/object-IR checks follow arkcc/tests/run_objir_tests.py; partial-link
identity follows run_partiallink_tests.py. These validate real output paths,
not just feature macros. No build or destination-ISA execution is performed.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from test_driver import ROOT, command


class LegacyParityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.command = command()
        cls.workspace = tempfile.TemporaryDirectory(prefix='legacy-', dir=ROOT)
        cls.directory = Path(cls.workspace.name)
        cls.relative = cls.directory.relative_to(ROOT).as_posix()
        (cls.directory / 'main.c').write_text(
            'extern int one(int);\nint main(void) { return one(9); }\n')
        (cls.directory / 'lib.c').write_text('int one(int x) { return x + 11; }\n')

    @classmethod
    def tearDownClass(cls):
        cls.workspace.cleanup()

    def path(self, name):
        return f'{self.relative}/{name}'

    def run_ark(self, *args, success=True):
        result = subprocess.run([*self.command, '--ark-compat', '-target', 'arm', *args],
                                cwd=ROOT, capture_output=True, text=True, timeout=60)
        diagnostics = result.stdout + result.stderr
        if success:
            self.assertEqual(result.returncode, 0, diagnostics)
        else:
            self.assertNotEqual(result.returncode, 0, diagnostics)
        return diagnostics

    def test_pipeline_reports_actual_enabled_stages(self):
        log = self.run_ark('--print-pipeline', '-O2', '--dce-cfg', '--licm', '--unroll',
                           '--tailmerge', '--xblock', '--const-global', '--warn-mem')
        for name in ('ast.memlint', 'ir.constfold', 'ir.dce', 'ir.inline',
                     'ir.vectorize', 'ir.dce-cfg', 'ir.licm', 'ir.unroll'):
            self.assertIn(name, log)
        self.assertNotIn('not compiled', log.lower())

    def test_ipo_and_object_ir_lto_same_assembly(self):
        for name in ('main', 'lib'):
            self.run_ark('-O2', '-c', '--obj-ir', '-o', self.path(name + '_ir.o'),
                         self.path(name + '.c'))
            payload = (self.directory / (name + '_ir.o')).read_bytes()
            self.assertIn(b'ARKIR1\n', payload)
        self.run_ark('-O2', '--ipo', '-S', '-o', self.path('ipo.s'),
                     self.path('main.c'), self.path('lib.c'))
        self.run_ark('-O2', '--lto', '-link', self.path('main_ir.o'), self.path('lib_ir.o'),
                     '-S', '-o', self.path('lto.s'))
        ipo = (self.directory / 'ipo.s').read_bytes()
        lto = (self.directory / 'lto.s').read_bytes()
        self.assertEqual(ipo, lto)
        text = ipo.decode('utf-8')
        self.assertNotRegex(text, r'\bBL\s+one\b')
        self.assertIn('ADD r0, r0, r1', text)
        self.assertIn('#11', text)

    def test_partial_link_reproduces_direct_image(self):
        for name in ('main', 'lib'):
            self.run_ark('-O1', '-c', '-o', self.path(name + '.o'), self.path(name + '.c'))
        self.run_ark('-r', '-link', self.path('lib.o'), self.path('main.o'),
                     '-o', self.path('merged.o'))
        self.assertTrue((self.directory / 'merged.o').read_bytes().startswith(b'ARKOBJ'))
        self.run_ark('-link', self.path('lib.o'), self.path('main.o'), '--vector-table',
                     '-o', self.path('direct.bin'))
        self.run_ark('-link', self.path('merged.o'), '--vector-table',
                     '-o', self.path('merged.bin'))
        self.assertEqual((self.directory / 'direct.bin').read_bytes(),
                         (self.directory / 'merged.bin').read_bytes())

    def test_real_memory_lint_diagnostic(self):
        source = self.directory / 'lint.c'
        source.write_text('int *bad(void) { int local = 1; return &local; }\n')
        log = self.run_ark('-O0', '-S', '--warn-mem', '-o', self.path('lint.s'),
                           self.path('lint.c'))
        self.assertTrue('local' in log.lower() or 'automatic' in log.lower(), log)
        self.assertIn('warning', log.lower())

    def test_scatter_parse_changes_real_image_layout(self):
        scatter = self.directory / 'app.sct'
        scatter.write_text('LR_IROM1 0x08010000 0x10000 {\n'
                           ' ER_IROM1 0x08010000 0x10000 { *.o (RESET, +First) .ANY (+RO) }\n'
                           ' RW_IRAM1 0x20000000 0x10000 { .ANY (+RW +ZI) }\n}\n')
        (self.directory / 'reset.c').write_text('void Reset_Handler(void) { for (;;) {} }\n')
        self.run_ark('-O1', '--vector-table', '--scatter', self.path('app.sct'),
                     '-o', self.path('scatter.bin'), self.path('reset.c'))
        image = (self.directory / 'scatter.bin').read_bytes()
        self.assertGreater(len(image), 8)
        reset = int.from_bytes(image[4:8], 'little')
        self.assertGreaterEqual(reset & ~1, 0x08010000)
        self.assertLess(reset & ~1, 0x08020000)

    def test_bound_asm_ir_roundtrip_and_corrupt_metadata_rejected(self):
        source = self.directory / 'asm_ir.c'
        source.write_text('int asm_wire(int x) { int out; __asm__ volatile('
                          '"ADD %0, %1, #3" : "=r" (out) : "r" (x) : "cc"); '
                          'return out; }\n')
        self.run_ark('-O0', '-c', '--obj-ir', '-o', self.path('asm_ir.o'), self.path('asm_ir.c'))
        self.run_ark('-O0', '--ipo', '-S', '-o', self.path('asm_direct.s'), self.path('asm_ir.c'))
        self.run_ark('-O0', '--lto', '-link', self.path('asm_ir.o'), '-S', '-o', self.path('asm_lto.s'))
        self.assertEqual((self.directory / 'asm_direct.s').read_bytes(),
                         (self.directory / 'asm_lto.s').read_bytes())
        data = bytearray((self.directory / 'asm_ir.o').read_bytes())
        ir_start = data.index(b'ARKIR1\n')
        template = b'ADD %0, %1, #3'
        pos = data.index(template, ir_start) + len(template)
        self.assertEqual(int.from_bytes(data[pos:pos+4], 'little'), 0x40000001)
        data[pos:pos+4] = (0x40000002).to_bytes(4, 'little')
        (self.directory / 'bad_asm_ir.o').write_bytes(data)
        log = self.run_ark('-O0', '--lto', '-link', self.path('bad_asm_ir.o'),
                           '-S', '-o', self.path('bad_asm.s'), success=False)
        self.assertIn('invalid output metadata', log)

    def test_lto_refuses_objects_without_ir(self):
        self.run_ark('-O1', '-c', '-o', self.path('plain.o'), self.path('lib.c'))
        log = self.run_ark('--lto', '-link', self.path('plain.o'),
                           '-o', self.path('invalid.bin'), success=False)
        self.assertTrue('ir' in log.lower() or 'payload' in log.lower(), log)


if __name__ == '__main__':
    unittest.main()
