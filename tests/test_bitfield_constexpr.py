"""Bit-field ICE regression coverage; run AFTER rebuilding the compiler.

No build and no automatic tool-suite registration here. XHCC_COMMAND selects an
explicit fresh compiler; otherwise use the existing test_driver project default.
Execute from xhcc: python -m unittest discover -s tests -p test_bitfield_constexpr.py -v
"""
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_driver import ROOT, command
import test_c_rules


class BitfieldConstexpr(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.command = command()
        cls.workspace = tempfile.TemporaryDirectory(prefix='bitfield-constexpr-', dir=ROOT)
        cls.directory = Path(cls.workspace.name)
        cls.serial = 0
        # Evidence identifies the actual selected artifact, not a remembered build.
        cls.artifacts = {}
        for arg in cls.command:
            candidate = Path(arg)
            if candidate.suffix in ('.wasm', '.exe') and candidate.is_file():
                cls.artifacts[str(candidate.resolve())] = hashlib.sha256(candidate.read_bytes()).hexdigest()
        print('bitfield compiler artifacts:', cls.artifacts, flush=True)

    @classmethod
    def tearDownClass(cls):
        cls.workspace.cleanup()

    def compile(self, source, tool='arm-none-eabi-gcc', success=True, diagnostic=None):
        type(self).serial += 1
        src = self.directory / ('case_%d.c' % self.serial)
        output = src.with_name(src.stem + '_generated.c') if tool == 'xh-c-gcc' else src.with_suffix('.s')
        src.write_text(source, encoding='utf-8')
        args = [*self.command, tool, '-std=c99', '-O0', '-S',
                src.relative_to(ROOT).as_posix(), '-o', output.relative_to(ROOT).as_posix()]
        result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=90)
        evidence = result.stdout + result.stderr
        if success:
            self.assertEqual(result.returncode, 0, evidence)
            self.assertTrue(output.is_file(), evidence)
        else:
            self.assertNotEqual(result.returncode, 0, evidence)
            if diagnostic:
                self.assertRegex(evidence.lower(), diagnostic)
        return output

    def test_micropython_arm_size_t_and_enum_macro_widths(self):
        self.compile('''
typedef unsigned int size_t;
enum { HEADER_BITS = 8, FLAG_BITS = 1 };
#define WIDTH (HEADER_BITS * sizeof(size_t) - FLAG_BITS)
struct Qstr { size_t total_prev_len : (8 * sizeof(size_t) - 1); size_t flag:1; };
struct Obj { size_t refs : WIDTH; unsigned int flag:FLAG_BITS; };
struct Mpz { size_t alloc : (8*sizeof(size_t)-2); unsigned int neg:1; unsigned int fixed:1; };
struct Obj29 { size_t count:(8*sizeof(size_t)-3); unsigned flags:3; };
struct Arithmetic { unsigned int alloc : ((sizeof(size_t) * 8) / 2 + 15 - 0); unsigned int neg:1; };
_Static_assert(sizeof(struct Qstr) == 4, "qstr width31 plus flag1");
_Static_assert(sizeof(struct Obj) == 4, "enum and macro width31");
_Static_assert(sizeof(struct Mpz) == 4, "mpz width30");
_Static_assert(sizeof(struct Obj29) == 4, "obj width29");
_Static_assert(sizeof(struct Arithmetic) == 4, "arithmetic width31");
int width_test(void) { return sizeof(struct Qstr); }
''')

    def test_sizeof_unsigned_long_tracks_selected_package(self):
        for tool, expected in [('arm-none-eabi-gcc', 4), ('x86_64-linux-gnu-gcc', 8)]:
            with self.subTest(tool=tool):
                self.compile('''
struct Bits { unsigned int first:(sizeof(unsigned long) * 4); unsigned int tail:1; };
_Static_assert(sizeof(unsigned long) == %d, "selected target long size");
_Static_assert(sizeof(struct Bits) == %d, "32bit base layout follows width");
int width_test(void) { return sizeof(struct Bits); }
''' % (expected, 4 if expected == 4 else 8), tool=tool)

    def test_unnamed_zero_width_aligns_next_container(self):
        self.compile('''
struct Padding { unsigned a:(2+2); unsigned :(sizeof(int)-4); unsigned b:(8/2); };
_Static_assert(sizeof(struct Padding) == 8, "unnamed zero ends container");
int padding_test(void) { return sizeof(struct Padding); }
''')

    def test_invalid_widths_and_base_types_diagnose(self):
        cases = [
            ('unsigned value:-(1+1);', r'bit.field.*negative'),
            ('unsigned value:NEG_WIDTH;', r'bit.field.*negative'),
            ('unsigned value:FALSE_WIDTH;', r'named bit.field.*zero'),
            ('unsigned value:8*sizeof(int)+1;', r'bit.field.*exceeds.*type'),
            ('unsigned char value:9;', r'bit.field.*exceeds.*type'),
            ('unsigned value:(2-2);', r'named bit.field.*zero'),
            ('unsigned value:(2147483647+1);', r'bit.field.*(constant|overflow)'),
            ('unsigned value:(9223372036854775807LL*2);', r'bit.field.*(constant|overflow)'),
            ('unsigned value:(4/0);', r'bit.field.*(constant|overflow)'),
            ('unsigned value:1.5;', r'bit.field.*(integer|constant)'),
            ('unsigned value:(1 ? 3 : variable);', r'bit.field.*constant'),
            ('unsigned value:(0 && variable);', r'bit.field.*constant'),
            ('unsigned value:(1 ? -1 : 1u);', r'bit.field.*exceeds.*type'),
            ('unsigned value:(unsigned char)255 + 1;', r'bit.field.*exceeds.*type'),
            ('unsigned value:(signed char)255;', r'bit.field.*negative'),
            ('unsigned value:((unsigned)1 << 2);', r'bit.field.*supported.*constant'),
            ('float value:3;', r'bit.field.*integer type'),
            ('struct Inner value:3;', r'bit.field.*integer type'),
            ('_Bool value:2;', r'bit.field.*exceeds.*type'),
        ]
        for member, diagnostic in cases:
            with self.subTest(member=member):
                self.compile('enum { NEG_WIDTH=-1, FALSE_WIDTH=0 }; int variable; struct Inner { int x; }; struct Bad { %s };' % member,
                             success=False, diagnostic=diagnostic)
        self.compile('struct Bad { unsigned long value:31; };', tool='x86_64-linux-gnu-gcc',
                     success=False, diagnostic=r'bit.field.*32.bit containers')

    def test_unevaluated_division_does_not_reject_valid_width(self):
        self.compile('''
enum { NEG_WIDTH = -1, FALSE_WIDTH = 0 };
struct Valid { unsigned int a:(1 ? 3 : (1/0)); unsigned int :(FALSE_WIDTH && (1/0)); unsigned int b:4; };
struct Casts { unsigned int a:(unsigned char)257; unsigned int b:(1 ? 4 : 1u); unsigned int c:(sizeof(int)+NEG_WIDTH); };
int selected_width(void) { return sizeof(struct Valid); }
''')

    def test_actual_generated_c_signed_fullwidth_topbit_and_rmw_execute(self):
        output = self.compile('''
typedef unsigned int size_t;
enum { SIGNED_BITS = 4 };
struct Q { size_t len:(8*sizeof(size_t)-1); unsigned flag:1; };
struct S { signed int value:SIGNED_BITS; unsigned tail:(sizeof(unsigned)*8-SIGNED_BITS); };
struct F { signed int value:(8*sizeof(int)); };
int main(void) {
    struct Q q; struct S s; struct F f;
    q.len=0x12345678u; q.flag=1;
    q.len=0x76543210u;
    if (q.flag != 1 || q.len != 0x76543210u) return 11;
    q.flag=0;
    if (q.len != 0x76543210u) return 12;
    s.tail=0x7654321u; s.value=-8;
    if (s.value != -8 || s.tail != 0x7654321u) return 13;
    s.value=7;
    if (s.tail != 0x7654321u) return 14;
    f.value=-2147483647-1;
    return f.value < 0 ? 73 : 15;
}
''', tool='xh-c-gcc')
        arm = Path(os.environ.get('XH_C_ARM_CC', r'D:\arm-none-eabi\bin\arm-none-eabi-gcc.exe'))
        self.assertTrue(arm.is_file(), 'Set XH_C_ARM_CC for real GNU ARM linking and Unicorn execution')
        # Reuse the existing actual emitted-C -> GNU ARM ELF -> Unicorn runner.
        self.assertEqual(test_c_rules.CRules.run_arm_generated_c(self, arm, output), 73)


if __name__ == '__main__':
    unittest.main()
