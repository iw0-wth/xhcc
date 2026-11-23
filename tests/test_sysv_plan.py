"""Standalone Type adapter + canonical wire codec; does not switch native ABI."""
from pathlib import Path
import os,sys,subprocess,unittest
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/sysv-plan-tests';SDK=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'));WASM=os.environ.get('WASMTIME',r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
HARNESS=r'''
#include "xh_sysv_plan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int fs_file_size(const char*p){FILE*f=fopen(p,"rb");long n;if(!f)return -1;fseek(f,0,SEEK_END);n=ftell(f);fclose(f);return(int)n;}
int fs_read_file(const char*p,char*b,int cap){FILE*f=fopen(p,"rb");int n;if(!f)return -1;n=(int)fread(b,1,cap-1,f);b[n]=0;fclose(f);return n;}
void diag_tool(int s,const char*w,const char*m,const char*a,const char*b,const char*c,const char*d,const char*e,const char*f){fprintf(stderr,"%s: %s\n",w,m);}
/* Minimal independent layout facts exercise the adapter. Production type.c
 * layout/serialization integration is covered by the frontend agent's gates. */
int type_size(Type*t){if(t->kind==TYPE_ARRAY)return t->size*type_size(t->base);return t->kind==TYPE_VOID?0:t->size;}
int type_align(Type*t){return t->align_override?t->align_override:t->kind==TYPE_ARRAY?type_align(t->base):t->size>8?8:t->size?t->size:1;}
int main(int argc,char**argv){Type integer={0},real={0},mixed={0},fn={0};StructField a={0},b={0};Type*params[2];XhSysvPlan plan,copy,old;unsigned char wire[4608],before[4608];unsigned n,i;int r;if(argc!=2&&argc!=3)return 2;
integer.kind=TYPE_LONG;integer.size=8;real.kind=TYPE_DOUBLE;real.size=8;mixed.kind=TYPE_STRUCT;mixed.size=16;mixed.align_override=8;a.type=&real;a.offset=0;a.next=&b;b.type=&integer;b.offset=8;mixed.fields=&a;params[0]=&mixed;params[1]=&integer;fn.kind=TYPE_FUNC;fn.base=&mixed;fn.params=params;fn.param_count=2;
if(xh_typeclass_set_path("x86_64",argc==3?argv[2]:"packages/x86_64/aggregate.rules"))return 2;
r=xh_build_sysv_plan("x86_64.aggregate","abiplan",&fn,0,2,&plan);if(r)return 3;
if(!strcmp(argv[1],"fingerprint")){printf("%u\n",plan.rule_fingerprint);return 0;}
if(!strcmp(argv[1],"build")){printf("%u %u %u %u %u %u %u\n",plan.version,plan.arg_count,plan.result.fragments[0].class_id,plan.result.fragments[1].class_id,plan.args[0].fragments[0].bank,plan.args[0].fragments[1].bank,plan.args[1].fragments[0].index);return 0;}
n=xh_sysv_plan_wire_size(&plan);if(!n||n>sizeof(wire)||xh_sysv_plan_encode(&plan,wire,sizeof(wire))!=(int)n||xh_sysv_plan_decode(wire,n,&copy)||memcmp(&copy,&plan,sizeof(plan)))return 4;
if(!strcmp(argv[1],"wire")){memset(&copy,0xa5,sizeof(copy));old=copy;for(i=0;i<n;i++)if(!xh_sysv_plan_decode(wire,i,&copy)||memcmp(&old,&copy,sizeof(copy)))return 5;memcpy(before,wire,n);memset(wire,0xa5,sizeof(wire));if(xh_sysv_plan_encode(&plan,wire,n-1)!=-1)return 6;for(i=0;i<sizeof(wire);i++)if(wire[i]!=0xa5)return 7;memcpy(wire,before,n);wire[4]=99;if(!xh_sysv_plan_decode(wire,n,&copy)||memcmp(&old,&copy,sizeof(copy)))return 8;printf("wire %u atomic\n",n);return 0;}
if(!strcmp(argv[1],"corrupt")){memcpy(before,wire,n);unsigned offsets[]={0,4,8,16,20,24,28,32,64,72,76,80,96,104};memset(&copy,0xa5,sizeof(copy));old=copy;for(i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++){memcpy(wire,before,n);wire[offsets[i]]=255;wire[offsets[i]+1]=255;wire[offsets[i]+2]=255;wire[offsets[i]+3]=255;if(!xh_sysv_plan_decode(wire,n,&copy)||memcmp(&copy,&old,sizeof(copy)))return 9;}puts("corrupt atomic");return 0;}
if(!strcmp(argv[1],"badtype")){old=plan;a.bit_width=1;r=xh_build_sysv_plan("x86_64.aggregate","abiplan",&fn,0,2,&plan);printf("%d %d\n",r,memcmp(&old,&plan,sizeof(plan))==0);return 0;}
return 2;}
'''
class SysvPlan(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  OUT.mkdir(parents=True,exist_ok=True);(OUT/'harness.c').write_text(HARNESS);env=os.environ.copy();pd=Path(sys.executable).parent;env['PATH']=os.pathsep.join([str(pd),str(pd/'Library/bin'),env.get('PATH','')])
  args=[str(SDK/'bin/clang.exe'),'--target=wasm32-wasip1','-O2','-Iinclude','-Iconfig','-include','include/xh_small.h','build/sysv-plan-tests/harness.c',*['src/backend/rules/'+x+'.c' for x in ['sysv_plan','abiplan_wire','abiplan','typeclass','template']],'-o','build/sysv-plan-tests/vm.wasm']
  p=subprocess.run(args,cwd=ROOT,env=env,capture_output=True,text=True)
  if p.returncode:raise RuntimeError(p.stderr)
 def run_case(self,mode):
  p=subprocess.run([WASM,'run','--dir=.','build/sysv-plan-tests/vm.wasm',mode],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stdout+p.stderr);return p.stdout.strip()
 def test_fingerprint_tracks_rule_file_bytes(self):
  path=OUT/'fingerprint.rules';base=(ROOT/'packages/x86_64/aggregate.rules').read_bytes();values=[]
  for data in [base,base+b'\n# content change\n']:
   path.write_bytes(data);p=subprocess.run([WASM,'run','--dir=.','build/sysv-plan-tests/vm.wasm','fingerprint',str(path.relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr);values.append(int(p.stdout.strip()))
  self.assertNotEqual(values[0],values[1])
 def test_type_adapter_mixed_plan(self):self.assertEqual(self.run_case('build'),'1 2 2 1 1 0 1')
 def test_codec_roundtrip_truncation_and_atomicity(self):self.assertEqual(self.run_case('wire'),'wire 196 atomic')
 def test_corrupt_wire_fields_preserve_output(self):self.assertEqual(self.run_case('corrupt'),'corrupt atomic')
 def test_unsupported_bitfield_preserves_output(self):self.assertEqual(self.run_case('badtype'),'-1 1')
if __name__=='__main__':unittest.main()
