"""Standalone bounded ld-script parser/evaluator tests; never builds main app.

The layout oracle below is a tiny test consumer, not the production linker.
Production consumption is tested separately by test_elf_link.py.
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import unittest

if os.name == 'nt':
    _python_dir = Path(sys.executable).parent
    os.environ['PATH'] = os.pathsep.join([str(_python_dir), str(_python_dir / 'Library/bin'), os.environ.get('PATH', '')])

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/ld-script-tests'
SDK = Path(os.environ.get('WASI_SDK_PATH', r'D:\wasi-sdk-34.0-x86_64-windows'))
GNU = Path(os.environ.get('ARM_BIN', r'D:\arm-none-eabi\bin'))
EXE = '.exe' if os.name == 'nt' else ''
WASMTIME = os.environ.get('WASMTIME') or shutil.which('wasmtime')
FIXTURE = '''/* Minimal Cortex-M layout; sizes are KiB, addresses hexadecimal. */
ENTRY(Reset_Handler)
MEMORY {
 FLASH(rx): ORIGIN = 0x08000000, LENGTH = 512K
 RAM(rwx): ORIGIN = 0x20000000, LENGTH = 128K
}
_estack = ORIGIN(RAM) + LENGTH(RAM);
SECTIONS {
 .text : { KEEP(*(.isr_vector)) *(.text*) *(.rodata*) . = ALIGN(4); } > FLASH
 .data : { _sdata = .; *(.data*) _edata = .; } > RAM AT> FLASH
 _sidata = LOADADDR(.data);
 .bss (NOLOAD) : { __bss_start__ = .; *(.bss*) *(COMMON) __bss_end__ = .; } > RAM
 _data_size = SIZEOF(.data);
 _data_addr = ADDR(.data);
}
'''
HARNESS = r'''
#include "xh_ld_script.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static XhLdScript ir;
static uint32_t addr[16],lma[16],size[16],values[64];
static unsigned known[64];
static int lookup(void *unused,int kind,const char *name,uint32_t *out) {
 unsigned i;(void)unused;
 if(kind==XH_LD_SYMBOL) {for(i=0;i<ir.symbol_count;i++)if(known[i]&&!strcmp(name,ir.symbols[i].name)){*out=values[i];return 0;}return -1;}
 for(i=0;i<ir.section_count;i++)if(!strcmp(name,ir.sections[i].name)) {*out=kind==XH_LD_ADDR?addr[i]:kind==XH_LD_LOADADDR?lma[i]:size[i];return 0;}return -1;
}
static int ev(int id,uint32_t dot,uint32_t *out) {char err[256];int rc=xh_ld_script_eval(&ir,id,dot,lookup,0,out,err,sizeof(err));if(rc)fprintf(stderr,"%s\n",err);return rc;}
int main(int argc,char **argv) {
 FILE *f;static char text[65538];char err[256];size_t n;unsigned i,j;uint32_t cursor[8],dot,v;int rc;
 if(argc<2)return 2;f=fopen(argv[1],"rb");if(!f)return 2;n=fread(text,1,sizeof(text),f);fclose(f);
 rc=xh_ld_script_parse(text,n,&ir,err,sizeof(err));if(rc){fprintf(stderr,"%s\n",err);return 1;}
 printf("IR %lu %u %u %u %u %u\n",(unsigned long)sizeof(ir),ir.region_count,ir.section_count,ir.pattern_count,ir.symbol_count,ir.expr_count);
 for(i=0;i<ir.region_count;i++)printf("R %s %lu %lu\n",ir.regions[i].name,(unsigned long)ir.regions[i].origin,(unsigned long)ir.regions[i].length);
 if(argc<3)return 0;
 if(!strcmp(argv[2],"eval")) {
   for(i=0;i<ir.symbol_count;i++){if(ev(ir.symbols[i].expr,3,&v))return 1;printf("S %s %lu\n",ir.symbols[i].name,(unsigned long)v);}return 0;
 }
 /* Fixed input sections also emitted by the GNU oracle fixture below. */
 for(i=0;i<ir.region_count;i++)cursor[i]=ir.regions[i].origin;
 for(i=0;i<ir.section_count;i++) {
   XhLdSection *s=&ir.sections[i];dot=cursor[s->region];
   if(s->address_expr>=0&&ev(s->address_expr,dot,&dot))return 1;
   if(s->align_expr>=0&&ev(s->align_expr,dot,&dot))return 1;
   addr[i]=dot;lma[i]=cursor[s->load_region];
   for(j=s->first_event;j<s->first_event+s->event_count;j++) {
     XhLdEvent *e=&ir.events[j];
     if(e->kind==XH_LD_PATTERN) {
       static const char *names[]={".isr_vector",".text.fn",".rodata.str",".data.value",".bss.zero","COMMON"};
       static const unsigned sizes[]={8,12,4,8,12,4};unsigned k;
       for(k=0;k<6;k++)if(xh_ld_script_match(&ir.patterns[e->index],names[k],k==5))dot+=sizes[k];
     } else if(e->kind==XH_LD_SET_DOT) {if(ev(e->index,dot,&dot))return 1;}
     else {if(ev(ir.symbols[e->index].expr,dot,&values[e->index]))return 1;known[e->index]=1;}
   }
   size[i]=dot-addr[i];cursor[s->region]=dot;
   if(!s->noload&&s->load_region!=s->region)cursor[s->load_region]+=size[i];
   printf("O %s %lu %lu %lu\n",s->name,(unsigned long)addr[i],(unsigned long)lma[i],(unsigned long)size[i]);
 }
 for(i=0;i<ir.symbol_count;i++) {if(!known[i]&&ev(ir.symbols[i].expr,0,&values[i]))return 1;printf("S %s %lu\n",ir.symbols[i].name,(unsigned long)values[i]);}
 return 0;
}
'''


class ScriptTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        (OUT / 'harness.c').write_text(HARNESS, encoding='utf-8')
        cls.run_command([SDK / ('bin/clang' + EXE), '--target=wasm32-wasip1',
                         '-std=c99', '-O1', '-Wall', '-Wextra', '-Werror',
                         '-fsanitize=undefined', '-fsanitize-trap=all', '-Iinclude',
                         OUT / 'harness.c', 'src/link/ld_script.c', '-o', OUT / 'script.wasm'])

    @staticmethod
    def run_command(args, ok=True):
        p = subprocess.run(list(map(str, args)), cwd=ROOT, capture_output=True,
                           text=True, timeout=60)
        if (p.returncode == 0) != ok:
            raise AssertionError(p.stdout + p.stderr)
        return p

    def parse(self, text, mode=None, ok=True):
        path = OUT / 'input.ld'
        path.write_bytes(text.encode('ascii'))
        args = [WASMTIME, 'run', '--dir=.', OUT / 'script.wasm', path.relative_to(ROOT)]
        if mode:
            args.append(mode)
        result = self.run_command(args, ok)
        if not ok:
            self.assertIn('ld script', result.stderr)
            self.assertNotIn('wasm trap', result.stderr)
        return result

    def test_real_cortex_m_fixture(self):
        p = self.parse(FIXTURE, 'layout')
        header = p.stdout.splitlines()[0].split()
        self.assertLess(int(header[1]), 32768)
        self.assertEqual(header[2:6], ['2', '3', '6', '8'])
        self.assertIn('S _sidata 134217752', p.stdout)
        self.assertIn('S _sdata 536870912', p.stdout)
        self.assertIn('S _edata 536870920', p.stdout)
        self.assertIn('S __bss_end__ 536870936', p.stdout)
        self.assertIn('S _estack 537001984', p.stdout)

    def test_expressions_and_comments(self):
        text = FIXTURE.replace('512K', '(1M / 2)').replace('128K', '64K * 2')
        text += '\nx = (2 + 3) * 4 - 1; y = ALIGN(8); z = +0; octal=010;'
        p = self.parse(text, 'eval')
        self.assertIn('S x 19', p.stdout)
        self.assertIn('S y 8', p.stdout)
        self.assertIn('S octal 8', p.stdout)
        self.parse(FIXTURE.replace('128K', '128K;'))
        # Earlier region references are evaluated while parsing.
        self.parse(FIXTURE.replace('0x20000000', 'ORIGIN(FLASH) + LENGTH(FLASH)'))

    def test_unknown_syntax_rejected(self):
        for extra in ['INCLUDE other.ld', 'ASSERT(1,"ok")', 'PHDRS {}', 'OVERLAY {}',
                      'PROVIDE(x=1);', 'OUTPUT_FORMAT(elf32-littlearm)', '// comment\n']:
            with self.subTest(extra=extra):
                self.parse(FIXTURE + extra, ok=False)
        for statement in ['SORT(*(.text*))', 'foo.o(.text)', '*(.text?)',
                          '*(.text[ab])', '*(*.text)', 'BYTE(1)', '*(SORT(.text*))', '*(COMMON*)']:
            with self.subTest(statement=statement):
                self.parse(FIXTURE.replace('*(.text*)', statement), ok=False)
        self.parse(FIXTURE.replace('AT> FLASH', 'AT(0x08000100)'), ok=False)
        self.parse(FIXTURE.replace('> RAM AT>', '> UNKNOWN AT>'), ok=False)

    def test_invalid_bounds_and_numbers(self):
        variants = [FIXTURE + '/* missing end', FIXTURE + '\0',
                    FIXTURE.replace('512K', '4194304K'),
                    FIXTURE.replace('512K', '0x100000000'),
                    FIXTURE.replace('512K', '0x'),
                    FIXTURE.replace('512K', '09'),
                    FIXTURE.replace('512K', '0'),
                    FIXTURE.replace('512K', '1 / 0'),
                    FIXTURE.replace('512K', '1 - 2'),
                    FIXTURE.replace('512K', '0xffffffff * 2'),
                    FIXTURE.replace('512K', '(' * 18 + '1' + ')' * 18),
                    FIXTURE.replace('.text :', '.' + 'x' * 64 + ' :'),
                    FIXTURE + 'x=1; x=2;',
                    FIXTURE.replace('*(.text*)', '*(.text*) ' * 65),
                    FIXTURE + 'x=' + '+'.join(['1'] * 128) + ';',
                    FIXTURE.replace(' FLASH(rx)', ' FLASH(rxq)'),
                    FIXTURE.replace('128K', '0xffffffff'),
                    FIXTURE + ' ' * 65537]
        for text in variants:
            with self.subTest(text=text[-80:]):
                self.parse(text, ok=False)
        self.parse('MEMORY {' + ''.join('R%d: ORIGIN=%d,LENGTH=1;' % (i, i) for i in range(9)) + '} SECTIONS{.x:{*(.text)}>R0}', ok=False)
        self.parse(FIXTURE.replace(' .data :', ''.join(' .z%d : { } > FLASH\n' % i for i in range(17)) + ' .data :'), ok=False)
        self.parse(FIXTURE + ''.join('s%d=0;' % i for i in range(65)), ok=False)

    def test_unresolved_and_invalid_runtime_expressions(self):
        for expression in ['missing', 'a', 'LOADADDR(.missing)', 'ALIGN(0)',
                           'ALIGN(3)', '0xffffffff + 1', '1 / 0', '-1']:
            with self.subTest(expression=expression):
                self.parse(FIXTURE + 'a=' + expression + ';', 'eval', ok=False)
        self.parse(FIXTURE + 'a=b; b=a;', 'eval', ok=False)

    def test_gnu_simple_layout_oracle(self):
        assembler = GNU / ('arm-none-eabi-as' + EXE)
        if not assembler.exists():
            self.skipTest('GNU ARM tools unavailable')
        asm = '''.syntax unified
.thumb
.section .isr_vector,"a"
.space 8
.section .text.fn,"ax"
.global Reset_Handler
.thumb_func
Reset_Handler:
.space 12
.section .rodata.str,"a"
.space 4
.section .data.value,"aw"
.space 8
.section .bss.zero,"aw",%nobits
.space 12
.comm common_value,4,4
'''
        (OUT / 'oracle.s').write_text(asm, encoding='ascii')
        (OUT / 'oracle.ld').write_text(FIXTURE, encoding='ascii')
        self.run_command([assembler, OUT / 'oracle.s', '-o', OUT / 'oracle.o'])
        self.run_command([GNU / ('arm-none-eabi-ld' + EXE), '-T', OUT / 'oracle.ld', OUT / 'oracle.o', '-o', OUT / 'oracle.elf'])
        result = self.run_command([GNU / ('arm-none-eabi-nm' + EXE), OUT / 'oracle.elf'])
        symbols = {parts[2]: int(parts[0], 16) for line in result.stdout.splitlines()
                   if len(parts := line.split()) == 3}
        result = self.parse(FIXTURE, 'layout')
        checked = 0
        for line in result.stdout.splitlines():
            parts = line.split()
            if parts[0] == 'S':
                self.assertEqual(int(parts[2]), symbols[parts[1]], parts[1])
                checked += 1
        self.assertEqual(checked, 8)


if __name__ == '__main__':
    unittest.main()
