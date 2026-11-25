"""Bounded object attributes profiles; small actual C parser harness only."""
from pathlib import Path
import os,sys,subprocess,unittest
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/object-profile-tests'
SDK=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'));WASM=os.environ.get('WASMTIME',r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
HARNESS=r'''
#include "xh_object_rules.h"
#include <stdio.h>
#include <stdlib.h>
int fs_file_size(const char*p){FILE*f=fopen(p,"rb");long n;if(!f)return -1;fseek(f,0,SEEK_END);n=ftell(f);fclose(f);return(int)n;}
int fs_read_file(const char*p,char*b,int cap){FILE*f=fopen(p,"rb");int n;if(!f)return -1;n=(int)fread(b,1,cap-1,f);b[n]=0;fclose(f);return n;}
void diag_tool(int s,const char*w,const char*m,const char*a,const char*b,const char*c,const char*d,const char*e,const char*f){fprintf(stderr,"%s: %s\n",w,m);}
int main(int n,char**v){const XhObjectTarget*t;const unsigned char*b;unsigned len,i;if(n!=4)return 2;if(xh_object_rules_set_path("arm",v[1]))return 2;t=xh_object_rules_get("arm");if(!t)return 1;if(xh_object_rules_attributes_for_profile(t,v[2],atoi(v[3]),&b,&len))return 1;for(i=0;i<len;i++)printf("%02x",b[i]);puts("");return 0;}
'''
class ObjectProfiles(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  OUT.mkdir(parents=True,exist_ok=True);(OUT/'harness.c').write_text(HARNESS);env=os.environ.copy();pd=Path(sys.executable).parent;env['PATH']=os.pathsep.join([str(pd),str(pd/'Library/bin'),env.get('PATH','')]);p=subprocess.run([str(SDK/'bin/clang.exe'),'--target=wasm32-wasip1','-O2','-Iinclude','-include','include/xh_small.h','build/object-profile-tests/harness.c','src/backend/rules/object_rules.c','-o','build/object-profile-tests/vm.wasm'],cwd=ROOT,env=env,capture_output=True,text=True)
  if p.returncode:raise RuntimeError(p.stderr)
 def invoke(self,path,profile='thumb1',required=1,ok=True):
  p=subprocess.run([WASM,'run','--dir=.','build/object-profile-tests/vm.wasm',str(path),profile,str(required)],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0 if ok else 1,p.stdout+p.stderr);return bytes.fromhex(p.stdout.strip()) if ok else p.stderr
 def test_default_profiles_and_legacy_fallback(self):
  base=(ROOT/'packages/arm/object.rules').read_text();self.assertEqual(self.invoke('packages/arm/object.rules'),bytes.fromhex('4115000000616561626900010b000000060c074d0901'))
  path=OUT/'legacy.rules';path.write_text('\n'.join(s for s in base.splitlines() if not s.startswith('attributes_profile '))+'\n');legacy=bytes.fromhex(next(s.split()[1] for s in base.splitlines() if s.startswith('attributes ')))
  self.assertEqual(self.invoke(path.relative_to(ROOT),'thumb2',0),legacy);self.assertIn('required',self.invoke(path.relative_to(ROOT),ok=False))
 def test_invalid_duplicate_capacity_and_attribute_bytes(self):
  base=(ROOT/'packages/arm/object.rules').read_text();path=OUT/'bad.rules'
  bad=['attributes_profile thumb1 -\n','attributes_profile INVALID -\n','attributes_profile thumb0 41\n','attributes_profile thumb0 4100000000\n','attributes_profile thumb0 zz\n',''.join(f'attributes_profile extra{i} -\n' for i in range(3))]
  for extra in bad:path.write_text(base+extra);self.invoke(path.relative_to(ROOT),ok=False)
 def test_profile_variant_changes_selected_bytes(self):
  base=(ROOT/'packages/arm/object.rules').read_text();path=OUT/'variant.rules';path.write_text(base.replace('060c074d0901','060b074d0901'));a=self.invoke('packages/arm/object.rules');b=self.invoke(path.relative_to(ROOT));self.assertNotEqual(a,b)
if __name__=='__main__':unittest.main()
