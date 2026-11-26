"""Tentative globals: isolated sema harness and coordinated ARM ELF regression.

Never rebuilds the production compiler. Set XHCC_COMMAND for the fresh root
compiler when running TentativeGlobalObjects after the coordinated build.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from test_driver import ROOT, command

sys.path.insert(0, str(ROOT / 'build/test-deps'))

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include "sema.h"
#include "symtab.h"
#include "memory.h"
#include "diag.h"
int g_irgen_no_builtin = 1;
void ark_log(const char *fmt, ...) { (void)fmt; }
void ark_abort(const char *s) { fprintf(stderr, "%s\n", s); exit(90); }
void diag_report(int code, DiagSeverity severity, const DiagLoc *loc,
                 const char *a, const char *b, const char *c) {
    (void)code; (void)severity; (void)loc; (void)a; (void)b; (void)c;
}
void diag_report_msg(DiagSeverity severity, const DiagLoc *loc, const char *fmt,
                     const char *a, const char *b, const char *c) {
    (void)severity; (void)loc; (void)fmt; (void)a; (void)b; (void)c;
}
static AstNode *var(Type *t, int stat, int ext, int init) {
    AstNode *n = ast_new(AST_VAR_DECL, 1);
    n->u.var.name = "value"; n->u.var.var_type = t;
    n->u.var.is_static = stat; n->u.var.is_extern = ext;
    if (init) { n->u.var.init = ast_new(AST_NUM, 1); n->u.var.init->u.num_val = 23; }
    return n;
}
static int check(AstNode **nodes, int count) {
    AstNode *p = ast_new(AST_PROGRAM, 1);
    p->u.program.decls = nodes; p->u.program.decl_count = count;
    return sema_check(p);
}
#define CHECK(x) do { if (!(x)) { printf("failed line %d\n", __LINE__); return 1; } } while (0)
int main(void) {
    AstNode *n[4]; Type *t, *a, *b; int i;
    mem_pool_reset();
    for (i = 0; i < 3; i++) {
        n[0] = var(type_int(),0,0,i==0); n[1] = var(type_int(),0,0,i==1);
        n[2] = var(type_int(),0,0,i==2); CHECK(check(n,3)==0);
        CHECK(sym_find("value")->is_defined==1);
        CHECK(n[0]->u.var.is_global && n[1]->u.var.is_global && n[2]->u.var.is_global);
    }
    n[0]=var(type_int(),0,0,0);n[1]=var(type_int(),0,0,0);
    CHECK(check(n,2)==0 && !sym_find("value")->is_defined);
    n[0]=var(type_int(),0,1,0);n[1]=var(type_int(),0,0,1);n[2]=var(type_int(),0,1,0);
    CHECK(check(n,3)==0 && sym_find("value")->is_defined && !sym_find("value")->is_extern);
    n[0]=var(type_int(),0,1,1);CHECK(check(n,1)==0 && !n[0]->u.var.is_extern);
    n[0]=var(type_int(),1,0,0);n[1]=var(type_int(),0,1,0);n[2]=var(type_int(),1,0,1);
    CHECK(check(n,3)==0 && n[1]->u.var.is_static && sym_find("value")->is_static);
    n[0]=var(type_int(),0,0,1);n[1]=var(type_int(),0,0,1);CHECK(check(n,2)>0);
    n[0]=var(type_int(),0,1,0);n[1]=var(type_long(),0,0,0);CHECK(check(n,2)>0);
    n[0]=var(type_int(),1,0,0);n[1]=var(type_int(),0,0,0);CHECK(check(n,2)>0);
    n[0]=var(type_int(),0,1,0);n[1]=var(type_int(),1,0,0);CHECK(check(n,2)>0);
    t=type_qualified(type_int(),0,1);
    n[0]=var(t,1,0,0);n[1]=var(t,1,0,1);CHECK(check(n,2)==0);
    n[0]=var(t,0,1,0);n[1]=var(type_int(),0,0,0);CHECK(check(n,2)>0);
    a=type_array(type_int(),0);b=type_array(type_int(),2);
    n[0]=var(a,0,1,0);n[1]=var(b,0,0,1);n[2]=var(a,0,0,0);
    CHECK(check(n,3)==0 && n[2]->u.var.var_type==b && sym_find("value")->type==b);
    n[0]=var(b,0,0,0);n[1]=var(type_array(type_int(),3),0,1,0);CHECK(check(n,2)>0);
    a=type_struct("Outer",0);b=type_struct("Outer",0);
    type_struct_add_field(a,"next",type_pointer(a));type_struct_add_field(b,"next",type_pointer(b));
    n[0]=var(a,1,0,0);n[1]=var(b,1,0,1);CHECK(check(n,2)==0);
    n[0]=var(type_pointer(a),0,1,0);n[1]=var(type_pointer(type_struct("Other",0)),0,0,0);
    CHECK(check(n,2)>0);
    /* Every accepted later definition still gets initialization conversion. */
    n[0]=var(type_double(),0,1,0);n[1]=var(type_double(),0,0,1);
    CHECK(check(n,2)==0 && n[1]->u.var.init->kind==AST_CAST);
    /* Locals keep the separate strict same-scope declaration rule. */
    { AstNode *fn=ast_new(AST_FUNC_DEF,1), *body=ast_new(AST_BLOCK,1);
      AstNode *locals[2];locals[0]=var(type_int(),0,0,0);locals[1]=var(type_int(),0,0,0);
      body->u.block.stmts=locals;body->u.block.stmt_count=2;
      fn->u.func.name="entry";fn->u.func.ret_type=type_int();fn->u.func.body=body;
      n[0]=fn;CHECK(check(n,1)>0); }
    puts("tentative globals sema PASS");return 0;
}
'''


class TentativeGlobalSema(unittest.TestCase):
    def test_isolated_sema(self):
        cc = os.environ.get('HOST_CC') or shutil.which('gcc') or shutil.which('clang')
        if not cc and Path(r'D:\msys64\ucrt64\bin\gcc.exe').is_file():
            cc = r'D:\msys64\ucrt64\bin\gcc.exe'
        self.assertTrue(cc, 'Set HOST_CC to a native GCC-compatible compiler')
        env = os.environ.copy()
        env['PATH'] = str(Path(cc).resolve().parent) + os.pathsep + env.get('PATH', '')
        with tempfile.TemporaryDirectory(prefix='tentative-sema-') as tmp:
            source = Path(tmp) / 'sema-test.c'
            executable = Path(tmp) / ('sema-test.exe' if os.name == 'nt' else 'sema-test')
            source.write_text(HARNESS, encoding='utf-8')
            sources = ['src/frontend/c/sema.c', 'src/frontend/c/symtab.c',
                       'src/frontend/c/type.c', 'src/frontend/c/ast.c', 'src/core/memory.c']
            built = subprocess.run([cc, '-std=c99', '-I' + str(ROOT / 'include'),
                                    str(source), *[str(ROOT / p) for p in sources],
                                    '-o', str(executable)], env=env, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            run = subprocess.run([str(executable)], env=env, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('tentative globals sema PASS', run.stdout)


class TentativeGlobalObjects(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.command = command()
        cls.workspace = tempfile.TemporaryDirectory(prefix='tentative-objects-', dir=ROOT)
        cls.addClassCleanup(cls.workspace.cleanup)
        cls.directory = Path(cls.workspace.name)

    def compile(self, source, success=True):
        src = self.directory / 'case.c'
        obj = self.directory / 'case.o'
        if obj.exists():
            obj.unlink()
        src.write_text(source + '\n', encoding='utf-8')
        result = subprocess.run([*self.command, 'arm-none-eabi-gcc', '-std=c99', '-O0',
                                 '-c', src.relative_to(ROOT).as_posix(), '-o',
                                 obj.relative_to(ROOT).as_posix()], cwd=ROOT,
                                capture_output=True, text=True, timeout=120)
        diagnostic = result.stdout + result.stderr
        self.assertEqual(result.returncode == 0, success, diagnostic)
        if not success:
            self.assertFalse(obj.exists(), diagnostic)
        return obj, diagnostic

    def inspect(self, obj, expected):
        from elftools.elf.elffile import ELFFile
        with obj.open('rb') as stream:
            elf = ELFFile(stream)
            self.assertEqual(elf['e_machine'], 'EM_ARM')
            for name, (section_name, values, binding) in expected.items():
                symbols = elf.get_section_by_name('.symtab').get_symbol_by_name(name)
                self.assertEqual(len(symbols or []), 1, name)
                symbol = symbols[0]
                section = elf.get_section(symbol['st_shndx'])
                self.assertEqual(section.name, section_name, name)
                self.assertEqual(symbol['st_info']['bind'], binding, name)
                expected_bytes = b''.join(v.to_bytes(4, 'little') for v in values)
                self.assertEqual(symbol['st_size'], len(expected_bytes), name)
                off = symbol['st_value'] - section['sh_addr']
                self.assertEqual(section.data()[off:off + symbol['st_size']], expected_bytes, name)

    def test_initializer_before_between_after_tentatives_and_bss(self):
        obj, _ = self.compile('''int before=17; int before; int before;
int middle; int middle=23; int middle;
int after; int after; int after=29;
int zero; int zero; extern int zero;
extern int header; int header=31; extern int header;
''')
        expected = {name: ('.data', [value], 'STB_GLOBAL') for name, value in
                    [('before', 17), ('middle', 23), ('after', 29), ('header', 31)]}
        expected['zero'] = ('.bss', [0], 'STB_GLOBAL')
        self.inspect(obj, expected)

    def test_micropython_static_const_shape_and_inherited_linkage(self):
        obj, _ = self.compile('''typedef struct Type Type;
struct Type { const Type *base; int flags; };
static const Type mp_type_dict_view_it;
static const Type mp_type_dict_view;
static const Type mp_type_dict_view_it={0,41};
static const Type mp_type_dict_view={0,43};
static int internal; extern int internal; static int internal=47;
''')
        # The existing standard ARM writer places every initialized object in
        # .data, including const objects. This fixture checks declaration merge,
        # payload and local binding; it does not claim read-only placement.
        self.inspect(obj, {'mp_type_dict_view_it': ('.data', [0, 41], 'STB_LOCAL'),
                           'mp_type_dict_view': ('.data', [0, 43], 'STB_LOCAL'),
                           'internal': ('.data', [47], 'STB_LOCAL')})

    def test_incomplete_array_is_completed_and_preserved(self):
        obj, _ = self.compile('''extern int inferred[];
int inferred[]={53,59}; extern int inferred[];
int bounded[]; int bounded[2]={61,67}; int bounded[];
''')
        self.inspect(obj, {'inferred': ('.data', [53, 59], 'STB_GLOBAL'),
                           'bounded': ('.data', [61, 67], 'STB_GLOBAL')})

    def test_invalid_redeclarations_are_diagnosed(self):
        cases = [
            ('int x=1; int x=2;', None),
            ('extern int x; int x=1; int x=2;', None),
            ('int entry(void) { int x; int x; return 0; }', None),
            ('extern int x; long x;', 'conflicting types'),
            ('int x[2]; extern int x[3];', 'conflicting types'),
            ('extern const int x; int x;', 'conflicting types'),
            ('extern const int *x; int *x;', 'conflicting types'),
            ('struct A {int x;}; struct B {int x;}; extern struct A x; struct B x;', 'conflicting types'),
            ('static int x; int x;', 'conflicting linkage'),
            ('int x; static int x;', 'conflicting linkage'),
            ('extern int x; static int x;', 'conflicting linkage'),
        ]
        for source, message in cases:
            with self.subTest(source=source):
                _, diagnostic = self.compile(source, success=False)
                if message:
                    self.assertIn(message, diagnostic)


if __name__ == '__main__':
    unittest.main()
