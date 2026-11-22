"""C template integration against the already-built compiler (no build here).

An unchanged Wasm SHA emits different C after an external arithmetic rule edit;
when a host C compiler is present the two generated programs execute differently.
XH_C_NATIVE_CC selects the native compiler explicitly.
"""
from pathlib import Path
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RULES = ROOT / 'packages/c/c.rules'
COMPILER = ROOT / 'build/wasm/xhcc.wasm'


class CRules(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not COMPILER.is_file():
            raise unittest.SkipTest('build the compiler first')
        cls.workspace = tempfile.TemporaryDirectory(prefix='c-rules-', dir=ROOT)
        cls.work = Path(cls.workspace.name)
        cls.package_root = cls.work / 'packages'
        cls.package = cls.package_root / 'c_variant'
        cls.package.mkdir(parents=True)
        (cls.package / 'include').mkdir()
        shutil.copy2(ROOT / 'packages/c/target.rules', cls.package / 'target.rules')
        manifest = (ROOT / 'packages/c/package.ini').read_text(encoding='utf-8')
        manifest = manifest.replace('id=c\n', 'id=c_variant\n').replace('tools=xh-c-gcc', 'tools=c-variant-gcc')
        if not re.search(r'^templates=', manifest, re.M):
            manifest += '\ntemplates=c.rules\n'
        (cls.package / 'package.ini').write_text(manifest, encoding='utf-8')
        cls.variant = cls.package / 'c.rules'
        cls.original = RULES.read_text(encoding='utf-8')
        cls.source = cls.work / 'source.c'
        cls.source.write_text('int main(void) { int a = 10, b = 3; return a + b; }\n', encoding='utf-8')

    @classmethod
    def tearDownClass(cls):
        cls.workspace.cleanup()

    def compile(self, text, label='generated', success=True):
        self.variant.write_text(text, encoding='utf-8')
        output = self.work / (label + '.c')
        if output.exists():
            output.unlink()
        process = subprocess.run([
            sys.executable, 'tools/xhcc.py', '--package-root', str(self.package_root),
            'c-variant-gcc', '-O0', '-S', str(self.source.relative_to(ROOT)),
            '-o', str(output.relative_to(ROOT))], cwd=ROOT, capture_output=True, text=True, timeout=60)
        if success:
            self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
            self.assertTrue(output.is_file(), process.stdout + process.stderr)
        else:
            self.assertNotEqual(process.returncode, 0, process.stdout + process.stderr)
            self.assertFalse(output.exists(), process.stdout + process.stderr)
            self.assertIn('template', process.stderr.lower())
        return output

    def test_external_arithmetic_changes_generated_c_and_execution_same_sha(self):
        before = hashlib.sha256(COMPILER.read_bytes()).digest()
        base = self.compile(self.original, 'base')
        changed = self.original.replace('text (ark_a + ark_c)', 'text (ark_a - ark_c)', 1)
        self.assertNotEqual(changed, self.original)
        variant = self.compile(changed, 'variant')
        self.assertIn('(ark_a + ark_c)', base.read_text(encoding='utf-8'))
        self.assertIn('(ark_a - ark_c)', variant.read_text(encoding='utf-8'))
        self.assertNotEqual(base.read_bytes(), variant.read_bytes())
        self.assertEqual(hashlib.sha256(COMPILER.read_bytes()).digest(), before)
        compiler = os.environ.get('XH_C_NATIVE_CC') or shutil.which('gcc')
        if not compiler:
            for candidate in [r'C:\msys64\ucrt64\bin\gcc.exe', r'C:\msys64\mingw64\bin\gcc.exe',
                              r'D:\msys64\ucrt64\bin\gcc.exe', r'D:\msys64\mingw64\bin\gcc.exe']:
                if Path(candidate).is_file():
                    compiler = candidate
                    break
        if not compiler:
            arm = Path(os.environ.get('XH_C_ARM_CC', r'D:\arm-none-eabi\bin\arm-none-eabi-gcc.exe'))
            self.assertTrue(arm.is_file(), 'Set XH_C_NATIVE_CC or XH_C_ARM_CC to complete execution evidence')
            values = [self.run_arm_generated_c(arm, source) for source in [base, variant]]
            self.assertEqual(values, [13, 7])
            self.assertEqual(hashlib.sha256(COMPILER.read_bytes()).digest(), before)
            return
        env = os.environ.copy()
        env['PATH'] = str(Path(compiler).resolve().parent) + os.pathsep + env.get('PATH', '')
        values = []
        for source in [base, variant]:
            exe = source.with_suffix('.exe')
            process = subprocess.run([compiler, '-w', '-std=gnu99', str(source), '-lm', '-o', str(exe)],
                                     cwd=ROOT, env=env, capture_output=True, text=True, timeout=90)
            self.assertEqual(process.returncode, 0, process.stderr)
            values.append(subprocess.run([str(exe)], env=env, capture_output=True, timeout=30).returncode)
        self.assertEqual(values, [13, 7])
        self.assertEqual(hashlib.sha256(COMPILER.read_bytes()).digest(), before)

    def run_arm_generated_c(self, compiler, source):
        # Build the actual emitted C with the installed GNU ARM compiler. Dead
        # libc wrapper sections are discarded; execute ark_main's arithmetic
        # with real ARM instructions using Unicorn. No replacement C source.
        sys.path.insert(0, str(ROOT / 'build/test-deps'))
        from elftools.elf.elffile import ELFFile
        from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB
        from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0
        elf_path = source.with_suffix('.elf')
        process = subprocess.run([str(compiler), '-w', '-Wno-error=implicit-function-declaration', '-std=gnu99', '-O1', '-mcpu=cortex-m3',
                                  '-mthumb', '-ffunction-sections', '-fdata-sections', '-nostdlib',
                                  '-Wl,--gc-sections,-Ttext=0x10000,-e,ark_main', str(source),
                                  '-o', str(elf_path)], cwd=ROOT, capture_output=True, text=True, timeout=90)
        self.assertEqual(process.returncode, 0, process.stderr)
        with elf_path.open('rb') as stream:
            elf = ELFFile(stream)
            uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
            uc.mem_map(0x10000, 0x40000)
            uc.mem_map(0x700000, 0x20000)
            for segment in elf.iter_segments():
                if segment['p_type'] == 'PT_LOAD' and segment['p_filesz']:
                    uc.mem_write(segment['p_vaddr'], segment.data())
            uc.reg_write(UC_ARM_REG_SP, 0x71f000)
            uc.reg_write(UC_ARM_REG_LR, 0x4f001)
            uc.emu_start(elf.header['e_entry'] | 1, 0x4f000, count=100000)
            return uc.reg_read(UC_ARM_REG_R0) & 0xffffffff

    def test_version_malformed_and_missing_rule_rejected(self):
        invalid = [
            self.original.replace('XHTEMPLATE 1', 'XHTEMPLATE 999', 1),
            self.original + '\nrule invalid\ntext \\q\nend\n',
            self.original + '\nrule invalid\ntext ${16}\nend\n',
            self.original + '\nrule invalid\ntext ${oops}\nend\n',
            self.original + '\nrule invalid\ntext trailing\\\nend\n',
            self.original + '\nrule file_header\ntext duplicate\nend\n',
            self.original.replace('rule tok_plus\n', 'rule renamed_plus\n', 1),
            self.original.replace('rule ir_push_int_value\n', 'rule renamed_push\n', 1),
            'XHTEMPLATE 1\n',
        ]
        for index, text in enumerate(invalid):
            with self.subTest(index=index):
                self.compile(text, 'invalid_' + str(index), success=False)

    def test_abi_contract_and_policy_rules(self):
        for key, value in [('abi_pointer_bits', '64'), ('abi_slot_bytes', '4'),
                           ('abi_frame_align', '3'), ('abi_varargs_offset', '7999'),
                           ('policy_frame_min', '8192'), ('policy_stack_margin', '0'),
                           ('policy_return_buffer_bytes', '99999999999999999999')]:
            with self.subTest(key=key):
                bad = re.sub(r'(rule ' + key + r'\ntext )[^\n]*', lambda m: m[1] + value, self.original)
                self.compile(bad, 'abi_invalid', success=False)
        changed = self.original.replace('rule policy_frame_min\ntext 8704',
                                        'rule policy_frame_min\ntext 9000')
        changed = changed.replace('rule policy_stack_min\ntext 256',
                                  'rule policy_stack_min\ntext 300')
        output = self.compile(changed, 'policy').read_text(encoding='utf-8')
        self.assertIn('char frame[9000]', output)
        self.assertIn('ark_slot stk[300]', output)

    def test_placeholder_and_output_bounds_fail(self):
        # Caller supplies only two numeric args. A valid VM placeholder that is
        # outside this call's arity must fail when the instruction is emitted.
        bad = self.original.replace('text stk[sp++].i = ${0}LL;',
                                    'text stk[sp++].i = ${15}LL;', 1)
        self.assertNotEqual(bad, self.original)
        self.compile(bad, 'arity', success=False)
        # A long but valid template must fail the adapter's explicit output
        # capacity instead of silently truncating the generated C instruction.
        bad = self.original.replace('text stk[sp++].i = ${0}LL;',
                                    'text ' + 'x' * 17000 + '${0}', 1)
        self.compile(bad, 'capacity', success=False)


if __name__ == '__main__':
    unittest.main()
