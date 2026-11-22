"""Execute actual bounded C relocation VM in Wasm; failures must be atomic."""
from pathlib import Path
import os,sys,subprocess,unittest
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/relocation-tests'
SDK=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'));WASM=os.environ.get('WASMTIME',r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
HARNESS=r'''
#include "xh_relocation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int fs_file_size(const char*p){FILE*f=fopen(p,"rb");long n;if(!f)return -1;fseek(f,0,SEEK_END);n=ftell(f);fclose(f);return(int)n;}
int fs_read_file(const char*p,char*b,int cap){FILE*f=fopen(p,"rb");int n;if(!f)return -1;n=(int)fread(b,1,cap-1,f);b[n]=0;fclose(f);return n;}
void diag_tool(int s,const char*w,const char*m,const char*a,const char*b,const char*c,const char*d,const char*e,const char*f){fprintf(stderr,"%s: %s\n",w,m);}
int main(int n,char**v){unsigned char code[12],before[12];int i,rc;XhRelocContext c;if(n!=9)return 2;for(i=0;i<12;i++)code[i]=(unsigned char)strtoul(v[8],0,0);if(v[8][0]=='h'){for(i=0;i<12;i++){unsigned b;if(sscanf(v[8]+1+i*2,"%2x",&b)!=1)return 2;code[i]=(unsigned char)b;}}memcpy(before,code,12);c.symbol=strtoull(v[3],0,0);c.addend=strtoull(v[4],0,0);c.place=strtoull(v[5],0,0);c.image_base=0;if(xh_relocation_set_path("x86",v[1]))return 2;rc=xh_relocation_apply("x86",v[2],code,(size_t)strtoul(v[6],0,0),(size_t)strtoul(v[7],0,0),&c);for(i=0;i<12;i++)printf("%02x",code[i]);printf("\n");return rc?1:0;}
'''
class RelocationRules(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  OUT.mkdir(parents=True,exist_ok=True);(OUT/'harness.c').write_text(HARNESS)
  env=os.environ.copy();pd=Path(sys.executable).parent;env['PATH']=str(pd)+os.pathsep+str(pd/'Library/bin')+os.pathsep+env.get('PATH','')
  p=subprocess.run([str(SDK/'bin/clang.exe'),'--target=wasm32-wasip1','-O2','-Iinclude','-include','include/xh_small.h','build/relocation-tests/harness.c','src/backend/rules/relocation.c','-o','build/relocation-tests/vm.wasm'],cwd=ROOT,env=env,capture_output=True,text=True)
  if p.returncode:raise RuntimeError(p.stderr)
 def apply(self,key,s,a=0,p=0,length=12,site=2,fill=0xaa,path='packages/x86/relocation.rules',ok=True):
  result=subprocess.run([WASM,'run','--dir=.','build/relocation-tests/vm.wasm',path,key,str(s&0xffffffffffffffff),str(a&0xffffffffffffffff),str(p&0xffffffffffffffff),str(length),str(site),str(fill)],cwd=ROOT,capture_output=True,text=True)
  self.assertEqual(result.returncode,0 if ok else 1,result.stderr);return bytes.fromhex(result.stdout.strip())
 def test_rv_pair_native_oracle_and_atomic_failures(self):
  import struct
  first=0x00000297;seconds=[0x00028293,0x000280e7]
  path='packages/riscv32/relocation.rules';count=0
  for second in seconds:
   original=b'\xaa\xaa'+struct.pack('<II',first,second)+b'\xaa\xaa'
   for d in [-2147483648,-1048577,-4097,-2049,-2048,-1,0,1,2047,2048,4095,4096,1048576,2147481599]:
    hi=(d+0x800)>>12;lo=d-(hi<<12);want=struct.pack('<II',(first&0xfff)|((hi&0xfffff)<<12),(second&0xfffff)|((lo&0xfff)<<20))
    data=self.apply('RV_AUIPC_PAIR',0x10000+d,p=0x10000,fill='h'+original.hex(),path=path);self.assertEqual(data[2:10],want,(d,hex(second)));self.assertEqual(data[:2]+data[10:],b'\xaa'*4);count+=1
   for d in [-2147485697,2147481600]:self.assertEqual(self.apply('RV_AUIPC_PAIR',0x10000+d,p=0x10000,fill='h'+original.hex(),path=path,ok=False),original)
   for corrupt in [struct.pack('<II',0,second),struct.pack('<II',first,0)]:
    broken=b'\xaa\xaa'+corrupt+b'\xaa\xaa';self.assertEqual(self.apply('RV_AUIPC_PAIR',7,fill='h'+broken.hex(),path=path,ok=False),broken)
   self.assertEqual(self.apply('RV_AUIPC_PAIR',7,length=9,fill='h'+original.hex(),path=path,ok=False),original)
  self.assertEqual(count,28)
 def test_new_ops_and_embedded_nul(self):
  path=OUT/'ops.rules';path.write_text('XHRELOC 1\nrule T 4\nset r0 S\nshr r1 r0 3\nconst r2 1\neq r1 r2\nnot r0 r0\nwrite r0 0 4\nend\n')
  self.assertEqual(self.apply('T',8,path=str(path.relative_to(ROOT)))[2:6],(0xfffffff7).to_bytes(4,'little'));self.assertEqual(self.apply('T',16,path=str(path.relative_to(ROOT)),ok=False),b'\xaa'*12)
  path.write_bytes(path.read_bytes()+b'\0malicious');self.assertEqual(self.apply('T',8,path=str(path.relative_to(ROOT)),ok=False),b'\xaa'*12)
 def test_absolute_and_pc_formulas(self):
  for key,width,s,a,p,want in [('ABS32',4,0x12340000,7,0,0x12340007),('ABS64',8,0x1234567800000000,7,0,0x1234567800000007),('PC32',4,0x1000,-5,0x1100,-265),('PC64',8,0x100000000,-7,0x100000100,-267)]:
   data=self.apply(key,s,a,p);self.assertEqual(data[2:2+width],(want&((1<<(width*8))-1)).to_bytes(width,'little'));self.assertEqual(data[:2],b'\xaa'*2);self.assertEqual(data[2+width:],b'\xaa'*(10-width))
 def test_range_bounds_and_atomicity(self):
  for s in [0,0x7fffffff,0x80000000,0xffffffff]:self.assertEqual(int.from_bytes(self.apply('ABS32',s)[2:6],'little'),s)
  for s,a,p in [(0x100000000,0,0),(0,-1,0)]:self.assertEqual(self.apply('ABS32',s,a,p,ok=False),b'\xaa'*12)
  self.assertEqual(self.apply('PC32',0x80000004,ok=False),b'\xaa'*12)
  self.assertEqual(self.apply('ABS64',1,length=7,ok=False),b'\xaa'*12)
 def test_write_then_failed_assertion_is_atomic(self):
  rules=OUT/'atomic.rules';rules.write_text('XHRELOC 1\nrule T 4\nset r0 S\nwrite r0 0 4\nalign r0 4\nend\n')
  self.assertEqual(self.apply('T',3,path=str(rules.relative_to(ROOT)),ok=False),b'\xaa'*12)
 def test_masks_alignment_and_signed_pc_boundaries(self):
  rules=OUT/'mask.rules';rules.write_text('XHRELOC 1\nrule T 4\nread r0 0 4\ncheckmask r0 255 170\nset r1 S\nwrite r1 0 4\nend\n')
  self.assertEqual(self.apply('T',7,path=str(rules.relative_to(ROOT)))[2:6],b'\x07\0\0\0')
  self.assertEqual(self.apply('T',7,path=str(rules.relative_to(ROOT)),fill=0xab,ok=False),b'\xab'*12)
  for displacement in [-0x80000000,0x7fffffff]:
   symbol=displacement+4
   data=self.apply('PC32',symbol);self.assertEqual(int.from_bytes(data[2:6],'little'),displacement&0xffffffff)
  self.assertEqual(self.apply('PC32',0x80000004,ok=False),b'\xaa'*12)
 def test_malformed_and_uninitialized_are_rejected(self):
  rules=OUT/'bad.rules'
  for body in ['add r0 r1 r2','set r0 S\nwrite r0 7 8','set r0 S\nshl r1 r0 64','const r0 18446744073709551616']:
   rules.write_text('XHRELOC 1\nrule T 4\n'+body+'\nend\n');self.assertEqual(self.apply('T',3,path=str(rules.relative_to(ROOT)),ok=False),b'\xaa'*12)
 def test_legacy_cross_object_pc32_executes(self):
  sys.path.insert(0,str(ROOT/'build/test-deps'))
  from unicorn import Uc,UC_ARCH_X86,UC_MODE_32
  from unicorn.x86_const import UC_X86_REG_ESP,UC_X86_REG_EAX
  source=OUT/'main.c';library=OUT/'library.c';source.write_text('extern int one(int);int main(void){return one(9);}\n');library.write_text('int one(int x){return x+11;}\n')
  command=[WASM,'run','--dir=.','build/wasm/dev/all/xhcc.wasm','--ark-compat','-target','x86']
  for name in ['main','library']:
   p=subprocess.run([*command,'-c','-o',str((OUT/(name+'.ark_obj')).relative_to(ROOT)),str((OUT/(name+'.c')).relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stdout+p.stderr)
  output=OUT/'linked.bin';p=subprocess.run([*command,'-link',str((OUT/'main.ark_obj').relative_to(ROOT)),str((OUT/'library.ark_obj').relative_to(ROOT)),'-o',str(output.relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stdout+p.stderr)
  code=output.read_bytes();uc=Uc(UC_ARCH_X86,UC_MODE_32);uc.mem_map(0x10000,0x20000);uc.mem_map(0x700000,0x20000);uc.mem_write(0x10000,code);uc.reg_write(UC_X86_REG_ESP,0x71effc);uc.mem_write(0x71effc,(0x2f000).to_bytes(4,'little'));uc.emu_start(0x10000,0x2f000,count=100000);self.assertEqual(uc.reg_read(UC_X86_REG_EAX),20)
  # Relink identical private objects under a copied package directory. Only
  # the external PC bias changes; no compiler bytes or input objects change.
  import tempfile,shutil,hashlib
  compiler=ROOT/'build/wasm/dev/all/xhcc.wasm';digest=hashlib.sha256(compiler.read_bytes()).digest()
  with tempfile.TemporaryDirectory(prefix='xh-rel-') as tmp:
   project=Path(tmp);shutil.copytree(ROOT/'packages/x86',project/'packages/x86');shutil.copy2(OUT/'main.ark_obj',project/'main.ark_obj');shutil.copy2(OUT/'library.ark_obj',project/'library.ark_obj')
   local_command=[WASM,'run','--dir=.',str(compiler),'--ark-compat','-target','x86','-link','main.ark_obj','library.ark_obj','-o','image.bin']
   p=subprocess.run(local_command,cwd=project,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr);normal=(project/'image.bin').read_bytes()
   rule=project/'packages/x86/relocation.rules';rule.write_text(rule.read_text().replace('const r1 4','const r1 8'))
   p=subprocess.run(local_command,cwd=project,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr);changed=(project/'image.bin').read_bytes();self.assertNotEqual(normal,changed);self.assertEqual(len(normal),len(changed))
  self.assertEqual(hashlib.sha256(compiler.read_bytes()).digest(),digest)
 def test_rv_legacy_cross_object_pair_executes_and_variant(self):
  import tempfile,shutil,hashlib
  sys.path.insert(0,str(ROOT/'build/test-deps'))
  from unicorn import Uc,UC_ARCH_RISCV,UC_MODE_RISCV32
  from unicorn.riscv_const import UC_RISCV_REG_SP,UC_RISCV_REG_RA,UC_RISCV_REG_A0
  compiler=ROOT/'build/wasm/dev/all/xhcc.wasm';digest=hashlib.sha256(compiler.read_bytes()).digest();source=OUT/'rvmain.c';library=OUT/'rvlib.c';source.write_text('extern int one(int);int main(void){return one(9);}\n');library.write_text('int one(int x){return x+11;}\n')
  command=[WASM,'run','--dir=.',str(compiler),'--ark-compat','-target','riscv32']
  for name in ['rvmain','rvlib']:
   p=subprocess.run([*command,'-c','-o',str((OUT/(name+'.ark_obj')).relative_to(ROOT)),str((OUT/(name+'.c')).relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stdout+p.stderr)
  with tempfile.TemporaryDirectory(prefix='xh-rv-rel-') as tmp:
   root=Path(tmp);shutil.copytree(ROOT/'packages/riscv32',root/'packages/riscv32')
   for name in ['rvmain','rvlib']:shutil.copy2(OUT/(name+'.ark_obj'),root/(name+'.ark_obj'))
   link=[*command,'-link','rvmain.ark_obj','rvlib.ark_obj','-o','image.bin']
   p=subprocess.run(link,cwd=root,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stdout+p.stderr);normal=(root/'image.bin').read_bytes()
   uc=Uc(UC_ARCH_RISCV,UC_MODE_RISCV32);uc.mem_map(0x10000,0x20000);uc.mem_map(0x700000,0x20000);uc.mem_write(0x10000,normal);uc.reg_write(UC_RISCV_REG_SP,0x71f000);uc.reg_write(UC_RISCV_REG_RA,0x2f000);uc.emu_start(0x10000,0x2f000,count=100000);self.assertEqual(uc.reg_read(UC_RISCV_REG_A0),20)
   rule=root/'packages/riscv32/relocation.rules';rule.write_text(rule.read_text().replace('set r3 P','const r3 0',1));p=subprocess.run(link,cwd=root,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stdout+p.stderr);self.assertNotEqual((root/'image.bin').read_bytes(),normal)
  self.assertEqual(hashlib.sha256(compiler.read_bytes()).digest(),digest)
 def test_external_formula_variant_same_vm(self):
  import hashlib
  vm=OUT/'vm.wasm';digest=hashlib.sha256(vm.read_bytes()).digest();rules=OUT/'variant.rules';text=(ROOT/'packages/x86/relocation.rules').read_text();rules.write_text(text.replace('const r1 4','const r1 8'))
  self.assertEqual(int.from_bytes(self.apply('PC32',0x1200,p=0x1000)[2:6],'little'),508)
  self.assertEqual(int.from_bytes(self.apply('PC32',0x1200,p=0x1000,path=str(rules.relative_to(ROOT)))[2:6],'little'),504);self.assertEqual(hashlib.sha256(vm.read_bytes()).digest(),digest)
if __name__=='__main__':unittest.main()
