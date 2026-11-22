"""Execute the C target descriptor loader in Wasm, including custom IDs."""
from pathlib import Path
import os,sys,subprocess,unittest
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/target-rule-tests';SDK=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'));WASM=os.environ.get('WASMTIME',r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
HARNESS=r'''
#include "xh_target_rules.h"
#include <stdio.h>
int fs_file_size(const char*p){FILE*f=fopen(p,"rb");long n;if(!f)return -1;fseek(f,0,SEEK_END);n=ftell(f);fclose(f);return(int)n;}
int fs_read_file(const char*p,char*b,int c){FILE*f=fopen(p,"rb");int n;if(!f)return -1;n=fread(b,1,c-1,f);b[n]=0;fclose(f);return n;}
void diag_tool(int s,const char*w,const char*m,const char*a,const char*b,const char*c,const char*d,const char*e,const char*f){fprintf(stderr,"%s: %s\n",w,m);}
int main(int n,char**v){const XhTargetDescriptor*d;int i;if(n!=3)return 2;if(xh_target_rules_set_path(v[1],v[2]))return 1;d=xh_target_rules_get(v[1]);if(!d)return 1;printf("%s %s %s %u %u %u %s %u %u %u\n",d->id,d->engine,d->abi,d->pointer_bytes,d->long_bytes,d->slot_bytes,d->va_mode,d->armcc_asm,d->native_asm,d->arm_immediates);for(i=0;i<d->macro_count;i++)printf("%s=%s\n",d->macros[i].name,d->macros[i].value);return 0;}
'''
class TargetRules(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  OUT.mkdir(parents=True,exist_ok=True);(OUT/'harness.c').write_text(HARNESS);env=os.environ.copy();pd=Path(sys.executable).parent;env['PATH']=str(pd)+os.pathsep+str(pd/'Library/bin')+os.pathsep+env.get('PATH','');p=subprocess.run([str(SDK/'bin/clang.exe'),'--target=wasm32-wasip1','-O2','-Iinclude','-include','include/xh_small.h','build/target-rule-tests/harness.c','src/backend/rules/target_rules.c','-o','build/target-rule-tests/vm.wasm'],cwd=ROOT,env=env,capture_output=True,text=True)
  if p.returncode:raise RuntimeError(p.stderr)
 def load(self,engine,path,ok=True):
  p=subprocess.run([WASM,'run','--dir=.','build/target-rule-tests/vm.wasm',engine,path],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0 if ok else 1,p.stderr);return p.stdout
 def test_all_supported_engine_contracts(self):
  for engine in ['arm','riscv32','x86','x86_64','c']:
   output=self.load(engine,f'packages/{engine}/target.rules');self.assertIn(' '+engine+' ',output)
  self.assertIn('8 8 8 sysv',self.load('x86_64','packages/x86_64/target.rules'))
 def test_external_custom_metadata_without_rebuild(self):
  import hashlib
  vm=OUT/'vm.wasm';digest=hashlib.sha256(vm.read_bytes()).digest();path=OUT/'custom.rules';path.write_text((ROOT/'packages/riscv32/target.rules').read_text().replace('id riscv32','id rv32-custom')+'macro __CUSTOM_BOARD__ 73\n');out=self.load('riscv32',str(path.relative_to(ROOT)));self.assertIn('rv32-custom riscv32',out);self.assertIn('__CUSTOM_BOARD__=73',out);self.assertEqual(hashlib.sha256(vm.read_bytes()).digest(),digest)
 def test_compiler_custom_id_and_macro_with_unchanged_binary(self):
  import tempfile,shutil,hashlib
  compiler=ROOT/'build/wasm/xhcc.wasm';digest=hashlib.sha256(compiler.read_bytes()).digest()
  with tempfile.TemporaryDirectory(prefix='xh-target-') as tmp:
   root=Path(tmp);pkg=root/'variant';shutil.copytree(ROOT/'packages/riscv32',pkg);manifest=(pkg/'package.ini').read_text();(pkg/'package.ini').write_text('\n'.join('id=variant' if s.startswith('id=') else 'tools=variant-gcc' if s.startswith('tools=') else s for s in manifest.splitlines())+'\n')
   path=pkg/'target.rules';path.write_text(path.read_text().replace('id riscv32','id custom-rv-board')+'macro __BOARD_REVISION__ 73\n')
   source=OUT/'custom.c';source.write_text('#if __BOARD_REVISION__ != 73 || __SIZEOF_POINTER__ != 4 || __SIZEOF_LONG__ != 4\n#error descriptor failed\n#endif\nconst char *identity=__XHCC_TARGET_ID__; int entry(void){return __BOARD_REVISION__;}\n');output=OUT/'custom.o'
   command=[sys.executable,'tools/xhcc.py','--package-root',str(root),'variant-gcc']
   p=subprocess.run([*command,'-c',str(source.relative_to(ROOT)),'-o',str(output.relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr);self.assertIn(b'custom-rv-board',output.read_bytes())
   p=subprocess.run([*command,'-E','-dM',str(source.relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr);self.assertIn('#define __BOARD_REVISION__ 73',p.stdout);self.assertIn('custom-rv-board',p.stdout)
  self.assertEqual(hashlib.sha256(compiler.read_bytes()).digest(),digest)
 def test_invalid_layout_and_dialect_rejected(self):
  base=(ROOT/'packages/riscv32/target.rules').read_text();path=OUT/'bad.rules'
  for bad in [base.replace('pointer 4','pointer 8'),base.replace('slot 4','slot 8'),base.replace('native_asm 1','native_asm 0'),base.replace('abi ilp32','abi invented'),base+'pointer 4\n',base+'macro __riscv 2\n',base+'unknown 1\n',base+'\0trailing']:
   path.write_text(bad);self.load('riscv32',str(path.relative_to(ROOT)),ok=False)
if __name__=='__main__':unittest.main()
