"""Standard ARM ELF32 REL adapter tested against actual GNU as/ld output.
No compiler build or driver integration: small WASI harness calls production C.
"""
from pathlib import Path
import sys,os,subprocess,unittest,struct,hashlib
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/arm-elf-reloc-tests'
sys.path.insert(0,str(ROOT/'build/test-deps'))
from elftools.elf.elffile import ELFFile
SDK=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'));WASM=os.environ.get('WASMTIME',r'C:\Program Files\Wasmtime\bin\wasmtime.exe');GNU=Path(os.environ.get('GNUARM',r'D:\arm-none-eabi'))/'bin'
HARNESS=r'''
#include "xh_arm_elf_reloc.h"
#include "xh_relocation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int fs_file_size(const char*p){FILE*f=fopen(p,"rb");long n;if(!f)return -1;fseek(f,0,SEEK_END);n=ftell(f);fclose(f);return(int)n;}
int fs_read_file(const char*p,char*b,int cap){FILE*f=fopen(p,"rb");int n;if(!f)return -1;n=(int)fread(b,1,cap-1,f);b[n]=0;fclose(f);return n;}
void diag_tool(int s,const char*w,const char*m,const char*a,const char*b,const char*c,const char*d,const char*e,const char*f){fprintf(stderr,"%s: %s\n",w,m);}
int main(int n,char**v){unsigned char b[64];int i,len,rc;int64_t a=1234567;unsigned width=99,type;if(n!=8)return 2;len=(int)strlen(v[6])/2;if(len>64)return 2;for(i=0;i<len;i++){unsigned x;sscanf(v[6]+2*i,"%2x",&x);b[i]=(unsigned char)x;}type=(unsigned)strtoul(v[2],0,0);if(xh_relocation_set_path("arm",v[1]))return 2;rc=xh_arm_elf_rel_decode(type,b,len,0,&a,&width);printf("%lld %u\n",(long long)a,width);if(!rc)rc=xh_arm_elf_rel_apply(type,b,len,0,(uint32_t)strtoul(v[3],0,0),(uint32_t)strtoul(v[4],0,0),(unsigned)strtoul(v[7],0,0));if(!strcmp(v[5],"decode"))rc=xh_arm_elf_rel_decode(type,b,0,0,&a,&width);for(i=0;i<len;i++)printf("%02x",b[i]);puts("");return rc?1:0;}
'''
class ArmElfReloc(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  OUT.mkdir(parents=True,exist_ok=True);(OUT/'harness.c').write_text(HARNESS)
  env=os.environ.copy();pd=Path(sys.executable).parent;env['PATH']=str(pd)+os.pathsep+str(pd/'Library/bin')+os.pathsep+env.get('PATH','')
  p=subprocess.run([str(SDK/'bin/clang.exe'),'--target=wasm32-wasip1','-O2','-Iinclude','-include','include/xh_small.h','build/arm-elf-reloc-tests/harness.c','src/link/arm_elf_reloc.c','src/backend/rules/relocation.c','-o','build/arm-elf-reloc-tests/vm.wasm'],cwd=ROOT,env=env,capture_output=True,text=True)
  if p.returncode:raise RuntimeError(p.stderr)
 def run_cmd(self,args):
  p=subprocess.run(list(map(str,args)),cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr);return p
 def apply(self,typ,data,symbol,place,ok=True,thumb=None,path='packages/arm/relocation.rules'):
  if thumb is None:thumb=int(typ in [10,30,51] and (symbol&1)!=0)
  p=subprocess.run([WASM,'run','--dir=.','build/arm-elf-reloc-tests/vm.wasm',path,str(typ),str(symbol),str(place),'apply',data.hex(),str(thumb)],cwd=ROOT,capture_output=True,text=True)
  self.assertEqual(p.returncode,0 if ok else 1,p.stderr);lines=p.stdout.splitlines();return tuple(map(int,lines[0].split())),bytes.fromhex(lines[1])
 def fixtures(self):
  source=OUT/'vectors.s';symbols=OUT/'symbols.s';obj=OUT/'vectors.o';symobj=OUT/'symbols.o'
  source.write_text('.syntax unified\n.thumb\n.text\n.global entry\n.thumb_func\nentry:\n'+''.join('bl function'+f'{a:+d}'+'\nb.w function'+f'{a:+d}'+'\nbeq.w function'+f'{a:+d}'+'\n' for a in [0,6,-8])+''.join('movw r3, #:lower16:target'+f'{a:+d}'+'\nnop\nmovt r9, #:upper16:target'+f'{a:+d}'+'\n' for a in [0,0x1234,-12,32767,-32768,-1])+'movw r5, #:lower16:function+1\nmovt r6, #:upper16:function-1\n.word target-7\n.word function+3\n.word function+1\n.word function-1\n')
  symbols.write_text('.syntax unified\n.thumb\n.section .target,"ax",%progbits\n.global function\n.thumb_func\nfunction:\nbx lr\n.global target\ntarget:\n.word 0\n')
  self.run_cmd([GNU/'arm-none-eabi-as.exe','-mcpu=cortex-m3','-mthumb',source,'-o',obj]);self.run_cmd([GNU/'arm-none-eabi-as.exe','-mcpu=cortex-m3','-mthumb',symbols,'-o',symobj]);return obj,symobj
 def test_gnu_rel_oracle_two_placements(self):
  obj,symobj=self.fixtures()
  with obj.open('rb') as f:
   elf=ELFFile(f);raw=elf.get_section_by_name('.text').data();rel=elf.get_section_by_name('.rel.text');symtab=elf.get_section(rel['sh_link']);entries=[(r['r_offset'],r['r_info_type'],symtab.get_symbol(r['r_info_sym']).name) for r in rel.iter_relocations()]
  self.assertEqual({t for _,t,_ in entries},{2,10,30,47,48,51})
  for textbase,targetbase in [(0x10000,0x2fff0),(0x50000,0x4fff0)]:
   exe=OUT/(str(textbase)+'.elf');self.run_cmd([GNU/'arm-none-eabi-ld.exe','-Ttext='+hex(textbase),'--section-start=.target='+hex(targetbase),'-e','entry',obj,symobj,'-o',exe])
   with exe.open('rb') as f:
    elf=ELFFile(f);expected=elf.get_section_by_name('.text').data();symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
   for offset,typ,name in entries:
    decoded,got=self.apply(typ,raw[offset:offset+4],symbols[name],textbase+offset,thumb=int(name=='function'))
    self.assertEqual(decoded[1],4);self.assertEqual(got,expected[offset:offset+4],(hex(textbase),offset,typ,name,decoded))
 def test_rejection_atomicity_and_output_sentinels(self):
  for typ,data in [(10,bytes.fromhex('00f000d0')),(30,bytes.fromhex('00f00090')),(51,bytes.fromhex('00f00080'))]:
   for symbol in [0x20000,0x4000001]:self.assertEqual(self.apply(typ,data,symbol,0x10000,ok=False)[1],data)
  for typ,data in [(10,bytes.fromhex('00f000c0')),(51,bytes.fromhex('80f30080')),(47,bytes.fromhex('00bf00bf')),(48,bytes.fromhex('40f20000')),(999,b'\0'*4),(2,b'\0'*3)]:
   decoded,out=self.apply(typ,data,0x20001,0x10000,ok=False);self.assertEqual(decoded,(1234567,99));self.assertEqual(out,data)
 def test_odd_data_and_thumb_postaddition_are_distinct(self):
  data=(1).to_bytes(4,'little')
  self.assertEqual(self.apply(2,data,0x20001,0,thumb=0)[1],(0x20002).to_bytes(4,'little'))
  self.assertEqual(self.apply(2,data,0x20001,0,thumb=1)[1],(0x20001).to_bytes(4,'little'))
  # Bounds/alignment rejection must not mutate bytes or decode outputs.
  decoded,out=self.apply(47,bytes.fromhex('40f20080'),0x20000,0,ok=False)
  self.assertEqual(decoded,(1234567,99));self.assertEqual(out,bytes.fromhex('40f20080'))

 def test_abs32_wrap_and_same_vm_formula_variant(self):
  self.assertEqual(self.apply(2,bytes.fromhex('f9ffffff'),3,0)[1],bytes.fromhex('fcffffff'))
  vm=OUT/'vm.wasm';digest=hashlib.sha256(vm.read_bytes()).digest();base=(ROOT/'packages/arm/relocation.rules').read_text();start=base.index('rule ELF_ABS32 4');end=base.index('\nend',start);variant=base[:start]+base[start:end].replace('add r0 r0 r1','sub r0 r0 r1')+base[end:];path=OUT/'variant.rules';path.write_text(variant)
  self.assertEqual(self.apply(2,(7).to_bytes(4,'little'),100,0)[1],(107).to_bytes(4,'little'));self.assertEqual(self.apply(2,(7).to_bytes(4,'little'),100,0,path=str(path.relative_to(ROOT)))[1],(93).to_bytes(4,'little'));self.assertEqual(hashlib.sha256(vm.read_bytes()).digest(),digest)
if __name__=='__main__':unittest.main()
