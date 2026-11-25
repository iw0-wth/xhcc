"""Actual RV32 ELF RELA adapter tests; formulas execute in the shared Wasm VM."""
from pathlib import Path
import os,sys,subprocess,unittest,struct
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/rv-elf-reloc-tests';SDK=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'));WASM=os.environ.get('WASMTIME',r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
HARNESS=r'''
#include "xh_rv_elf_reloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int fs_file_size(const char*p){FILE*f=fopen(p,"rb");long n;if(!f)return -1;fseek(f,0,SEEK_END);n=ftell(f);fclose(f);return(int)n;}
int fs_read_file(const char*p,char*b,int c){FILE*f=fopen(p,"rb");int n;if(!f)return -1;n=fread(b,1,c-1,f);b[n]=0;fclose(f);return n;}
void diag_tool(int s,const char*w,const char*m,const char*a,const char*b,const char*c,const char*d,const char*e,const char*f){fprintf(stderr,"%s: %s\n",w,m);}
int main(int n,char**v){unsigned char code[16];unsigned i,x;int rc;XhRvElfRelContext c={0};if(n!=11)return 2;for(i=0;i<16;i++){if(sscanf(v[2]+2*i,"%2x",&x)!=1)return 2;code[i]=(unsigned char)x;}c.symbol=strtoul(v[3],0,0);c.addend=(int32_t)strtol(v[4],0,0);c.place=strtoul(v[5],0,0);c.pair_symbol=strtoul(v[6],0,0);c.pair_addend=(int32_t)strtol(v[7],0,0);c.pair_place=strtoul(v[8],0,0);c.has_pair=strtoul(v[9],0,0);rc=xh_rv_elf_rela_apply(strtoul(v[1],0,0),code,strtoul(v[10],0,0),0,&c);for(i=0;i<16;i++)printf("%02x",code[i]);puts("");return rc?1:0;}
'''
class RvElfReloc(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  OUT.mkdir(parents=True,exist_ok=True);(OUT/'harness.c').write_text(HARNESS);env=os.environ.copy();pd=Path(sys.executable).parent;env['PATH']=str(pd)+os.pathsep+str(pd/'Library/bin')+os.pathsep+env.get('PATH','');p=subprocess.run([str(SDK/'bin/clang.exe'),'--target=wasm32-wasip1','-O2','-Iinclude','-include','include/xh_small.h','build/rv-elf-reloc-tests/harness.c','src/link/rv_elf_reloc.c','src/backend/rules/relocation.c','-o','build/rv-elf-reloc-tests/adapter.wasm'],cwd=ROOT,env=env,capture_output=True,text=True)
  if p.returncode:raise RuntimeError(p.stderr)
 def apply(self,typ,words,s=0,a=0,p=0,high=(0,0,0,0),ok=True,length=16):
  code=b''.join(struct.pack('<I',w) for w in words).ljust(16,b'\xaa');r=subprocess.run([WASM,'run','--dir=.','build/rv-elf-reloc-tests/adapter.wasm',str(typ),code.hex(),str(s),str(a),str(p),*map(str,high[:3]),str(high[3]),str(length)],cwd=ROOT,capture_output=True,text=True);self.assertEqual(r.returncode,0 if ok else 1,r.stderr);data=bytes.fromhex(r.stdout.strip())
  if not ok:self.assertEqual(data,code)
  return list(struct.unpack('<4I',data))
 def test_abs_hi_lo_and_add_sub(self):
  self.assertEqual(self.apply(1,[0xffffffff],s=0x12340000,a=7)[0],0x12340007)
  for value in [0,0x7ff,0x800,0x12345800,0x80000000,0xffffffff]:
   hi=self.apply(26,[0x537],s=value)[0];lo=self.apply(27,[0x50513],s=value)[0];self.assertEqual(hi,(value+0x800)&0xfffff000|0x537);self.assertEqual(lo,(value&4095)<<20|0x50513)
  self.assertEqual(self.apply(35,[7],s=11,a=-2)[0],16);self.assertEqual(self.apply(39,[7],s=11,a=-2)[0],0xfffffffe)
 def test_branches_jumps_calls_and_pcrel(self):
  for offset in [-4096,-2,0,2,4094]:
   word=self.apply(16,[0x628063],s=0x10000+offset,p=0x10000)[0];u=offset&0x1fff;want=0x628063|((u>>12)<<31)|(((u>>5)&63)<<25)|(((u>>1)&15)<<8)|(((u>>11)&1)<<7);self.assertEqual(word,want)
  for offset in [-1048576,-2,0,4,1048574]:
   u=offset&0x1fffff;want=0xef|(((u>>20)&1)<<31)|(((u>>1)&1023)<<21)|(((u>>11)&1)<<20)|(((u>>12)&255)<<12);self.assertEqual(self.apply(17,[0xef],s=0x200000+offset,p=0x200000)[0],want)
  for offset in [-2147483648,-4097,-2048,0,2048,0x7fffffff]:
   words=self.apply(18,[0x297,0x280e7],s=(0x10000+offset)&0xffffffff,p=0x10000,ok=(offset%2==0 and -2147483648<=((0x10000+offset)&0xffffffff)-0x10000<=2147483647))
  hi=self.apply(23,[0x297],s=0x12345a00,p=0x10000)[0];lo=self.apply(24,[0x28293],high=(0x12345a00,0,0x10000,1))[0];self.assertEqual(hi,((0x12345a00-0x10000+0x800)&0xfffff000)|0x297);self.assertEqual(lo,((0x12345a00-0x10000)&4095)<<20|0x28293)
 def test_full_clang_lld_relocation_byte_oracle(self):
  sys.path.insert(0,str(ROOT/'build/test-deps'))
  from elftools.elf.elffile import ELFFile
  clang=Path(os.environ.get('TARGET_CLANG',r'D:\msys64\ucrt64\bin\clang.exe'))
  if not clang.is_file():self.skipTest('full-target Clang unavailable')
  source=OUT/'oracle.s';obj=OUT/'oracle.o';peer=OUT/'peer.o';exe=OUT/'oracle.elf'
  source.write_text('.option norvc\n.option norelax\n.text\n.globl entry\nentry:\n.word external+7\nlui t0,%hi(external+19)\naddi t0,t0,%lo(external+19)\nsw t0,%lo(external+19)(t1)\nbeq t0,t1,external\njal ra,external\ncall external\n.Lanchor:\nauipc t0,%pcrel_hi(external+37)\naddi t0,t0,%pcrel_lo(.Lanchor)\n.Lstore:\nauipc t1,%pcrel_hi(external+41)\nsw t0,%pcrel_lo(.Lstore)(t1)\n')
  (OUT/'peer.s').write_text('.globl external\n.set external,0x10800\n')
  env=os.environ.copy();env['PATH']=str(clang.parent)+os.pathsep+str(Path(sys.executable).parent/'Library/bin')+os.pathsep+env.get('PATH','')
  for inp,out in [(source,obj),(OUT/'peer.s',peer)]:
   p=subprocess.run([str(clang),'--target=riscv32-unknown-elf','-march=rv32im','-mabi=ilp32','-c',str(inp),'-o',str(out)],cwd=ROOT,env=env,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr)
  p=subprocess.run([str(SDK/'bin/ld.lld.exe'),'-m','elf32lriscv','--no-relax','-Ttext=0x10000','-e','entry','-o',str(exe),str(obj),str(peer)],cwd=ROOT,env=env,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr)
  with obj.open('rb') as f:
   elf=ELFFile(f);original=bytearray(elf.get_section_by_name('.text').data());symtab=elf.get_section_by_name('.symtab');relocations=list(elf.get_section_by_name('.rela.text').iter_relocations());high={r['r_offset']:r for r in relocations if r['r_info_type']==23}
   for r in relocations:
    typ=r['r_info_type'];offset=r['r_offset'];sym=symtab.get_symbol(r['r_info_sym']);value=0x10800 if sym.name=='external' else 0x10000+sym['st_value'];context=(0,0,0,0)
    if typ in [24,25]:
     h=high[sym['st_value']+r['r_addend']];hs=symtab.get_symbol(h['r_info_sym']);hv=0x10800 if hs.name=='external' else 0x10000+hs['st_value'];context=(hv,h['r_addend'],0x10000+h['r_offset'],1)
    chunk=bytes(original[offset:offset+16]).ljust(16,b'\xaa');words=list(struct.unpack('<4I',chunk));patched=self.apply(typ,words,s=value,a=r['r_addend'],p=0x10000+offset,high=context);span=0 if typ in [0,51] else 8 if typ in [18,19] else 4;original[offset:offset+span]=struct.pack('<4I',*patched)[:span]
  with exe.open('rb') as f:self.assertEqual(bytes(original),ELFFile(f).get_section_by_name('.text').data())
 def test_failures_are_atomic_and_noops(self):
  for typ,words in [(16,[0]),(17,[0]),(18,[0x297,0x300e7]),(26,[0x17]),(27,[0])]:self.apply(typ,words,s=7,ok=False)
  self.apply(16,[0x63],s=3,ok=False);self.apply(16,[0x63],s=4096,ok=False);self.apply(17,[0x6f],s=1048576,ok=False);self.apply(18,[0x297,0x280e7],length=7,ok=False);self.apply(24,[0x28293],ok=False);self.apply(24,[0x28293],a=1,high=(0x10000,0,0x2000,1),ok=False)
  self.assertEqual(self.apply(0,[0x12345678])[0],0x12345678);self.assertEqual(self.apply(51,[0x12345678])[0],0x12345678);self.apply(999,[0],ok=False)
if __name__=='__main__':unittest.main()
