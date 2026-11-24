"""Compile only the standalone reader to WASI; test real xhcc/GNU objects.
No main build or driver changes. Requires existing production xhcc.wasm, WASI SDK,
Wasmtime and GNU ARM GCC. Run: python tests/test_elf_reader.py.
"""
from pathlib import Path
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import inspect

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include "xh_elf_read.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int inspect(unsigned char *data, size_t len, int strict) {
    XhElfView v; unsigned i; uint32_t j;
    int rc=xh_elf_open(data,len,&v);
    if(rc) { if(v.data || v.len || v.shnum) return 91; return 1; }
    if(v.data!=data || v.len!=len) return 92;
    for(i=0;i<v.shnum;i++) {
        XhElfSection s;
        if(xh_elf_section(&v,i,&s)) return 93;
        if(s.type==XH_ELF_SHT_NOBITS && s.data) return 94;
        if(!s.name) return 95;
        if(s.type==XH_ELF_SHT_SYMTAB) for(j=0;j<s.size/16;j++) {
            XhElfSymbol sym;
            if(xh_elf_symbol(&v,i,j,&sym) || !sym.name) return 96;
        }
        if(s.type==XH_ELF_SHT_REL) for(j=0;j<s.size/8;j++) {
            XhElfRel rel; if(xh_elf_rel(&v,i,j,&rel)) return 97;
        }
    }
    { XhElfSection s; XhElfSymbol sym; XhElfRel rel;
      if(xh_elf_section(&v,v.shnum,&s)!=XH_ELF_ERR_INDEX ||
         xh_elf_symbol(&v,v.shnum,0,&sym)!=XH_ELF_ERR_INDEX ||
         xh_elf_rel(&v,v.shnum,0,&rel)!=XH_ELF_ERR_INDEX) return 98;
    }
    if(strict && xh_elf_validate_thumb(&v)) return 2;
    return 0;
}
int main(int argc,char **argv) {
    FILE *f; unsigned char hdr[6], *data; unsigned count=0;
    if(argc!=2 || !(f=fopen(argv[1],"rb"))) return 80;
    while(fread(hdr,1,6,f)==6) {
        uint32_t n=(uint32_t)hdr[0]|((uint32_t)hdr[1]<<8)|((uint32_t)hdr[2]<<16)|((uint32_t)hdr[3]<<24);
        int got;
        data=malloc(n?n:1); if(!data) return 81;
        if(fread(data,1,n,f)!=n) return 82;
        got=inspect(data,n,hdr[5]); free(data);
        if((hdr[4]==3 && got>=90) || (hdr[4]!=3 && got!=hdr[4])) {
            fprintf(stderr,"case=%u len=%lu expected=%u got=%d\n",count,(unsigned long)n,hdr[4],got); return 83;
        }
        count++;
    }
    if(!feof(f)) return 84;
    fclose(f); printf("PASS reader %u cases, real xhcc/GNU objects and bounded accessors\n",count);
    return 0;
}
'''


def u16(b, off):
    return struct.unpack_from('<H', b, off)[0]


def u32(b, off):
    return struct.unpack_from('<I', b, off)[0]


def put(b, off, value, width=4):
    struct.pack_into('<H' if width == 2 else '<I', b, off, value)


class ElfReaderTests(unittest.TestCase):
    def test_real_objects_and_mutations(self):
        sdk = Path(os.environ.get('WASI_SDK_PATH', r'D:\wasi-sdk-34.0-x86_64-windows'))
        cc = os.environ.get('WASI_CC') or str(sdk/'bin'/('clang.exe' if os.name == 'nt' else 'clang'))
        runtime = os.environ.get('WASMTIME') or shutil.which('wasmtime') or r'C:\Program Files\Wasmtime\bin\wasmtime.exe'
        arm = Path(os.environ.get('ARM_BIN', r'D:\arm-none-eabi\bin'))
        gcc = arm/('arm-none-eabi-gcc.exe' if os.name == 'nt' else 'arm-none-eabi-gcc')
        env = os.environ.copy()
        if os.name == 'nt':
            py = Path(sys.executable).parent
            env['PATH'] = os.pathsep.join([str(py), str(py/'Library/bin'), env.get('PATH', '')])
        def run(args):
            p = subprocess.run(list(map(str,args)), cwd=ROOT, env=env,
                               capture_output=True, text=True, timeout=90)
            if p.returncode and 'case=' in p.stderr:
                import re
                index = int(re.search(r'case=(\d+)', p.stderr).group(1))
                print('failed mutation origin:', origins[index])
            self.assertEqual(p.returncode, 0, p.stdout+p.stderr)
            return p.stdout
        with tempfile.TemporaryDirectory(prefix='elf-reader-', dir=ROOT/'build') as tmp:
            out = Path(tmp)
            (out/'reader.c').write_text(HARNESS, encoding='utf-8')
            run([cc, '--target=wasm32-wasip1', '-std=c99', '-Wall', '-Wextra', '-Werror',
                 '-O1', '-fsanitize=undefined', '-fsanitize-trap=all', '-I'+str(ROOT/'include'),
                 ROOT/'src/link/elf_read.c', out/'reader.c', '-o', out/'reader.wasm'])
            fixture = out/'fixture.c'
            fixture.write_text('extern int external(int); extern int extdata;\n'
                'static int local=3; int common; int *pointer=&extdata;\n'
                'int entry(int x){return external(x)+local+common+*pointer;}\n', encoding='utf-8')
            relative = fixture.relative_to(ROOT).as_posix()
            run([sys.executable, ROOT/'tools/xhcc.py', 'arm-none-eabi-gcc', '-c',
                 relative, '-o', (out/'xh.o').relative_to(ROOT).as_posix()])
            run([gcc, '-ffreestanding', '-mthumb', '-mcpu=cortex-m3', '-mfloat-abi=soft',
                 '-fcommon', '-g', '-c', fixture, '-o', out/'gnu.o'])
            cases = []
            origins = []
            def add(b, expected=1, strict=0):
                frame = inspect.currentframe().f_back
                origins.append((frame.f_lineno, frame.f_locals.get('off'), frame.f_locals.get('value')))
                cases.append((bytes(b), expected, strict))
            for path in (out/'xh.o', out/'gnu.o'):
                base = path.read_bytes()
                add(base, 0, 1)
                sh = u32(base,32); count = u16(base,48)
                secs = [sh+i*40 for i in range(count)]
                sym = next(s for s in secs if u32(base,s+4)==2)
                rel = next(s for s in secs if u32(base,s+4)==9 and u32(base,s+20))
                text = secs[u32(base,rel+28)]
                strings = secs[u32(base,sym+24)]
                names = secs[u16(base,50)]
                symoff = u32(base,sym+16); reloff = u32(base,rel+16)
                # Every truncated byte length must fail, including a truncated section table.
                for n in range(len(base)):
                    add(base[:n])
                def mutate(off, value, width=4, expected=1, strict=0):
                    b=bytearray(base); put(b,off,value,width); add(b,expected,strict)
                for off in (4,5,6):
                    b=bytearray(base); b[off]=2; add(b)
                for off,value,width in [(16,2,2),(18,3,2),(20,2,4),(40,51,2),
                        (46,39,2),(48,0,2),(48,257,2),(50,65535,2),
                        (50,count,2),(32,0xfffffff0,4),(36,0x05000400,4),
                        (36,0x05000001,4),(sh+20,1,4),(28,4,4),(44,1,2)]:
                    mutate(off,value,width)
                for sec in (text, sym, rel, strings, names):
                    mutate(sec+16,0xfffffff0); mutate(sec+20,0xffffffff)
                    mutate(sec+32,3); mutate(sec+32,131072)
                    mutate(sec,u32(base,names+20))
                mutate(sym+36,15); mutate(sym+20,u32(base,sym+20)-1)
                mutate(sym+24,count); mutate(sym+24,secs.index(text))
                mutate(sym+28,u32(base,sym+20)//16+1)
                mutate(sym+20,16*65537)
                # Valid file spans still cannot exceed the bounded record budget.
                for table,entsize in ((sym,16),(rel,8)):
                    b=bytearray(base); offset=len(b)
                    b.extend(bytes(entsize*65537))
                    put(b,table+16,offset); put(b,table+20,entsize*65537); add(b)
                mutate(symoff,1); mutate(symoff+4,1)
                mutate(symoff+16,u32(base,strings+20))
                mutate(symoff+16+14,0xffff,2)
                first=u32(base,sym+28)
                b=bytearray(base); b[symoff+first*16+12]&=15; add(b)
                b=bytearray(base); b[symoff+16+12]|=16; add(b)
                for off,value in [(rel+36,7),(rel+20,u32(base,rel+20)-1),
                        (rel+24,count),(rel+24,secs.index(text)),(rel+28,0),
                        (rel+28,count),(reloff,u32(base,text+20)),
                        (reloff+4,(u32(base,sym+20)//16)<<8)]:
                    mutate(off,value)
                # Both end-of-table NUL and a referenced name terminator are required.
                b=bytearray(base); b[u32(base,strings+16)+u32(base,strings+20)-1]=65; add(b)
                b=bytearray(base); b[u32(base,names+16)]=65; add(b)
                mutate(text+4,17); mutate(text+4,18)
                mutate(text+4,4); mutate(text+4,11)
                mutate(text+12,0xffffffff)
                mutate(text+4,0x60000042,expected=0)  # Generic unknown nonalloc semantics.
                mutate(text+8,u32(base,text+8)|0x400)
                mutate(text+8,u32(base,text+8)|0x200)
                b=bytearray(base); b[symoff+first*16+12]=(b[symoff+first*16+12]&0xf0)|6; add(b)
                # Base and explicit soft-float ABI; unknown REL type is the decoder's job.
                mutate(36,0x05000200,expected=0)
                b=bytearray(base); b[reloff+4]=255; add(b,0)
                # Last target byte is legal at reader level: decoder determines width.
                mutate(reloff,u32(base,text+20)-1,expected=0)
                nobits=next(s for s in secs if u32(base,s+4)==8)
                mutate(nobits+16,0x10000000,expected=0)
                b=bytearray(base); put(b,nobits+16,0xffffffff); put(b,nobits+20,1); add(b)
                # ARM state is a separate opt-in policy, with ABS functions exempt.
                func=next(symoff+i*16 for i in range(u32(base,sym+20)//16)
                          if base[symoff+i*16+12]&15==2 and u16(base,symoff+i*16+14)>0)
                b=bytearray(base); put(b,func+4,u32(base,func+4)&~1); add(b,0); add(b,2,1)
                put(b,func+14,0xfff1,2); add(b,0,1)
                # Seeded random mutations exercise arbitrary combinations without imposing
                # semantics on mutations that happen to remain valid.
                rng=random.Random(104)
                for _ in range(400):
                    b=bytearray(base)
                    for _ in range(rng.randrange(1,5)):
                        b[rng.randrange(len(b))]=rng.randrange(256)
                    add(b,3,1)
            corpus=out/'corpus.bin'
            with corpus.open('wb') as f:
                for b,expected,strict in cases:
                    f.write(struct.pack('<IBB',len(b),expected,strict)); f.write(b)
            result=run([runtime,'run','--dir=.',out/'reader.wasm',corpus.relative_to(ROOT).as_posix()])
            print(result.strip())
            self.assertIn(f'PASS reader {len(cases)} cases',result)


if __name__ == '__main__':
    unittest.main()
