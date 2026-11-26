"""Source harness: GNU static flexible tail extent, bounds, and zero padding.
Run independently; does not rebuild the compiler or touch MicroPython sources.
"""
import os
import sys

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'build/test-deps'))
HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "agginit.h"
void *mem_pool_alloc(int n) { return calloc(1,(size_t)n); }
static Type integer={.kind=TYPE_INT};
Type *type_int(void) { return &integer; }
int type_size(Type *t) {
 if (!t) return 4;
 if (t->kind==TYPE_ARRAY) return t->is_zero_array?0:type_size(t->base)*(t->size>0?t->size:1);
 if (t->kind==TYPE_STRUCT) return (t->size+3)&~3;
 if (t->kind==TYPE_CHAR) return 1;
 return 4;
}
StructField *type_find_field(Type *t,const char *s) {
 StructField *f; for(f=t->fields;f;f=f->next) if(f->name&&!strcmp(f->name,s))return f; return 0;
}
static int hook(void *ud,int off,Type *t) { int *n=ud; if(t->kind!=TYPE_PTR||off<20||off>=40||off%4)return 0; ++*n;return 1; }
#define CHECK(x) do{if(!(x)){printf("failed line %d\n",__LINE__);return 1;}}while(0)
int main(void) {
 Type ptr={.kind=TYPE_PTR},tail={.kind=TYPE_ARRAY,.base=&ptr};
 StructField b={.name="slots",.type=&tail,.offset=20};
 StructField a={.name="head",.type=&integer,.offset=0,.next=&b};
 Type s={.kind=TYPE_STRUCT,.size=20,.fields=&a};
 unsigned char bytes[64]={0};int refs=0,i;
 const char *text="{.head=7,.slots={&a,&b,&c,&d,&e}}";
 CHECK(ark_agg_storage_size(&s,text)==40);
 CHECK(s.size==20&&tail.size==0);
 CHECK(ark_agg_fill_ex(&s,text,bytes,40,hook,&refs)==0&&refs==5);
 CHECK(bytes[0]==7);for(i=1;i<40;i++)CHECK(bytes[i]==0);
 CHECK(ark_agg_fill_ex(&s,text,bytes,20,hook,&refs)==-1);
 CHECK(ark_agg_storage_size(&s,"{.slots={[4]=0,[1]=0}}") ==40);
 CHECK(ark_agg_storage_size(&s,"{.slots={[536870912]=0}}") ==-1);
 tail.is_zero_array=1;CHECK(ark_agg_fill(&s,"{.slots={1}}",bytes,40)==-1);tail.is_zero_array=0;
 { Type ch={.kind=TYPE_CHAR};tail.base=&ch;b.offset=4;s.size=4;
 CHECK(ark_agg_storage_size(&s,"{.slots={65}}") ==5);
 CHECK(ark_agg_fill(&s,"{.slots={65}}",bytes,5)==0&&bytes[4]==65);
 CHECK(ark_agg_storage_size(&s,"{.slots=\"ab\"}") ==7);
 }
 puts("flex extent40 refs5 nominal20 padding/bounds/designator/char-tail PASS");return 0;
}
'''

class FlexibleStaticInit(unittest.TestCase):
    def test_source_storage_contract(self):
        cc=os.environ.get('HOST_CC') or shutil.which('gcc') or shutil.which('clang')
        if not cc:
            candidate = Path(r'D:\msys64\ucrt64\bin\gcc.exe')
            if candidate.is_file():
                cc = str(candidate)
        self.assertTrue(cc, 'Set HOST_CC to a native GCC-compatible compiler')
        env = os.environ.copy()
        env['PATH'] = str(Path(cc).resolve().parent) + os.pathsep + env.get('PATH', '')
        with tempfile.TemporaryDirectory(prefix='xhcc-flex-') as tmp:
            src=Path(tmp)/'flex.c';exe=Path(tmp)/'flex.exe'
            src.write_text(HARNESS,encoding='utf-8')
            subprocess.run([cc,'-std=c99','-Wall','-Wextra','-Werror','-I'+str(ROOT/'include'),str(src),str(ROOT/'src/frontend/c/agginit.c'),'-o',str(exe)],check=True,env=env)
            subprocess.run([str(exe)],check=True,env=env)

class FlexibleStaticObjects(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from test_driver import command
        sys.path.insert(0, str(ROOT / 'build/test-deps'))
        cls.command = command()
        cls.workspace = tempfile.TemporaryDirectory(prefix='flex-objects-', dir=ROOT)
        cls.directory = Path(cls.workspace.name)

    @classmethod
    def tearDownClass(cls):
        cls.workspace.cleanup()

    def compile(self, text, stem, driver='arm-none-eabi-gcc'):
        source = self.directory / (stem + '.c')
        obj = source.with_suffix('.o')
        source.write_text(text, encoding='utf-8')
        result = subprocess.run([*self.command, driver, '-std=c99', '-O0', '-c',
                                 source.relative_to(ROOT).as_posix(), '-o', obj.relative_to(ROOT).as_posix()],
                                cwd=ROOT, capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return source, obj

    def inspect(self, obj, names):
        from elftools.elf.elffile import ELFFile
        with obj.open('rb') as stream:
            elf = ELFFile(stream)
            symbols = elf.get_section_by_name('.symtab')
            blobs, locations, refs = {}, {}, {}
            for name in names:
                found = symbols.get_symbol_by_name(name)
                self.assertTrue(found, name)
                sym = found[0]
                section = elf.get_section(sym['st_shndx'])
                off = sym['st_value'] - section['sh_addr']
                blobs[name] = section.data()[off:off + sym['st_size']]
                locations[name] = (sym['st_shndx'], off, sym['st_size'])
                refs[name] = []
            for section in elf.iter_sections():
                if section['sh_type'] not in ('SHT_REL', 'SHT_RELA'):
                    continue
                table = elf.get_section(section['sh_link'])
                for relocation in section.iter_relocations():
                    for name, (sec, off, size) in locations.items():
                        at = relocation['r_offset']
                        if section['sh_info'] == sec and off <= at < off + size:
                            refs[name].append((at - off, table.get_symbol(relocation['r_info_sym']).name,
                                               relocation['r_info_type']))
            return blobs, locations, refs, elf['e_machine']

    def test_arm_extent_and_five_relocations_match_gnu(self):
        text = '''extern void a(void); extern void b(void); extern void c(void); extern void d(void); extern void e(void);
typedef struct S S;
extern const S mp;
struct S { int head[5]; void (*slots[])(void); };
const S mp = {{1,2,3,4,5},{a,b,c,d,e}};
int nominal = sizeof(struct S);
'''
        source, actual = self.compile(text, 'arm-references')
        gnu = Path(os.environ.get('XH_C_ARM_CC', r'D:\arm-none-eabi\bin\arm-none-eabi-gcc.exe'))
        self.assertTrue(gnu.is_file(), 'Set XH_C_ARM_CC to the GNU ARM reference compiler')
        oracle = source.with_name('gnu.o')
        result = subprocess.run([str(gnu), '-std=c99', '-O0', '-c', str(source), '-o', str(oracle)],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        blobs, places, refs, machine = self.inspect(actual, ('mp', 'nominal'))
        expected, _, expected_refs, _ = self.inspect(oracle, ('mp', 'nominal'))
        self.assertEqual(machine, 'EM_ARM')
        self.assertEqual(blobs, expected)
        self.assertEqual(places['mp'][2], 40)
        self.assertEqual(int.from_bytes(blobs['nominal'], 'little'), 20)
        self.assertEqual(refs, expected_refs)
        self.assertEqual(refs['mp'], [(20 + 4*i, name, 2) for i, name in enumerate('abcde')])

    def test_distinct_instances_and_char_tail_across_object_writers(self):
        text = '''struct S { int head; int tail[]; };
struct S short_one={7,{11}}, long_one={9,{[3]=44,[1]=22}};
struct C { int head; char tail[]; };
struct C chars={3,{65}};
int after_chars=99;
int nominal=sizeof(struct S);
'''
        expected = {
            'short_one': b''.join(n.to_bytes(4, 'little') for n in (7, 11)),
            'long_one': b''.join(n.to_bytes(4, 'little') for n in (9, 0, 22, 0, 44)),
            'chars': (3).to_bytes(4, 'little') + b'A',
            'after_chars': (99).to_bytes(4, 'little'),
            'nominal': (4).to_bytes(4, 'little'),
        }
        for target, machine in [('arm-none-eabi-gcc', 'EM_ARM'), ('riscv32-unknown-elf-gcc', 'EM_RISCV'),
                                ('x86_64-elf-gcc', 'EM_X86_64')]:
            with self.subTest(target=target):
                _, obj = self.compile(text, target + '-instances', target)
                blobs, places, refs, actual_machine = self.inspect(obj, tuple(expected))
                self.assertEqual(actual_machine, machine)
                self.assertEqual(blobs, expected)
                self.assertEqual(places['chars'][2], 5)
                self.assertEqual(places['after_chars'][1] % 4, 0)
                self.assertTrue(all(not entries for entries in refs.values()))

if __name__=='__main__':unittest.main()
