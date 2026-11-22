"""Driver integration tests. Run after tools/build.py; no third-party modules.

XHCC_COMMAND may contain a JSON array (native executable or wasmtime invocation).
The default uses wasmtime with the project directory mounted as the guest cwd.
These tests spawn the executable for validation; the C driver never spawns tools.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def command():
    override = os.environ.get('XHCC_COMMAND')
    if override:
        return json.loads(override)
    runtime = shutil.which('wasmtime')
    if not runtime:
        candidate = Path(r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
        if candidate.exists():
            runtime = str(candidate)
    if not runtime:
        raise RuntimeError('wasmtime not found; set XHCC_COMMAND to a JSON argv array')
    return [runtime, 'run', '--dir=.', str(ROOT / 'build/wasm/xhcc.wasm')]


class DriverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.command = command()
        cls.workspace = tempfile.TemporaryDirectory(prefix='driver-', dir=ROOT)
        cls.dir = Path(cls.workspace.name)
        cls.relative = cls.dir.relative_to(ROOT).as_posix()
        (cls.dir / 'one.c').write_text('int one(void) { return 1; }\n')
        (cls.dir / 'two.c').write_text('int two(void) { return 2; }\n')
        (cls.dir / 'forced.h').write_text('#define FORCED 17\n')
        (cls.dir / 'macros.c').write_text(
            '#if defined(X) || !defined(Y) || Y != 7 || FORCED != 17\n'
            '#error invalid command macro/include order\n#endif\n'
            'int macro_result(void) { return Y + FORCED; }\n')

    @classmethod
    def tearDownClass(cls):
        cls.workspace.cleanup()

    def path(self, name):
        return f'{self.relative}/{name}'

    def run_xhcc(self, *args, success=True):
        result = subprocess.run([*self.command, *args], cwd=ROOT,
                                capture_output=True, text=True, timeout=30)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def test_help_version_without_packages(self):
        self.assertIn('Cortex-M3', self.run_xhcc('--package-root', 'missing-package', '--help').stdout)
        self.assertIn('xhcc', self.run_xhcc('--version').stdout)

    def test_reject_options_and_missing_values(self):
        for flag in ['-O2', '-marm', '-mfloat-abi=hard', '-mcpu=cortex-m4',
                     '-std=c11', '-Wunknown', '-funknown']:
            with self.subTest(flag=flag):
                self.run_xhcc('-c', self.path('one.c'), flag, success=False)
        self.run_xhcc('-c', self.path('one.c'), '-I', success=False)
        self.run_xhcc(self.path('one.c'), success=False)
        self.run_xhcc('-c', self.path('one.c'), '-MF', self.path('bad.d'), success=False)

    def test_response_cycle_quotes_and_limits(self):
        (self.dir / 'cycle.rsp').write_text('@' + self.path('./cycle.rsp'))
        r = self.run_xhcc('@' + self.path('cycle.rsp'), success=False)
        self.assertIn('cycle', r.stderr.lower())
        (self.dir / 'bad.rsp').write_text('"unterminated')
        self.run_xhcc('@' + self.path('bad.rsp'), success=False)
        (self.dir / 'huge.rsp').write_text('x' * (33 * 1024))
        self.run_xhcc('@' + self.path('huge.rsp'), success=False)
        (self.dir / 'many.rsp').write_text(' '.join(['-v'] * 513))
        self.run_xhcc('@' + self.path('many.rsp'), success=False)
        for i in range(9):
            (self.dir / f'depth{i}.rsp').write_text('@' + self.path(f'depth{i+1}.rsp') if i < 8 else '--help')
        self.run_xhcc('@' + self.path('depth0.rsp'), success=False)

    def test_multiple_sources_explicit_output_rejected(self):
        for flag in ['-o', '-MF', '-MT']:
            self.run_xhcc('-c', '-MMD', self.path('one.c'), self.path('two.c'),
                          flag, self.path('shared.out'), success=False)

    def test_macro_actions_forced_include_and_dependencies(self):
        out = self.path('macro_result.o')
        dep = self.path('macro_result.d')
        r = self.run_xhcc('arm-none-eabi-gcc', '-c', '-std=c99', '-O1',
                         '-mcpu=cortex-m3', '-mthumb', '-mfloat-abi=soft',
                         '-DX=1', '-UX', '-DY=2', '-UY', '-D', 'Y=7',
                         '-include', self.path('forced.h'), '-MMD', '-MF', dep,
                         '-MT', 'custom_target', '-o', out, '--memory-report',
                         self.path('macros.c'))
        self.assertEqual((ROOT / out).read_bytes()[:4], b'\x7fELF')
        text = (ROOT / dep).read_text()
        self.assertTrue(text.startswith('custom_target:'), text)
        self.assertIn('forced.h', text)
        self.assertIn('arena peak', r.stderr)

    def test_preprocessing_assembly_and_quoted_response(self):
        response = self.dir / 'arguments with spaces.rsp'
        response.write_text('armcc --c99 --cpu Cortex-M3 --thumb --fpu SoftVFP '
                            '-E "' + self.path('one.c') + '"')
        self.assertIn('int one', self.run_xhcc('@' + self.path(response.name)).stdout)
        out = self.path('one.s')
        self.run_xhcc('-S', '-O0', '-o', out, self.path('one.c'))
        self.assertTrue((ROOT / out).stat().st_size)
        private = self.path('one.ark_obj')
        self.run_xhcc('-c', '--private-object', '-o', private, self.path('one.c'))
        self.assertNotEqual((ROOT / private).read_bytes()[:4], b'\x7fELF')

    def test_multiple_sources_compile_sequentially(self):
        # Use unique basenames: default GCC outputs are in the process cwd.
        names = [self.dir.name + '_first', self.dir.name + '_second']
        outputs = [ROOT / (name + '.o') for name in names]
        try:
            for name in names:
                (self.dir / (name + '.c')).write_text('int ' + name.replace('-', '_') + '(void) { return 3; }\n')
            result = self.run_xhcc('-c', '--memory-report', *(self.path(name + '.c') for name in names))
            for output in outputs:
                self.assertEqual(output.read_bytes()[:4], b'\x7fELF')
            self.assertEqual(result.stderr.count('arena peak'), 2)
        finally:
            for output in outputs:
                if output.exists():
                    output.unlink()

    def test_missing_package_rejected(self):
        self.run_xhcc('--package-root', self.path('absent'), '-c', self.path('one.c'), success=False)


if __name__ == '__main__':
    unittest.main()
