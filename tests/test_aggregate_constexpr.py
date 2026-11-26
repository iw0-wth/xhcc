"""Independent source harnesses and post-build ARM ELF constexpr regression.

No compiler rebuild, suite registration, or MicroPython build. HOST_CC selects
native GCC for harnesses; XHCC_COMMAND selects the rebuilt compiler for ELF tests.
Run source-only checks with unittest's AggregateConstexprSourceHarness class.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'build/test-deps'))

HARNESS = r'''
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "agginit.h"
void *mem_pool_alloc(int size) { return calloc(1,(size_t)size); }
static Type int_t = {.kind=TYPE_INT};
Type *type_int(void) { return &int_t; }
int type_ptr_size(void) { return 4; }
int type_is_integer(Type *t) { return t && t->kind!=TYPE_PTR && t->kind!=TYPE_FLOAT && t->kind!=TYPE_DOUBLE; }
int type_size(Type *t) {
    if (!t) return 4;
    if (t->kind == TYPE_ARRAY) return t->size * type_size(t->base);
    if (t->kind == TYPE_STRUCT || t->kind == TYPE_UNION) return t->size;
    if (t->kind == TYPE_CHAR || t->kind == TYPE_BOOL) return 1;
    if (t->kind == TYPE_SHORT) return 2;
    if (t->kind == TYPE_DOUBLE || t->kind == TYPE_LLONG) return 8;
    return 4;
}
StructField *type_find_field(Type *t, const char *name) {
    StructField *f;
    for (f=t->fields; f; f=f->next)
        if (f->name && strcmp(f->name,name)==0) return f;
    return 0;
}
int main(void) {
    Type byte = {.kind=TYPE_CHAR,.is_unsigned=1};
    Type array = {.kind=TYPE_ARRAY,.base=&byte,.size=3};
    Type nested = {.kind=TYPE_ARRAY,.base=&array,.size=1};
    const char *texts[] = {
        "{((0x20)|1|(0x40)),((0x30)|2),((0x20)|8|(0x80))}",
        "{97,50,168}", "{{97,50,424}}"
    };
    int i;
    for (i=0; i<3; i++) {
        unsigned char out[3] = {0,0,0};
        int rc=ark_agg_fill(i==2 ? &nested : &array,texts[i],out,3);
        printf("case=%d rc=%d bytes=%u,%u,%u\n",i,rc,out[0],out[1],out[2]);
        if (i==0 ? rc!=-1 : rc!=0 || out[0]!=97 || out[1]!=50 || out[2]!=168)
            return 10+i;
    }
    return 0;
}
'''


CHECKED_HARNESS = HARNESS.split('int main(void)')[0] + r'''
#include "xh_const_int.h"
#include "symtab.h"
static Type uint_t={.kind=TYPE_INT,.is_unsigned=1};
static Type ll_t={.kind=TYPE_LLONG};
static Type ull_t={.kind=TYPE_LLONG,.is_unsigned=1};
Type *usual_arith_conv(Type *a,Type *b) {
    int wide=type_size(a)==8 || type_size(b)==8;
    int u=(type_size(a)==type_size(b)) ? a->is_unsigned||b->is_unsigned :
          type_size(a)>type_size(b) ? a->is_unsigned : b->is_unsigned;
    return wide ? (u?&ull_t:&ll_t) : (u?&uint_t:&int_t);
}
Symbol *sym_find(const char *name) { (void)name; return 0; }
static AstNode number(long long n,Type *t) { AstNode a={0}; a.kind=AST_NUM;a.type=t;a.u.num_val=n;return a; }
static AstNode binary(int op,AstNode *a,AstNode *b) { AstNode n={0};n.kind=AST_BINARY;n.u.binary.op=op;n.u.binary.left=a;n.u.binary.right=b;return n; }
#define CHECK(n,rc,b) do { XhConstInt v; int got=xh_const_int_eval(&(n),&v); if(got!=(rc) || (got==1 && v.bits!=(unsigned long long)(b))) {printf("line=%d rc=%d bits=%llu\n",__LINE__,got,got==1?v.bits:0);return 1;} } while(0)
int main(void) {
    Type byte={.kind=TYPE_CHAR,.is_unsigned=1}, ptr={.kind=TYPE_PTR};
    AstNode a=number(32,&int_t),b=number(1,&int_t),c=number(64,&int_t);
    AstNode ab=binary(TOK_BOR,&a,&b),abc=binary(TOK_BOR,&ab,&c);
    CHECK(abc,1,97);
    a.u.num_val=424; AstNode cast={0};cast.kind=AST_CAST;cast.type=&byte;cast.u.unary.operand=&a;
    CHECK(cast,1,168);
    { XhConstInt v; if(xh_const_int_eval(&cast,&v)!=1 || xh_const_int_signed(v)!=168) return 2; }
    cast.type=&ptr; CHECK(cast,0,0);cast.type=&byte;
    a.u.num_val=1;b.u.num_val=31;ab=binary(TOK_SHL,&a,&b);CHECK(ab,-1,0);
    a.type=&uint_t;CHECK(ab,1,2147483648ULL);
    b.u.num_val=32;CHECK(ab,-1,0);b.u.num_val=-1;CHECK(ab,-1,0);
    a=number(2147483647,&int_t);b=number(1,&int_t);ab=binary(TOK_PLUS,&a,&b);CHECK(ab,-1,0);
    a=number(-9223372036854775807LL-1,&ll_t);b=number(-1,&ll_t);ab=binary(TOK_DIV,&a,&b);CHECK(ab,-1,0);
    a=number(-1,&ull_t);b=number(1,&ull_t);ab=binary(TOK_PLUS,&a,&b);CHECK(ab,1,0);
    a=number(7,&int_t);b=number(0,&int_t);ab=binary(TOK_DIV,&a,&b);CHECK(ab,-1,0);
    AstNode cond=number(0,&int_t),logic=binary(TOK_AND,&cond,&ab);CHECK(logic,1,0);
    a=number(424,&int_t);
    AstNode ternary={0};ternary.kind=AST_TERNARY;ternary.u.ternary.cond=&cond;
    ternary.u.ternary.true_expr=&ab;ternary.u.ternary.false_expr=&cast;CHECK(ternary,1,168);
    a=number(-8,&int_t);b=number(2,&int_t);ab=binary(TOK_SHR,&a,&b);CHECK(ab,1,4294967294ULL);
    ab=binary(TOK_LT,&a,&b);CHECK(ab,1,1);
    a=number(-1,&ull_t); { XhConstInt v; if(xh_const_int_eval(&a,&v)!=1 || xh_const_int_signed(v)!=-1) return 3; }
    AstNode sz=number(4,&uint_t);sz.kind=AST_SIZEOF;CHECK(sz,1,4);sz.sizeof_is_vla=2;CHECK(sz,0,0);
    AstNode addr={0};addr.kind=AST_UNARY;addr.u.unary.op=TOK_BAND;addr.u.unary.operand=&a;CHECK(addr,0,0);
    puts("checked AST arithmetic, shifts, casts, sizeof and address rejection: OK");return 0;
}
'''


class AggregateConstexprSourceHarness(unittest.TestCase):
    def test_actual_aggregate_reader_and_folded_byte_representation(self):
        self.run_harness(HARNESS, 'agginit.c')

    def test_checked_ast_integer_evaluation(self):
        self.run_harness(CHECKED_HARNESS, 'const_int.c')

    def run_harness(self, source, implementation):
        cc = os.environ.get('HOST_CC') or shutil.which('gcc')
        if not cc:
            fallback = Path(r'D:\msys64\ucrt64\bin\gcc.exe')
            cc = str(fallback) if fallback.is_file() else None
        if not cc:
            self.skipTest('Set HOST_CC to a native GCC-compatible compiler')
        with tempfile.TemporaryDirectory(prefix='aggregate-constexpr-') as directory:
            src = Path(directory) / 'harness.c'
            exe = Path(directory) / ('harness.exe' if os.name == 'nt' else 'harness')
            src.write_text(source, encoding='utf-8')
            env = dict(os.environ)
            env['PATH'] = str(Path(cc).resolve().parent) + os.pathsep + env.get('PATH', '')
            result = subprocess.run([cc, '-std=c99', '-Wall', '-Wextra', '-I', str(ROOT / 'include'),
                                     str(src), str(ROOT / 'src/frontend/c' / implementation), '-o', str(exe)],
                                    env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], env=env, capture_output=True, text=True, timeout=30)
            print(result.stdout, end='', flush=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class AggregateConstexprArmElf(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from test_driver import command
        cls.command = command()
        cls.workspace = tempfile.TemporaryDirectory(prefix='aggregate-elf-', dir=ROOT)
        cls.directory = Path(cls.workspace.name)

    @classmethod
    def tearDownClass(cls):
        cls.workspace.cleanup()

    def compile(self, source, stem):
        src = self.directory / (stem + '.c')
        obj = src.with_suffix('.o')
        src.write_text(source, encoding='utf-8')
        result = subprocess.run([*self.command, 'arm-none-eabi-gcc', '-std=c99', '-O0', '-c',
                                 src.relative_to(ROOT).as_posix(), '-o', obj.relative_to(ROOT).as_posix()],
                                cwd=ROOT, capture_output=True, text=True, timeout=120)
        return src, obj, result

    def test_arm_elf_constant_bytes_match_gnu_and_address_relocation_survives(self):
        from elftools.elf.elffile import ELFFile
        source = r'''
typedef unsigned char uint8_t;
enum { FLAG = 0x40, SHIFT = 3 };
const uint8_t rule_act_table[] = {((0x20)|1|(0x40)), ((0x30)|2), ((0x20)|8|(0x80))};
struct Nested { uint8_t bytes[3]; signed char signed_byte; unsigned long long wide; };
const struct Nested nested = {
    .bytes = {[2]=(uint8_t)424, [0]=(1u<<SHIFT)|FLAG, [1]=sizeof(unsigned int)+1},
    .signed_byte=(signed char)255,
    .wide=((1ULL<<63)|7ULL)
};
int external_value;
struct Ref { uint8_t code; int *address; } reference = {.code=(FLAG|1),.address=&external_value};
'''
        src, obj, result = self.compile(source, 'valid')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        arm = Path(os.environ.get('XH_C_ARM_CC', r'D:\arm-none-eabi\bin\arm-none-eabi-gcc.exe'))
        self.assertTrue(arm.is_file(), 'Set XH_C_ARM_CC for the GNU ARM reference compiler')
        oracle = src.with_name('gnu.o')
        reference = subprocess.run([str(arm), '-std=c99', '-O0', '-c', str(src), '-o', str(oracle)],
                                   capture_output=True, text=True, timeout=60)
        self.assertEqual(reference.returncode, 0, reference.stdout + reference.stderr)

        def inspect(path):
            with path.open('rb') as stream:
                elf = ELFFile(stream)
                self.assertEqual(elf['e_machine'], 'EM_ARM')
                symtab = elf.get_section_by_name('.symtab')
                blobs = {}
                for name in ('rule_act_table', 'nested', 'reference'):
                    found = symtab.get_symbol_by_name(name)
                    self.assertTrue(found, name)
                    symbol = found[0]
                    section = elf.get_section(symbol['st_shndx'])
                    offset = symbol['st_value'] - section['sh_addr']
                    blobs[name] = section.data()[offset:offset + symbol['st_size']]
                refs = []
                for section in elf.iter_sections():
                    if section['sh_type'] != 'SHT_REL':
                        continue
                    symbols = elf.get_section(section['sh_link'])
                    for relocation in section.iter_relocations():
                        refs.append((symbols.get_symbol(relocation['r_info_sym']).name,
                                     relocation['r_info_type']))
                return blobs, refs

        actual, relocations = inspect(obj)
        expected, oracle_relocations = inspect(oracle)
        self.assertEqual(actual, expected)
        self.assertEqual(actual['rule_act_table'], bytes([97, 50, 168]))
        self.assertEqual(actual['nested'][:4], bytes([72, 5, 168, 255]))
        self.assertIn(('external_value', 2), relocations)  # R_ARM_ABS32
        self.assertIn(('external_value', 2), oracle_relocations)

    def test_invalid_constant_division_is_rejected(self):
        _, obj, result = self.compile('const unsigned char broken[] = {(1 / 0)};', 'invalid')
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertRegex((result.stdout + result.stderr).lower(), r'initial|constant|division')
        self.assertFalse(obj.exists(), 'Invalid constant must not produce a successful ELF object')


if __name__ == '__main__':
    unittest.main(verbosity=2)
