"""RV32IM ELF RELA mixed-object link execution; never builds the main app.

Needs existing production xhcc.wasm, WASI SDK, full Clang's RISC-V backend,
pyelftools and Unicorn. Every run owns a temporary harness/fixture directory.
"""
from pathlib import Path
import hashlib
import io
import os
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    py = Path(sys.executable).parent
    os.environ['PATH'] = os.pathsep.join([str(py), str(py/'Library/bin'),
                                        r'D:\msys64\ucrt64\bin', os.environ.get('PATH', '')])
sys.path.insert(0, str(ROOT/'build/test-deps'))
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_RISCV, UC_MODE_RISCV32
from unicorn.riscv_const import UC_RISCV_REG_SP, UC_RISCV_REG_RA, UC_RISCV_REG_A0

SDK = Path(os.environ.get('WASI_SDK_PATH', r'D:\wasi-sdk-34.0-x86_64-windows'))
CLANG = os.environ.get('RV_CLANG', r'D:\msys64\ucrt64\bin\clang.exe')
WASMTIME = os.environ.get('WASMTIME', r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
COMPILER = Path(os.environ.get('XHCC_WASM', str(ROOT/'build/wasm/xhcc.wasm')))
EXE = '.exe' if os.name == 'nt' else ''
HARNESS = r'''
#include "xh_elf_link.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
int fs_file_size(const char *p){FILE*f=fopen(p,"rb");long n;if(!f)return -1;fseek(f,0,SEEK_END);n=ftell(f);fclose(f);return (int)n;}
int fs_read_file(const char*p,char*b,int cap){FILE*f=fopen(p,"rb");size_t n;if(!f)return -1;n=fread(b,1,(size_t)cap-1,f);b[n]=0;fclose(f);return (int)n;}
int fs_write_file(const char*p,const unsigned char*b,int n){FILE*f=fopen(p,"wb");size_t done;int rc;if(!f)return -1;done=fwrite(b,1,(size_t)n,f);rc=fclose(f);return done==(size_t)n&&!rc?n:-1;}
const char*fs_last_error(void){return strerror(errno);}
void diag_tool(int severity,const char*where,const char*message,const char*a,const char*b,const char*c,const char*d,const char*e,const char*f){(void)severity;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;fprintf(stderr,"%s: %s\n",where,message);}
int main(int argc,char**argv){XhElfLinkOptions o;char err[512];int rc;if(argc<7)return 2;memset(&o,0,sizeof(o));o.output_path=argv[1];o.entry_name=argv[2];o.text_addr=(uint32_t)strtoul(argv[3],0,0);o.data_addr=(uint32_t)strtoul(argv[4],0,0);o.max_memory=(size_t)strtoul(argv[5],0,0);rc=xh_elf_link(XH_ELF_TARGET_RISCV32,(const char*const*)(argv+6),(unsigned)(argc-6),&o,err,sizeof(err));if(rc)fprintf(stderr,"link: %s\n",err);return rc?1:0;}
'''
XH = '''
extern int clang_data;
extern int clang_bridge(int);
int xh_data=11; int xh_bss;
static int local=2;
int *xh_pointer=&clang_data;
int xh_helper(int x){return x+xh_data+local;}
int entry(void){xh_bss=5;return clang_bridge(7)+*xh_pointer+xh_bss+99;}
'''
CL = '''
extern int xh_data;
extern int xh_helper(int);
int clang_data=13; int clang_bss;
static int local=3;
int *clang_pointer=&xh_data;
int (*callback)(int)=xh_helper;
int clang_bridge(int x){clang_bss=9;return xh_helper(x)+local+clang_bss;}
'''


def elf(path):
    return ELFFile(io.BytesIO(path.read_bytes()))


def symbols(path):
    return {s.name:s['st_value'] for s in elf(path).get_section_by_name('.symtab').iter_symbols() if s.name}


class RvElfLinkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        (ROOT/'build').mkdir(exist_ok=True)
        cls.tmp = tempfile.TemporaryDirectory(prefix='rv-elf-link-', dir=ROOT/'build')
        cls.out = Path(cls.tmp.name)
        cls.digest = hashlib.sha256(COMPILER.read_bytes()).digest()
        source = cls.out/'harness.c'; source.write_text(HARNESS, encoding='utf-8')
        p = subprocess.run([str(SDK/('bin/clang'+EXE)), '--target=wasm32-wasip1',
                            '-std=c99', '-O1', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', '-fsanitize-trap=all',
                            '-Iinclude', '-include', 'include/xh_small.h', str(source),
                            'src/link/elf_link.c', 'src/link/elf_read.c',
                            'src/link/arm_elf_reloc.c', 'src/link/rv_elf_reloc.c',
                            'src/link/ld_script.c', 'src/backend/rules/relocation.c',
                            '-o', str(cls.out/'link.wasm')], cwd=ROOT,
                           capture_output=True, text=True, timeout=120)
        if p.returncode:
            cls.tmp.cleanup()
            raise RuntimeError(p.stdout+p.stderr)

    @classmethod
    def tearDownClass(cls):
        try:
            if hashlib.sha256(COMPILER.read_bytes()).digest() != cls.digest:
                raise AssertionError('production xhcc.wasm changed during standalone tests')
        finally:
            cls.tmp.cleanup()

    def command(self, args, ok=True):
        p = subprocess.run(list(map(str,args)), cwd=ROOT, capture_output=True, text=True, timeout=120)
        if ok:
            self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        else:
            self.assertNotEqual(p.returncode,0,p.stdout+p.stderr)
        return p

    def compile(self, name, text, xh=False, asm=False):
        source=self.out/(name+('.s' if asm else '.c')); source.write_text(text,encoding='utf-8')
        obj=self.out/(name+'.o')
        if xh:
            args=[sys.executable,'tools/xhcc.py','--compiler',COMPILER,'--wasmtime',WASMTIME,
                  'riscv32-unknown-elf-gcc','-O0']
        else:
            args=[CLANG,'--target=riscv32-unknown-elf','-march=rv32im','-mabi=ilp32',
                  '-O0','-ffreestanding','-fno-unwind-tables','-fno-asynchronous-unwind-tables',
                  '-mno-relax' if asm else '-mrelax']
            if not asm:
                args+=['-msmall-data-limit=0','-ffunction-sections','-fdata-sections']
        self.command(args+['-c',source.relative_to(ROOT),'-o',obj.relative_to(ROOT)])
        view=elf(obj)
        self.assertEqual(view['e_machine'],'EM_RISCV');self.assertEqual(view['e_type'],'ET_REL')
        self.assertEqual(view['e_flags'],0,'RV32IM soft ABI, no RVC/RVE')
        return obj

    def link(self, objects, name, entry='entry', text=0x08004000, data=0x20002000, budget=0, ok=True):
        output=self.out/(name+'.elf')
        p=self.command([WASMTIME,'run','--dir=.',(self.out/'link.wasm').relative_to(ROOT),
                        output.relative_to(ROOT),entry,hex(text),hex(data),str(budget),
                        *[o.relative_to(ROOT) for o in objects]],ok)
        return output,p

    def execute(self, path, expected, name=None):
        view=elf(path);self.assertEqual(view['e_type'],'ET_EXEC');self.assertEqual(view['e_machine'],'EM_RISCV')
        self.assertEqual(view['e_flags'],0);self.assertEqual(view.elfclass,32);self.assertTrue(view.little_endian)
        uc=Uc(UC_ARCH_RISCV,UC_MODE_RISCV32)
        loads=[s for s in view.iter_segments() if s['p_type']=='PT_LOAD']
        pages=set()
        for s in loads:
            self.assertLessEqual(s['p_filesz'],s['p_memsz'])
            pages.update(range(s['p_vaddr']&~4095,(s['p_vaddr']+s['p_memsz']+4095)&~4095,4096))
        for p in pages:uc.mem_map(p,4096);uc.mem_write(p,b'\xa5'*4096)
        for s in loads:
            uc.mem_write(s['p_vaddr'],s.data())
            n=s['p_memsz']-s['p_filesz']
            if n:uc.mem_write(s['p_vaddr']+s['p_filesz'],b'\0'*n)
        uc.mem_map(0x21000000,0x10000);uc.mem_map(0x0fff0000,4096)
        uc.reg_write(UC_RISCV_REG_SP,0x2100fff0);uc.reg_write(UC_RISCV_REG_RA,0x0fff0000)
        syms=symbols(path);address=syms[name] if name else view['e_entry']
        self.assertEqual(address%4,0)
        uc.emu_start(address,0x0fff0000,count=100000)
        self.assertEqual(uc.reg_read(UC_RISCV_REG_A0)&0xffffffff,expected)
        return uc,syms

    def reject(self, objects, name, **kwargs):
        output=self.out/(name+'.elf');old=b'old RV output\0\xff';output.write_bytes(old)
        _,p=self.link(objects,name,ok=False,**kwargs)
        self.assertEqual(output.read_bytes(),old);self.assertTrue(p.stderr.strip())
        return p

    def test_real_xhcc_clang_calls_globals_pointers_two_layouts(self):
        a=self.compile('mixed-xh',XH,xh=True);b=self.compile('mixed-clang',CL)
        types=set()
        for obj in (a,b):
            view=elf(obj)
            for sec in view.iter_sections():
                if sec['sh_type']=='SHT_RELA':types.update(r['r_info_type'] for r in sec.iter_relocations())
        self.assertIn(1,types)  # R_RISCV_32 data/function pointer
        self.assertTrue(types & {18,19},types)  # CALL / CALL_PLT AUIPC+JALR
        self.assertIn(51,types)  # RELAX marker accepted without shrinking
        for text,data in [(0x08004000,0x20002000),(0x08018000,0x20008000)]:
            out,_=self.link([a,b],'mixed-'+hex(text),text=text,data=data)
            uc,syms=self.execute(out,149)
            for pointer,target in [('xh_pointer','clang_data'),('clang_pointer','xh_data'),('callback','xh_helper')]:
                self.assertEqual(int.from_bytes(uc.mem_read(syms[pointer],4),'little'),syms[target])
            loads=[s for s in elf(out).iter_segments() if s['p_type']=='PT_LOAD']
            self.assertTrue(any(s['p_vaddr']==text and s['p_flags']&1 for s in loads))
            ram=next(s for s in loads if s['p_vaddr']==data)
            self.assertGreater(ram['p_memsz'],ram['p_filesz']);self.assertLess(ram['p_paddr'],data)

    def pcrel_object(self):
        return self.compile('pcrel','''.option norvc
.option norelax
.text
.globl entry
.type entry,@function
entry:
.Lanchor:
 auipc t0,%pcrel_hi(value+4)
 lw a0,%pcrel_lo(.Lanchor)(t0)
.Lbranch:
 beq a0,zero,.Lbranch
 .reloc .Lbranch,R_RISCV_BRANCH,bad_path
 jal zero,done_path
.globl bad_path
bad_path: addi a0,zero,1
.globl done_path
done_path: ret
.data
.balign 4
.globl value
value: .word 3
.word 149
''',asm=True)

    def test_pcrel_anchor_branch_jal_and_explicit_addend(self):
        obj=self.pcrel_object()
        types={r['r_info_type'] for s in elf(obj).iter_sections() if s['sh_type']=='SHT_RELA' for r in s.iter_relocations()}
        self.assertTrue({16,17,23,24}.issubset(types),types)
        for text,data in [(0x10000,0x30000),(0x50000,0x70000)]:
            out,_=self.link([obj],'pcrel-'+hex(text),text=text,data=data)
            self.execute(out,149)

    def test_malformed_flags_relocations_anchor_and_budget_preserve_old(self):
        obj=self.pcrel_object();raw=obj.read_bytes();view=elf(obj)
        for flags in (1,2,4,8,16):
            bad=bytearray(raw);struct.pack_into('<I',bad,36,flags)
            path=self.out/('flags-'+str(flags)+'.o');path.write_bytes(bad)
            self.reject([path],'flags-'+str(flags))
        for label,content in [('badfile',b'bad ELF'),('endian',raw[:5]+b'\x02'+raw[6:])]:
            path=self.out/(label+'.o');path.write_bytes(content);self.reject([path],label)
        rel=next(s for s in view.iter_sections() if s['sh_type']=='SHT_RELA' and any(r['r_info_type']==23 for r in s.iter_relocations()))
        records=list(rel.iter_relocations())
        hi=next(i for i,r in enumerate(records) if r['r_info_type']==23)
        for label,index,typ in [('unknown',hi,255),('missing-hi',hi,0)]:
            bad=bytearray(raw);offset=rel['sh_offset']+index*12+4
            info=struct.unpack_from('<I',bad,offset)[0];struct.pack_into('<I',bad,offset,(info&~255)|typ)
            path=self.out/(label+'.o');path.write_bytes(bad);self.reject([path],label)
        # A second HI20 at the exact anchor makes LO12 pairing ambiguous.
        duplicate=next(i for i,r in enumerate(records) if r['r_info_type']==16)
        bad=bytearray(raw);offset=rel['sh_offset']+duplicate*12
        bad[offset:offset+12]=bad[rel['sh_offset']+hi*12:rel['sh_offset']+hi*12+12]
        path=self.out/'duplicate-anchor.o';path.write_bytes(bad);self.reject([path],'duplicate-anchor')
        table=view.get_section_by_name('.symtab')
        entry_index=next(i for i,s in enumerate(table.iter_symbols()) if s.name=='entry')
        bad=bytearray(raw);bad[table['sh_offset']+entry_index*16+12]&=0xf0
        path=self.out/'notype-entry.o';path.write_bytes(bad);self.reject([path],'notype-entry')
        relaxed=self.compile('relax-check',CL)
        rv=elf(relaxed);rr=next(s for s in rv.iter_sections() if s['sh_type']=='SHT_RELA'
                              and any(r['r_info_type']==51 for r in s.iter_relocations()))
        marker=next(i for i,r in enumerate(rr.iter_relocations()) if r['r_info_type']==51)
        bad=bytearray(relaxed.read_bytes());struct.pack_into('<i',bad,rr['sh_offset']+marker*12+8,1)
        path=self.out/'relax-addend.o';path.write_bytes(bad)
        provider=self.compile('relax-provider','int xh_data=11; int xh_helper(int x){return x+13;}\n')
        self.reject([path,provider],'relax-addend')
        self.reject([obj],'budget',budget=64)
        self.reject([obj,obj],'duplicate-strong')


if __name__=='__main__':
    unittest.main(verbosity=2)
