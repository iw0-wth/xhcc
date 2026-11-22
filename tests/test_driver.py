"""Driver integration tests. Run after tools/build.py; no third-party modules.

XHCC_COMMAND may contain a JSON array (native executable or wasmtime invocation).
The default runs the explicit dev/all monolith with the project directory mounted
as guest cwd. Shared-pipeline coverage is in test_staged_driver.py.
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
    return [runtime, 'run', '--dir=.', str(ROOT / 'build/wasm/dev/all/xhcc.wasm')]


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
        self.assertIn('C11 subset', self.run_xhcc('--package-root', 'missing-package', '--help').stdout)
        self.assertIn('xhcc', self.run_xhcc('--version').stdout)

    def test_reject_options_and_missing_values(self):
        for flag in ['-O3', '-marm', '-mfloat-abi=hard', '-mcpu=cortex-m0',
                     '-mcpu=cortex-m23', '-std=c17', '-Wunknown', '-funknown',
                     '-g', '-ffunction-sections']:
            with self.subTest(flag=flag):
                self.run_xhcc('-c', self.path('one.c'), flag, success=False)
        self.run_xhcc('-c', self.path('one.c'), '-I', success=False)
        self.run_xhcc(self.path('one.c'), success=False)
        self.run_xhcc('-c', self.path('one.c'), '-MF', self.path('bad.d'), success=False)

    def test_target_aliases_and_architecture(self):
        cases = [('arm-none-eabi-gcc', 'arm-none-eabi', 'armv7-m', 'aapcs', 40),
                 ('armcc', 'arm-none-eabi', 'armv7-m', 'aapcs', 40),
                 ('i686-linux-gnu-gcc', 'i686-linux-gnu', 'i686', 'sysv', 3),
                 ('x86_64-linux-gnu-gcc', 'x86_64-linux-gnu', 'x86-64', 'sysv', 62),
                 ('riscv32-unknown-elf-gcc', 'riscv32-unknown-elf', 'rv32im', 'ilp32', 243)]
        for tool, triple, arch, abi, machine in cases:
            with self.subTest(tool=tool):
                self.assertEqual(self.run_xhcc(tool, '-dumpmachine').stdout.strip(), triple)
                output = self.path(tool + '.o')
                self.run_xhcc(tool, '-xc', '-c', '-march=' + arch, '-mabi=' + abi,
                              '-o', output, self.path('one.c'))
                data = (ROOT / output).read_bytes()
                self.assertEqual(data[:4], b'\x7fELF')
                self.assertEqual(int.from_bytes(data[18:20], 'little'), machine)
        self.run_xhcc('--target=x86_64-linux-gnu', '-x', 'c', '-S',
                      '-o', self.path('target.s'), self.path('one.c'))
        self.run_xhcc('xh-c-gcc', '-S', '-o', self.path('generated.c'), self.path('one.c'))
        self.run_xhcc('xh-c-gcc', '-c', self.path('one.c'), success=False)
        self.run_xhcc('-x', 'c++', '-c', self.path('one.c'), success=False)
        self.run_xhcc('armcc', '-c', '-march=rv32im', self.path('one.c'), success=False)

    def test_standards_optimization_cpu_and_config(self):
        for standard in ('c99', 'c11', 'gnu99', 'gnu11'):
            self.run_xhcc('-c', '-std=' + standard, '-O2', '-mcpu=cortex-m4',
                          '-fopt-inline', '--warn-enum', '--pipeline=full',
                          '-o', self.path('standard.o'), self.path('one.c'))
        cfg = self.dir / 'driver.cfg'
        cfg.write_text('[general]\nopt_level=1\n')
        self.run_xhcc('-c', '-config', self.path('driver.cfg'), '-O2',
                      '-o', self.path('config.o'), self.path('one.c'))
        self.run_xhcc('-c', '-config', self.path('missing.cfg'),
                      self.path('one.c'), success=False)
        self.run_xhcc('-c', '--diag_suppress=abc', self.path('one.c'), success=False)

    def test_include_search_classes_and_nostdinc(self):
        for name in ('quote', 'user', 'system'):
            (self.dir / name).mkdir(exist_ok=True)
            (self.dir / name / 'search.h').write_text('#define SEARCH_' + name.upper() + ' 1\n')
        (self.dir / 'search.c').write_text(
            '#include "search.h"\n#ifndef SEARCH_QUOTE\n#error quote search order\n#endif\n'
            '#undef SEARCH_QUOTE\n#include <search.h>\n#ifndef SEARCH_USER\n#error angle search order\n#endif\n'
            'int search(void) { return 0; }\n')
        self.run_xhcc('-c', '-iquote', self.path('quote'), '-isystem', self.path('system'),
                      '-I', self.path('user'), '-o', self.path('search.o'), self.path('search.c'))
        self.run_xhcc('-E', '-nostdinc', self.path('one.c'))
        (self.dir / 'nostd.c').write_text('#include <stdint.h>\n')
        self.run_xhcc('-E', '-nostdinc', self.path('nostd.c'), success=False)

    def test_legacy_entry_private_objects_and_pipeline(self):
        output = self.path('legacy.ark_obj')
        self.run_xhcc('--ark-compat', '-c', '-O2', '-o', output, self.path('one.c'))
        self.assertNotEqual((ROOT / output).read_bytes()[:4], b'\x7fELF')
        result = self.run_xhcc('--ark-compat', '--print-pipeline', '-O2')
        self.assertIn('inline', result.stdout + result.stderr)
        (self.dir / 'legacy.rsp').write_text('--ark-compat --list-backends')
        result = self.run_xhcc('@' + self.path('legacy.rsp'))
        self.assertIn('arm', result.stdout + result.stderr)

    def test_arm_softfp_cpu_fpu_validation(self):
        for cpu, fpu in (('cortex-m4', 'fpv4-sp-d16'), ('cortex-m7', 'fpv5-d16'),
                         ('cortex-m7', 'fpv5-sp-d16'), ('cortex-m33', 'fpv5-sp-d16')):
            self.run_xhcc('-c', '-mcpu=' + cpu, '-mfloat-abi=softfp', '-mfpu=' + fpu,
                          '-o', self.path('softfp.o'), self.path('one.c'))
        for options in (('-mcpu=cortex-m3', '-mfloat-abi=softfp'),
                        ('-mcpu=cortex-m4', '-mfloat-abi=softfp', '-mfpu=fpv5-d16'),
                        ('-mcpu=cortex-m33', '-mfloat-abi=softfp', '-mfpu=fpv5-d16'),
                        ('-mcpu=cortex-m7', '-mfloat-abi=hard'),
                        ('-mcpu=cortex-m4', '-mfpu=fpv4-sp-d16')):
            self.run_xhcc('-c', *options, self.path('one.c'), success=False)

    def test_preprocessor_macro_dump_and_line_markers(self):
        (self.dir / 'dump.c').write_text('#define DUMPED_OBJECT 42\n#define DUMPED_FN(x) ((x)+1)\n'
                                        '#define REMOVED 1\n#undef REMOVED\nint dump_body;\n')
        result = self.run_xhcc('-E', '-dM', '-std=c11', '-DCLI_DUMP=7', self.path('dump.c'))
        for definition in ('#define __STDC__ 1', '#define __STDC_VERSION__ 201112L',
                           '#define CLI_DUMP 7', '#define DUMPED_OBJECT 42', '#define DUMPED_FN('):
            self.assertIn(definition, result.stdout)
        self.assertNotIn('#define REMOVED ', result.stdout)
        self.assertNotIn('int dump_body', result.stdout)
        self.run_xhcc('-E', '-dM', '-o', self.path('macros.out'), self.path('dump.c'))
        self.assertIn('#define DUMPED_OBJECT 42', (self.dir / 'macros.out').read_text())
        result = self.run_xhcc('-E', '-P', self.path('dump.c'))
        self.assertIn('int dump_body', result.stdout)
        self.assertNotIn('#line', result.stdout)
        self.run_xhcc('-c', '-dM', self.path('dump.c'), success=False)
        self.run_xhcc('-E', '-dD', self.path('dump.c'), success=False)

    def test_compiler_mode_predefines(self):
        (self.dir / 'mode.c').write_text(
            '#if __STDC_HOSTED__ != EXPECT_HOSTED\n#error hosted mode\n#endif\n'
            '#if __STDC_VERSION__ != EXPECT_VERSION\n#error language mode\n#endif\n'
            '#ifdef EXPECT_GCC\n#ifndef __GNUC__\n#error GCC mode\n#endif\n#endif\n'
            'int mode(void) { return 0; }\n')
        self.run_xhcc('-fsyntax-only', '-std=c11', '-ffreestanding', '-fno-builtin',
                      '-DEXPECT_HOSTED=0', '-DEXPECT_VERSION=201112L', '-DEXPECT_GCC=1', self.path('mode.c'))
        self.run_xhcc('-fsyntax-only', '-std=gnu11', '-std=c99', '-fhosted', '-fbuiltin',
                      '-DEXPECT_HOSTED=1', '-DEXPECT_VERSION=199901L', self.path('mode.c'))
        (self.dir / 'undef.c').write_text(
            '#ifdef __GNUC__\n#error GCC nonstandard macro survived undef\n#endif\n'
            '#ifndef __STDC__\n#error standard macro removed by undef\n#endif\nint undef_mode;\n')
        self.run_xhcc('-fsyntax-only', '-undef', self.path('undef.c'))

    def test_warning_flags_and_syntax_only(self):
        (self.dir / 'warning.c').write_text('#warning driver-warning\nint warned(void) { return 1; }\n')
        self.run_xhcc('-fsyntax-only', '-Wall', '-Wextra', self.path('one.c'))
        self.run_xhcc('-fsyntax-only', '-Werror', self.path('warning.c'), success=False)
        self.run_xhcc('-fsyntax-only', '-Werror', '-Wno-cpp', self.path('warning.c'))
        self.run_xhcc('-fsyntax-only', '-Werror', '-Wno-error', self.path('warning.c'))
        self.run_xhcc('-fsyntax-only', '-Werror', '-w', self.path('warning.c'))
        (self.dir / 'syntax-invalid.c').write_text('int broken(void) { return absent_name; }\n')
        self.run_xhcc('-fsyntax-only', self.path('syntax-invalid.c'), success=False)
        for flag in ('-Wno-unused-function', '-Wno-unused-result', '-Wno-switch-enum',
                     '-Wno-unreachable-code', '-Wno-infinite-recursion', '-Wno-return-local-addr'):
            self.run_xhcc('-fsyntax-only', '-Wall', '-Wextra', flag, self.path('one.c'))
        self.run_xhcc('-fsyntax-only', '-Wno-unknown-warning', self.path('one.c'), success=False)

    def test_gcc_dependency_modes(self):
        (self.dir / 'deps.c').write_text('#include "forced.h"\nint deps(void) { return FORCED; }\n')
        result = self.run_xhcc('-MM', '-MP', '-MQ', 'quoted target.o', self.path('deps.c'))
        self.assertIn('quoted\\ target.o:', result.stdout)
        self.assertIn('forced.h:', result.stdout)
        self.assertNotIn('int deps', result.stdout)
        result = self.run_xhcc('-M', '-MT', 'literal target.o', self.path('deps.c'))
        self.assertTrue(result.stdout.startswith('literal target.o:'), result.stdout)
        self.run_xhcc('-MM', '-o', self.path('only.d'), self.path('deps.c'))
        self.assertIn('forced.h', (self.dir / 'only.d').read_text())
        self.run_xhcc('-c', '-MD', '-MF', self.path('md.d'), '-o', self.path('md.o'), self.path('deps.c'))
        self.assertEqual((self.dir / 'md.o').read_bytes()[:4], b'\x7fELF')
        (self.dir / 'deps-system').mkdir(exist_ok=True)
        (self.dir / 'deps-system' / 'systemdep.h').write_text('#define SYSTEM_DEP 1\n')
        (self.dir / 'systemdeps.c').write_text('#include <systemdep.h>\nint sdep;\n')
        result = self.run_xhcc('-MM', '-isystem', self.path('deps-system'), self.path('systemdeps.c'))
        self.assertNotIn('systemdep.h', result.stdout)
        result = self.run_xhcc('-M', '-isystem', self.path('deps-system'), self.path('systemdeps.c'))
        self.assertIn('systemdep.h', result.stdout)
        self.run_xhcc('-c', '-MP', self.path('one.c'), success=False)
        self.run_xhcc('-MM', '-MT', 'a', '-MQ', 'b', self.path('deps.c'), success=False)

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
