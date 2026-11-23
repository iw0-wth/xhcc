"""Full-lexeme floating literal regressions; no compiler build is started.
ARM/AMD64 checks link emitted objects and execute integer views of IEEE bits.
"""
from pathlib import Path
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from test_driver import ROOT

if os.name == 'nt':
    pd = Path(sys.executable).parent
    os.environ['PATH'] = os.pathsep.join([str(pd), str(pd/'Library/bin'), os.environ.get('PATH', '')])
sys.path.insert(0, str(ROOT/'build/test-deps'))
import test_arm_rules
import test_x86_rules

DOUBLE_CASES = [
    ('9223372036854775808.0', 0x43e0000000000000),
    ('9223372036854775808.0e0', 0x43e0000000000000),
    ('9223372036854774784.0', 0x43dfffffffffffff),
    ('9223372036854774784.0e0', 0x43dfffffffffffff),
    ('0x1.fffffffffffffp+62', 0x43dfffffffffffff),
    ('0x8000000000000000p0', 0x43e0000000000000),
    ('0x10000000000000000p0', 0x43f0000000000000),
    ('0x0.0000000000001p-1022', 1),
    ('4.9406564584124654e-324', 1),
    ('1.7976931348623157e308', 0x7fefffffffffffff),
    ('01.5', 0x3ff8000000000000),
    ('01e1', 0x4024000000000000),
    # Exponent comes after the old 64-byte scratch cutoff.
    ('1.' + '0'*90 + 'e2', 0x4059000000000000),
    # Above the exact halfway point by a digit far beyond 64 bytes.
    ('1.00000000000000011102230246251565404236316680908203125' + '0'*50 + '1', 0x3ff0000000000001),
]
FLOAT_CASES = [
    ('1.5f', 0x3fc00000), ('1.5e0f', 0x3fc00000),
    ('0x1.000002p0f', 0x3f800001),
    ('0x1p-149f', 1), ('1.401298464324817e-45f', 1),
    ('1.000000059604644775390625' + '0'*50 + '1f', 0x3f800001),
    ('1.' + '0'*90 + 'e2f', 0x42c80000),
]


class FloatLiteralTests(unittest.TestCase):
    def invoke(self, tool, source, out, success=True):
        result = subprocess.run([sys.executable, 'tools/xhcc.py', tool, '-O0', '-c',
                                 source.relative_to(ROOT).as_posix(), '-o', out.relative_to(ROOT).as_posix()],
                                cwd=ROOT, text=True, capture_output=True, timeout=60)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def bit_source(self):
        lines = ['union D { double d; unsigned int w[2]; };',
                 'union F { float f; unsigned int w; };',
                 'int entry(void) { union D d; union F f;']
        for index, (literal, bits) in enumerate(DOUBLE_CASES):
            lines += [f'd.d = {literal};',
                      f'if (d.w[0] != {bits & 0xffffffff}U || d.w[1] != {bits >> 32}U) return {index + 1};']
        for index, (literal, bits) in enumerate(FLOAT_CASES):
            lines += [f'f.f = {literal};', f'if (f.w != {bits}U) return {index + 101};']
        lines += ['return 0; }']
        return '\n'.join(lines) + '\n'

    def test_literal_ieee_bits_arm_and_amd64_execution(self):
        for target, tool in [('arm', 'arm-none-eabi-gcc'), ('x86_64', 'x86_64-linux-gnu-gcc')]:
            with self.subTest(target=target), tempfile.TemporaryDirectory(prefix='float-lit-', dir=ROOT/'build') as tmp:
                work = Path(tmp); source = work/'bits.c'; source.write_text(self.bit_source())
                if target == 'arm':
                    helper = test_arm_rules.ArmRules(); elf = helper.compile_link(work, source)
                    actual = helper.execute(elf)
                else:
                    obj = work/'bits.o'; self.invoke(tool, source, obj)
                    actual = test_x86_rules.X86Rules().execute(obj, True)
                self.assertEqual(actual, 0, f'{target} failed literal case {actual}')

    def test_integer_token_boundaries_execute(self):
        text = '''
int entry(void) {
    unsigned long long a = 18446744073709551615ULL;
    unsigned long long b = 0xffffffffffffffffULL;
    unsigned long long c = 01777777777777777777777ULL;
    if (a != b || b != c) return 1;
    if (0xffffffffU != 4294967295U || 0777 != 511 || 0x1234 != 4660) return 2;
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='float-int-', dir=ROOT/'build') as tmp:
            work = Path(tmp); source = work/'ints.c'; source.write_text(text)
            obj = work/'ints.o'; self.invoke('x86_64-linux-gnu-gcc', source, obj)
            self.assertEqual(test_x86_rules.X86Rules().execute(obj, True), 0)

    def test_invalid_or_overflowing_literals_are_diagnosed(self):
        for literal in ['18446744073709551616ULL', '0x10000000000000000ULL',
                        '02000000000000000000000ULL', '0x', '1e+', '0x1p+', '0x1.2', '09']:
            with self.subTest(literal=literal), tempfile.TemporaryDirectory(prefix='float-bad-', dir=ROOT/'build') as tmp:
                work = Path(tmp); source = work/'bad.c'; source.write_text(f'int f(void) {{ return {literal}; }}\n')
                result = self.invoke('arm-none-eabi-gcc', source, work/'bad.o', success=False)
                self.assertIn('literal', (result.stdout + result.stderr).lower())


if __name__ == '__main__':
    unittest.main()
