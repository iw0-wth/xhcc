"""Explicit ARM ELF link core: standalone WASI harness, real objects and execution.

Run with the production build/wasm/xhcc.wasm already present. This test never
builds the main application and never supplies archives, libc or libgcc. Override
WASI_SDK_PATH, WASMTIME, GNUARM/ARM_BIN or XHCC_WASM for another tool install.
Python needs pyelftools and Unicorn (also searched in build/test-deps).
"""
from pathlib import Path
import hashlib
import io
import os
import shutil
import struct
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/elf-link-tests'
if os.name == 'nt':
    _py = Path(sys.executable).parent
    os.environ['PATH'] = os.pathsep.join(
        [str(_py), str(_py / 'Library/bin'), os.environ.get('PATH', '')])
sys.path.insert(0, str(ROOT / 'build/test-deps'))
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0

SDK = Path(os.environ.get('WASI_SDK_PATH', r'D:\wasi-sdk-34.0-x86_64-windows'))
WASMTIME = os.environ.get('WASMTIME') or shutil.which('wasmtime') or r'C:\Program Files\Wasmtime\bin\wasmtime.exe'
GNU = Path(os.environ.get('ARM_BIN', str(Path(os.environ.get('GNUARM', r'D:\arm-none-eabi')) / 'bin')))
COMPILER = Path(os.environ.get('XHCC_WASM', str(ROOT / 'build/wasm/xhcc.wasm')))
EXE = '.exe' if os.name == 'nt' else ''

HARNESS = r'''
#include "xh_elf_link.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
int fs_file_size(const char *p) {
    FILE *f=fopen(p,"rb"); long n;
    if(!f) return -1;
    if(fseek(f,0,SEEK_END)) { fclose(f); return -1; }
    n=ftell(f); fclose(f); return (int)n;
}
int fs_read_file(const char *p,char *b,int cap) {
    FILE *f=fopen(p,"rb"); size_t n;
    if(!f || cap<1) { if(f) fclose(f); return -1; }
    n=fread(b,1,(size_t)cap-1,f); b[n]=0;
    if(ferror(f)) { fclose(f); return -1; }
    fclose(f); return (int)n;
}
int fs_write_file(const char *p,const unsigned char *b,int n) {
    FILE *f=fopen(p,"wb"); size_t done; int rc;
    if(!f) return -1;
    done=fwrite(b,1,(size_t)n,f); rc=fclose(f);
    return done==(size_t)n && !rc ? n : -1;
}
const char *fs_last_error(void) { return strerror(errno); }
void diag_tool(int severity,const char *where,const char *message,
 const char *a,const char *b,const char *c,const char *d,const char *e,const char *f) {
    (void)severity;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;
    fprintf(stderr,"%s: %s\n",where,message);
}
int main(int argc,char **argv) {
    XhElfLinkOptions options; char error[512]; int rc;
    if(argc<9) return 2;
    memset(&options,0,sizeof(options));
    options.output_path=argv[1]; options.entry_name=argv[2];
    options.text_addr=(uint32_t)strtoul(argv[3],0,0);
    options.data_addr=(uint32_t)strtoul(argv[4],0,0);
    options.data_lma=(uint32_t)strtoul(argv[5],0,0);
    options.max_memory=(size_t)strtoul(argv[6],0,0);
    options.output_format=(unsigned)strtoul(argv[7],0,0);
    options.script_path=getenv("XH_TEST_LINK_SCRIPT");
    if(!strcmp(options.entry_name,"@script")) options.entry_name=NULL;
    error[0]=0;
    rc=xh_elf_link_arm((const char *const *)(argv+8),(unsigned)(argc-8),
                        &options,error,sizeof(error));
    if(rc) fprintf(stderr,"link: %s\n",error);
    return rc ? 1 : 0;
}
'''

XH_SOURCE = '''
extern int gnu_data, gnu_bss;
extern int gnu_helper(int);
extern int gnu_bridge(int);
static int local=2;
int xh_data=11, xh_bss;
int xh_array[3]={7,19,23};
int *xh_pointer=&gnu_data;
int (*xh_function)(int)=gnu_helper;
int xh_helper(int x){return x+xh_data;}
int xh_bridge(int x){return gnu_helper(x)+xh_data+local;}
int xh_entry(void){xh_bss=4;return gnu_bridge(7)+*xh_pointer+xh_bss+local+xh_function(2);}
'''
GNU_SOURCE = '''
extern int xh_data, xh_bss;
extern int xh_helper(int), xh_bridge(int);
static int local=3;
int gnu_data=13, gnu_bss;
int gnu_array[3]={5,17,29};
int *gnu_pointer=&xh_data;
int (*gnu_function)(int)=xh_helper;
int gnu_helper(int x){return x+gnu_data;}
int gnu_bridge(int x){return xh_helper(x)+gnu_data+local;}
int gnu_entry(void){gnu_bss=6;return xh_bridge(7)+*gnu_pointer+gnu_bss+local+gnu_function(2);}
'''


def elf_bytes(path):
    return ELFFile(io.BytesIO(path.read_bytes()))


def symbol_values(elf):
    table = elf.get_section_by_name('.symtab')
    return {s.name: s['st_value'] for s in table.iter_symbols() if s.name} if table else {}


class ElfLinkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        cls.compiler_digest = hashlib.sha256(COMPILER.read_bytes()).digest()
        source = OUT / 'harness.c'
        source.write_text(HARNESS, encoding='utf-8')
        command = [str(SDK / ('bin/clang' + EXE)), '--target=wasm32-wasip1',
                   '-std=c99', '-O1', '-Wall', '-Wextra', '-Werror',
                   '-fsanitize=undefined', '-fsanitize-trap=all',
                   '-Iinclude', '-include', 'include/xh_small.h',
                   str(source.relative_to(ROOT)), 'src/link/elf_link.c',
                   'src/link/elf_read.c', 'src/link/arm_elf_reloc.c', 'src/link/rv_elf_reloc.c', 'src/link/ld_script.c',
                   'src/backend/rules/relocation.c', '-o',
                   str((OUT / 'link.wasm').relative_to(ROOT))]
        p = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=120)
        if p.returncode:
            raise RuntimeError(p.stdout + p.stderr)

    @classmethod
    def tearDownClass(cls):
        if hashlib.sha256(COMPILER.read_bytes()).digest() != cls.compiler_digest:
            raise AssertionError('production compiler changed during standalone link tests')

    def command(self, args, ok=True):
        p = subprocess.run(list(map(str, args)), cwd=ROOT, capture_output=True,
                           text=True, timeout=120)
        if ok:
            self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        else:
            self.assertNotEqual(p.returncode, 0, p.stdout + p.stderr)
        return p

    def compile(self, name, text, compiler='gnu', common=False, sections=False):
        source = OUT / (name + '.c')
        obj = OUT / (name + '.o')
        source.write_text(text, encoding='utf-8')
        flags = ['-O0', '-ffreestanding', '-mthumb', '-mcpu=cortex-m3',
                 '-mfloat-abi=soft']
        if compiler != 'xh':
            flags += ['-fno-unwind-tables', '-fno-asynchronous-unwind-tables']
        if common:
            flags += ['-fcommon']
        if sections:
            flags += ['-ffunction-sections', '-fdata-sections']
        if compiler == 'xh':
            args = [sys.executable, 'tools/xhcc.py', '--compiler', COMPILER,
                    '--wasmtime', WASMTIME, 'arm-none-eabi-gcc']
        else:
            args = [GNU / ('arm-none-eabi-gcc' + EXE)]
        self.command(args + flags + ['-c', source.relative_to(ROOT), '-o', obj.relative_to(ROOT)])
        elf = elf_bytes(obj)
        self.assertEqual(elf['e_type'], 'ET_REL')
        self.assertEqual(elf['e_machine'], 'EM_ARM')
        # Do not silently strip PREL31/exidx or introduce a runtime dependency.
        self.assertFalse(any(s['sh_flags'] & 2 and s['sh_type'] == 'SHT_ARM_EXIDX'
                             and s['sh_size'] for s in elf.iter_sections()), name)
        table = elf.get_section_by_name('.symtab')
        helpers = [s.name for s in table.iter_symbols()
                   if s['st_shndx'] == 'SHN_UNDEF' and s.name.startswith('__aeabi_')]
        self.assertEqual(helpers, [], name)
        return obj

    def link(self, objects, name='linked', entry='xh_entry', text=0x08004000,
             data=0x20002000, lma=0, budget=0, ok=True, binary=False, script=None):
        output = OUT / (name + '.elf')
        args = [WASMTIME, 'run', '--dir=.']
        if script:
            args += ['--env', 'XH_TEST_LINK_SCRIPT=' + script.relative_to(ROOT).as_posix()]
        args += [(OUT / 'link.wasm').relative_to(ROOT),
                output.relative_to(ROOT), entry, hex(text), hex(data), hex(lma),
                str(budget), str(int(binary)), *[p.relative_to(ROOT) for p in objects]]
        result = self.command(args, ok=ok)
        return output, result

    def execute(self, output, expected, name=None):
        elf = elf_bytes(output)
        self.assertEqual(elf['e_type'], 'ET_EXEC')
        self.assertEqual(elf['e_machine'], 'EM_ARM')
        self.assertTrue(elf.little_endian)
        self.assertEqual(elf.elfclass, 32)
        segments = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
        self.assertTrue(segments)
        uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        pages = set()
        for seg in segments:
            self.assertLessEqual(seg['p_filesz'], seg['p_memsz'])
            if seg['p_align'] > 1:
                self.assertEqual(seg['p_vaddr'] % seg['p_align'],
                                 seg['p_offset'] % seg['p_align'])
            start, end = seg['p_vaddr'], seg['p_vaddr'] + seg['p_memsz']
            pages.update(range(start & ~4095, (end + 4095) & ~4095, 4096))
        for page in sorted(pages):
            uc.mem_map(page, 4096)
            uc.mem_write(page, b'\xa5' * 4096)
        for seg in segments:
            # Model loader/startup: copy initialized PT_LOAD bytes to VMA and
            # zero memsz-filesz. This is explicit; RAM was poisoned beforehand.
            uc.mem_write(seg['p_vaddr'], seg.data())
            n = seg['p_memsz'] - seg['p_filesz']
            if n:
                uc.mem_write(seg['p_vaddr'] + seg['p_filesz'], b'\0' * n)
                self.assertEqual(bytes(uc.mem_read(seg['p_vaddr'] + seg['p_filesz'], n)), b'\0' * n)
        uc.mem_map(0x21000000, 0x10000)
        uc.mem_map(0x0fff0000, 4096)
        uc.reg_write(UC_ARM_REG_SP, 0x2100fff0)
        uc.reg_write(UC_ARM_REG_LR, 0x0fff0001)
        values = symbol_values(elf)
        entry = values[name] if name else elf['e_entry']
        self.assertTrue(entry & 1, 'Thumb entry bit must survive ET_EXEC output')
        uc.emu_start(entry, 0x0fff0000, count=100000)
        self.assertEqual(uc.reg_read(UC_ARM_REG_R0) & 0xffffffff, expected)
        return elf, uc, values

    def oracle(self, objects, name, entry, text, data):
        script = OUT / (name + '.ld')
        script.write_text('ENTRY(' + entry + ')\nSECTIONS {\n'
                          '. = ' + hex(text) + '; .text : { *(.text*) *(.rodata*) }\n'
                          '. = ' + hex(data) + '; .data : AT(LOADADDR(.text)+SIZEOF(.text)) { *(.data*) }\n'
                          '.bss (NOLOAD) : { *(.bss*) *(COMMON) }\n}\n', encoding='utf-8')
        output = OUT / (name + '-gnu.elf')
        self.command([GNU / ('arm-none-eabi-ld' + EXE), '-T', script,
                      '-o', output, *objects])
        return output

    def test_mixed_calls_data_pointers_locals_two_layouts_and_gnu_oracle(self):
        xh = self.compile('mixed-xh', XH_SOURCE, 'xh')
        gnu = self.compile('mixed-gnu', GNU_SOURCE, sections=True)
        for text, data in [(0x08004000, 0x20002000), (0x08018000, 0x20008000)]:
            for entry, expected in [('xh_entry', 68), ('gnu_entry', 66)]:
                with self.subTest(text=hex(text), data=hex(data), entry=entry):
                    name = entry + '-' + hex(text)
                    output, _ = self.link([xh, gnu], name, entry, text, data)
                    elf, uc, symbols = self.execute(output, expected)
                    loads = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
                    self.assertTrue(any(s['p_vaddr'] == text and s['p_flags'] & 1 for s in loads))
                    ram = next(s for s in loads if s['p_vaddr'] == data)
                    self.assertTrue(ram['p_flags'] & 2)
                    self.assertGreater(ram['p_memsz'], ram['p_filesz'])
                    self.assertGreaterEqual(ram['p_paddr'], text)
                    self.assertLess(ram['p_paddr'], data)
                    self.assertLess(output.stat().st_size, 1024 * 1024)
                    oracle = self.oracle([xh, gnu], name, entry, text, data)
                    _, refuc, refsyms = self.execute(oracle, expected)
                    for values, machine in [(symbols, uc), (refsyms, refuc)]:
                        for pointer, target in [('xh_pointer', 'gnu_data'), ('gnu_pointer', 'xh_data'),
                                                ('xh_function', 'gnu_helper'), ('gnu_function', 'xh_helper')]:
                            self.assertEqual(int.from_bytes(machine.mem_read(values[pointer], 4), 'little'), values[target])
                        self.assertEqual(int.from_bytes(machine.mem_read(values['xh_data'], 4), 'little'), 11)
                        self.assertEqual(int.from_bytes(machine.mem_read(values['gnu_data'], 4), 'little'), 13)

    def test_weak_strong_override_common_and_undefined_weak_data(self):
        weak = self.compile('weak', 'int chosen __attribute__((weak))=2;\n'
                            'int pick(void) __attribute__((weak)); int pick(void){return 3;}\n')
        strong = self.compile('strong', 'int chosen=7; int pick(void){return 11;}\n')
        caller = self.compile('weak-caller', 'extern int chosen; extern int pick(void);\n'
                              'extern int absent __attribute__((weak));\n'
                              'int *missing_pointer=&absent;\n'
                              'int entry(void){return chosen+pick()+(missing_pointer==0);}\n')
        for objects, expected, suffix in [([caller, weak], 6, 'weak-only'),
                                          ([caller, weak, strong], 19, 'weak-strong'),
                                          ([strong, weak, caller], 19, 'strong-weak')]:
            output, _ = self.link(objects, suffix, 'entry')
            self.execute(output, expected)
        a = self.compile('common-a', 'extern int other(void);\n'
                         'int shared[2]; int entry(void){shared[0]=9;return other()+shared[0];}\n', common=True)
        b = self.compile('common-b', 'int shared[5] __attribute__((aligned(16)));\n'
                         'int other(void){shared[4]=12;return shared[4];}\n', common=True)
        output, _ = self.link([a, b], 'common', 'entry')
        elf, _, syms = self.execute(output, 21)
        self.assertEqual(syms['shared'] % 16, 0)
        common_symbol = elf.get_section_by_name('.symtab').get_symbol_by_name('shared')[0]
        self.assertGreaterEqual(common_symbol['st_size'], 20)
        self.execute(self.oracle([a, b], 'common', 'entry', 0x08004000, 0x20002000), 21)

    def assert_failed_preserves_output(self, objects, name, entry='entry', **options):
        output = OUT / (name + '.elf')
        sentinel = b'old output must survive\x00\xff'
        output.write_bytes(sentinel)
        _, result = self.link(objects, name, entry, ok=False, **options)
        self.assertEqual(output.read_bytes(), sentinel)
        self.assertTrue(result.stderr.strip(), 'failed links need a diagnostic')
        return result

    def test_rejected_inputs_symbols_relocations_and_budget_preserve_output(self):
        unresolved = self.compile('undefined', 'extern int missing; int entry(void){return missing;}\n', 'xh')
        self.assert_failed_preserves_output([unresolved], 'undefined-old')
        good = self.compile('good', 'int entry(void){return 42;}\n', 'xh')
        self.assert_failed_preserves_output([good, good], 'duplicate-old')
        self.assert_failed_preserves_output([good], 'missing-entry-old', entry='not_present')
        self.assert_failed_preserves_output([good], 'budget-old', budget=64)
        self.assert_failed_preserves_output([good], 'address-overflow-old', text=0xfffffff0)
        weakcall = self.compile('undefined-weak-call',
                                'extern int missing(void) __attribute__((weak));\n'
                                'int entry(void){return missing();}\n')
        self.assert_failed_preserves_output([weakcall], 'undefined-weak-call-old')
        raw = unresolved.read_bytes()
        elf = elf_bytes(unresolved)
        rel = next(s for s in elf.iter_sections() if s['sh_type'] == 'SHT_REL' and s['sh_size'])
        mutated = bytearray(raw)
        mutated[rel['sh_offset'] + 4] = 255
        badrel = OUT / 'unsupported.o'; badrel.write_bytes(mutated)
        self.assert_failed_preserves_output([badrel], 'unsupported-old')
        for label, contents in [('badfile', b'not ELF'), ('truncated', raw[:51]),
                                ('endian', raw[:5] + b'\x02' + raw[6:]),
                                ('machine', raw[:18] + b'\x03\x00' + raw[20:])]:
            path = OUT / (label + '.o'); path.write_bytes(contents)
            self.assert_failed_preserves_output([path], label + '-old')
        # A genuine GNU PREL31 exidx fixture must be rejected, never discarded.
        asm = OUT / 'prel31.s'
        asm.write_text('.syntax unified\n.thumb\n.text\n.global entry\n.type entry,%function\n'
                       '.thumb_func\nentry: bx lr\n.section .ARM.exidx,"a",%arm_exidx\n'
                       '.word entry-.\n.word 1\n', encoding='utf-8')
        obj = OUT / 'prel31.o'
        self.command([GNU / ('arm-none-eabi-as' + EXE), '-mcpu=cortex-m3', '-mthumb', asm, '-o', obj])
        self.assert_failed_preserves_output([obj], 'prel31-old')

    def test_script_regions_startup_symbols_execution_binary_and_errors(self):
        source = '''
extern unsigned char _sidata[], _sdata[], _edata[], _sbss[], _ebss[];
int entry(void);
int (*vectors[])(void) __attribute__((section(".isr_vector")))={entry};
int initialized=31; int zeroes[3]={0}; int common[2];
unsigned char *startup_addresses[5]={_sidata,_sdata,_edata,_sbss,_ebss};
int entry(void){return initialized+zeroes[2]+common[1]+(_edata-_sdata)+(_ebss-_sbss);}
'''
        obj = self.compile('script-fixture', source, common=True, sections=True)
        for text, data in [(0x08004000, 0x20002000), (0x08018000, 0x20008000)]:
            name = 'script-' + hex(text)
            script = OUT / (name+'.ld')
            content = '''ENTRY(entry)
MEMORY { FLASH (rx) : ORIGIN = TEXT, LENGTH = 128K
         RAM (rw) : ORIGIN = DATA, LENGTH = 32K }
SECTIONS {
 .vectors : { KEEP(*(.isr_vector)) } > FLASH
 .text : { *(.text*) *(.rodata*) } > FLASH
 _sidata = LOADADDR(.data);
 .data : { _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM AT> FLASH
 .bss (NOLOAD) : { _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM
}
ASSERT(SIZEOF(.text) < LENGTH(FLASH), "FLASH text overflow");
ASSERT(_ebss <= ORIGIN(RAM) + LENGTH(RAM), "RAM BSS overflow");
'''.replace('TEXT',hex(text)).replace('DATA',hex(data))
            script.write_text(content, encoding='utf-8')
            if text == 0x08004000:
                empty_source = OUT / 'script-empty-init-array.s'
                empty_source.write_text('.section .init_array,"aw",%init_array\n', encoding='utf-8')
                empty_obj = OUT / 'script-empty-init-array.o'
                self.command([GNU / ('arm-none-eabi-as'+EXE), '-mcpu=cortex-m3',
                              '-mthumb', empty_source, '-o', empty_obj])
                empty_section = elf_bytes(empty_obj).get_section_by_name('.init_array')
                self.assertEqual(empty_section['sh_size'], 0)
                self.assertTrue(empty_section['sh_flags'] & 2)
                empty_output, _ = self.link([obj, empty_obj], name+'-empty-array', '@script', script=script)
            output, _ = self.link([obj], name, '@script', script=script)
            if text == 0x08004000:
                empty_symbols = symbol_values(elf_bytes(empty_output))
                self.execute(empty_output, 31 + empty_symbols['_edata'] - empty_symbols['_sdata']
                             + empty_symbols['_ebss'] - empty_symbols['_sbss'])
            oracle = OUT / (name+'-oracle.elf')
            self.command([GNU / ('arm-none-eabi-ld'+EXE), '-T', script, obj, '-o', oracle])
            actual_elf = elf_bytes(output); oracle_elf = elf_bytes(oracle)
            actual = symbol_values(actual_elf); reference = symbol_values(oracle_elf)
            for symbol in ('entry', 'vectors', 'initialized', 'zeroes', 'common',
                           '_sidata', '_sdata', '_edata', '_sbss', '_ebss'):
                self.assertEqual(actual[symbol], reference[symbol], symbol)
            expected = 31 + reference['_edata'] - reference['_sdata'] + reference['_ebss'] - reference['_sbss']
            for path in (output, oracle):
                elf, uc, symbols = self.execute(path, expected)
                self.assertEqual(elf['e_entry'], symbols['entry'])
                self.assertEqual(int.from_bytes(uc.mem_read(symbols['vectors'],4),'little'), symbols['entry'])
                pointers = struct.unpack('<5I', uc.mem_read(symbols['startup_addresses'],20))
                self.assertEqual(pointers, tuple(symbols[s] for s in ('_sidata','_sdata','_edata','_sbss','_ebss')))
            binary, _ = self.link([obj], name+'-flat', '@script', script=script, binary=True)
            refbinary = OUT / (name+'-oracle.bin')
            self.command([GNU / ('arm-none-eabi-objcopy'+EXE), '-O', 'binary', oracle, refbinary])
            self.assertEqual(binary.read_bytes(), refbinary.read_bytes())
            if text == 0x08004000:
                # Explicit >REGION uses that region's cursor in GNU ld, even
                # after a top-level dot assignment between output sections.
                for suffix, statement in [('align', '. = ALIGN(16);'), ('add', '. = . + 16;')]:
                    dotscript = OUT / (name+'-dot-'+suffix+'.ld')
                    dotscript.write_text(content.replace(' .text :', ' '+statement+'\n .text :'), encoding='utf-8')
                    dotout, _ = self.link([obj], name+'-dot-'+suffix, '@script', script=dotscript)
                    dotref = OUT / (name+'-dot-'+suffix+'-gnu.elf')
                    self.command([GNU / ('arm-none-eabi-ld'+EXE), '-T', dotscript, obj, '-o', dotref])
                    av = symbol_values(elf_bytes(dotout)); rv = symbol_values(elf_bytes(dotref))
                    for symbol in ('entry', '_sidata', '_sdata', '_sbss', '_ebss'):
                        self.assertEqual(av[symbol], rv[symbol], (suffix, symbol))
            for label, bad in [('region-overflow', content.replace('LENGTH = 32K','LENGTH = 4')),
                               ('unmatched', content.replace('*(.text*)','*(.absent*)')),
                               ('unsupported', content+'PROVIDE(unknown = 1);\n'),
                               ('symbol-collision', content+'initialized = 9;\n')]:
                badscript = OUT / (name+'-'+label+'.ld')
                badscript.write_text(bad, encoding='utf-8')
                self.assert_failed_preserves_output([obj], name+'-'+label, '@script', script=badscript)
            if text == 0x08004000:
                # The bounded evaluator promises short circuit evaluation.
                # GNU ld resolves missing_symbol eagerly, so this is a core
                # contract check rather than part of the GNU layout oracle.
                shortscript = OUT / (name+'-assert-short-circuit.ld')
                shortscript.write_text(content+'ASSERT(1 || missing_symbol, "short circuit success");\n', encoding='utf-8')
                shortout, _ = self.link([obj], name+'-assert-short-circuit', '@script', script=shortscript)
                self.execute(shortout, expected)
                for label, assertion, message in [
                    ('false', 'ASSERT(0, "forced assertion failure");', 'forced assertion failure'),
                    ('ram-bound', 'ASSERT(_ebss < ORIGIN(RAM), "RAM assertion boundary");', 'RAM assertion boundary'),
                    ('undefined', 'ASSERT(unknown_assert_symbol, "undefined assertion");', 'undefined assertion'),
                    ('cycle', 'cycle_a = cycle_b; cycle_b = cycle_a; ASSERT(cycle_a, "cycle assertion");', None),
                    ('long-message', 'ASSERT(0, "' + 'x' * 256 + '");', None),
                ]:
                    badscript = OUT / (name+'-assert-'+label+'.ld')
                    badscript.write_text(content+assertion+'\n', encoding='utf-8')
                    failed = self.assert_failed_preserves_output([obj], name+'-assert-'+label,
                                                                 '@script', script=badscript)
                    if message:
                        self.assertIn(message, failed.stderr)

    def test_flat_binary_lma_bytes_gap_bss_and_objcopy_oracle(self):
        obj = self.compile('flat-data', 'int initialized=37; int zeroes[32];\n'
                           'int entry(void){return initialized+zeroes[0];}\n', sections=True)
        onlytext = self.compile('flat-text', 'int entry(void){return 42;}\n', 'xh')
        for objects, lma, name in [([obj], 0, 'flat-default'),
                                    ([obj], 0x08010000, 'flat-gap'),
                                    ([onlytext], 0, 'flat-text-only')]:
            with self.subTest(name=name):
                output, _ = self.link(objects, name, 'entry', lma=lma)
                binary, _ = self.link(objects, name+'-binary', 'entry', lma=lma, binary=True)
                segments = [s for s in elf_bytes(output).iter_segments()
                            if s['p_type'] == 'PT_LOAD' and s['p_filesz']]
                begin = min(s['p_paddr'] for s in segments)
                end = max(s['p_paddr'] + s['p_filesz'] for s in segments)
                self.assertEqual(begin, 0x08004000)
                expected = bytearray(end - begin)
                occupied = bytearray(end - begin)
                for seg in segments:
                    offset = seg['p_paddr'] - begin
                    expected[offset:offset+seg['p_filesz']] = seg.data()
                    occupied[offset:offset+seg['p_filesz']] = b'\x01' * seg['p_filesz']
                actual = binary.read_bytes()
                self.assertEqual(actual, expected)
                self.assertLess(len(actual), 0x20000)
                if lma:
                    self.assertTrue(any(not b for b in occupied))
                    self.assertTrue(all(actual[i] == 0 for i, used in enumerate(occupied) if not used))
                oracle = OUT / (name+'-objcopy.bin')
                self.command([GNU / ('arm-none-eabi-objcopy'+EXE), '-O', 'binary', output, oracle])
                self.assertEqual(actual, oracle.read_bytes())
        self.assert_failed_preserves_output([obj], 'flat-huge-gap-old',
                                            lma=0x09004000, binary=True)

    def test_independent_movw_movt_across_64k(self):
        source = OUT / 'movwide.s'
        source.write_text('.syntax unified\n.thumb\n.text\n.global entry\n.type entry,%function\n'
                          '.thumb_func\nentry:\nmovw r0, #:lower16:target+4\nnop\n'
                          'movt r0, #:upper16:target+4\nldr r0,[r0]\nbx lr\n'
                          '.data\n.balign 4\n.global target\ntarget: .word 3\n.word 42\n', encoding='utf-8')
        obj = OUT / 'movwide.o'
        self.command([GNU / ('arm-none-eabi-as' + EXE), '-mcpu=cortex-m3', '-mthumb', source, '-o', obj])
        for data in (0x2000fffc, 0x20020004):
            suffix = 'movwide-' + hex(data)
            output, _ = self.link([obj], suffix, 'entry', data=data)
            self.execute(output, 42)
            self.execute(self.oracle([obj], suffix, 'entry', 0x08004000, data), 42)

    def test_explicit_data_lma_and_pointer_addends(self):
        obj = self.compile('addends', 'int values[3]={2,7,11}; int zeroes[4];\n'
                           'int *plus=&values[2]; int *minus=&values[2]-1;\n'
                           'int *negative=&values[0]-1;\n'
                           'int entry(void){return *plus+*minus+zeroes[3];}\n', 'gnu', sections=True)
        output, _ = self.link([obj], 'explicit-lma', 'entry', lma=0x08010000)
        elf, uc, syms = self.execute(output, 18)
        self.assertEqual(int.from_bytes(uc.mem_read(syms['negative'], 4), 'little'), syms['values'] - 4)
        ram = next(s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD' and s['p_flags'] & 2)
        self.assertEqual(ram['p_paddr'], 0x08010000)
        self.assertEqual(ram['p_vaddr'], 0x20002000)


if __name__ == '__main__':
    unittest.main(verbosity=2)
