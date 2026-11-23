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
int type_is_integer(Type*t){return t->kind==TYPE_CHAR||t->kind==TYPE_SHORT||t->kind==TYPE_LONG||t->kind==TYPE_INT||t->kind==TYPE_LLONG;}
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
if(!strcmp(argv[1],"small")){mixed.size=8;a.type=&integer;a.next=0;params[0]=&mixed;params[1]=&integer;fn.base=&mixed;if(xh_build_sysv_plan("x86_64.aggregate","abiplan",&fn,0,2,&copy)||copy.flags!=2||copy.args[0].size!=8)return 12;n=xh_sysv_plan_wire_size(&copy);if(xh_sysv_plan_encode(&copy,wire,sizeof(wire))!=(int)n||xh_sysv_plan_decode(wire,n,&old)||old.flags!=2)return 13;mixed.is_packed=1;if(xh_build_sysv_plan("x86_64.aggregate","abiplan",&fn,0,2,&copy)||copy.flags)return 14;puts("small struct certified; packed rejected");return 0;}
if(!strcmp(argv[1],"mixedpair")){unsigned route;for(route=0;route<3;route++){mixed.size=16;mixed.align_override=8;a.type=route==0?&integer:&real;a.next=&b;b.type=route==1?&integer:&real;b.offset=8;b.next=0;params[0]=&mixed;params[1]=&integer;fn.base=&mixed;if(xh_build_sysv_plan("x86_64.aggregate","abiplan",&fn,0,2,&copy)||copy.flags!=16||copy.sret||copy.result.count!=2||copy.args[0].count!=2)return 21;if(copy.result.fragments[0].class_id!=(route==0?1:2)||copy.result.fragments[1].class_id!=(route==1?1:2))return 22;n=xh_sysv_plan_wire_size(&copy);if(xh_sysv_plan_encode(&copy,wire,sizeof(wire))!=(int)n||xh_sysv_plan_decode(wire,n,&old)||old.flags!=16)return 23;}puts("mixed and SSE pairs certified and serialized");return 0;}
if(!strcmp(argv[1],"pair")){mixed.size=16;mixed.align_override=8;a.type=&integer;a.next=&b;b.type=&integer;b.offset=8;b.next=0;params[0]=&mixed;params[1]=&integer;fn.base=&mixed;if(xh_build_sysv_plan("x86_64.aggregate","abiplan",&fn,0,2,&copy)||copy.flags!=8||copy.sret||copy.result.count!=2||copy.args[0].count!=2||copy.args[0].fragments[1].index!=1||copy.args[1].fragments[0].index!=2)return 19;n=xh_sysv_plan_wire_size(&copy);if(xh_sysv_plan_encode(&copy,wire,sizeof(wire))!=(int)n||xh_sysv_plan_decode(wire,n,&old)||old.flags!=8)return 20;puts("integer pair certified and serialized");return 0;}
if(!strcmp(argv[1],"sse")){mixed.size=8;mixed.align_override=8;a.type=&real;a.next=0;mixed.fields=&a;params[0]=&mixed;params[1]=&integer;fn.base=&mixed;if(xh_build_sysv_plan("x86_64.aggregate","abiplan",&fn,0,2,&copy)||copy.flags!=4||copy.result.fragments[0].class_id!=2||copy.args[0].fragments[0].bank!=1)return 16;n=xh_sysv_plan_wire_size(&copy);if(xh_sysv_plan_encode(&copy,wire,sizeof(wire))!=(int)n||xh_sysv_plan_decode(wire,n,&old)||old.flags!=4)return 17;copy.flags=3;if(xh_sysv_plan_wire_size(&copy))return 18;puts("single SSE structure certified; invalid combined flags rejected");return 0;}
if(!strcmp(argv[1],"irregular")){Type byte={0};StructField fields[8];unsigned size,j;byte.kind=TYPE_CHAR;byte.size=1;for(size=1;size<=8;size++){memset(fields,0,sizeof(fields));for(j=0;j<size;j++){fields[j].type=&byte;fields[j].offset=j;fields[j].next=j+1<size?&fields[j+1]:0;}mixed.size=size;mixed.align_override=1;mixed.fields=fields;params[0]=&mixed;params[1]=&integer;fn.base=&mixed;if(xh_build_sysv_plan("x86_64.aggregate","abiplan",&fn,0,2,&copy)||copy.flags!=2||copy.args[0].size!=size)return 15;}puts("sizes one through eight certified");return 0;}
if(!strcmp(argv[1],"adjust")){Type array={0},funptr={0};array.kind=TYPE_ARRAY;array.base=&real;array.size=1;funptr.kind=TYPE_FUNC;funptr.base=&integer;params[0]=&array;params[1]=&funptr;fn.base=&integer;if(xh_build_sysv_plan("x86_64.aggregate","abiplan",&fn,0,2,&copy)||copy.flags!=XH_SYSV_PLAN_SCALAR_VERIFIED||copy.args[0].size!=8||copy.args[1].size!=8)return 11;puts("adjusted scalar pointers");return 0;}
if(!strcmp(argv[1],"v1")){XhSysvPlan oldplan=plan;oldplan.version=1;oldplan.flags=0;n=xh_sysv_plan_wire_size(&oldplan);if(n!=196||xh_sysv_plan_encode(&oldplan,wire,sizeof(wire))!=(int)n||xh_sysv_plan_decode(wire,n,&copy)||copy.version!=1||copy.flags)return 10;puts("v1 inspect only");return 0;}
if(!strcmp(argv[1],"corrupt")){memcpy(before,wire,n);unsigned offsets[]={0,4,8,16,20,24,28,32,36,68,76,80,84,100,108};memset(&copy,0xa5,sizeof(copy));old=copy;for(i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++){memcpy(wire,before,n);wire[offsets[i]]=255;wire[offsets[i]+1]=255;wire[offsets[i]+2]=255;wire[offsets[i]+3]=255;if(!xh_sysv_plan_decode(wire,n,&copy)||memcmp(&copy,&old,sizeof(copy)))return 9;}puts("corrupt atomic");return 0;}
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
 def test_type_adapter_mixed_plan(self):self.assertEqual(self.run_case('build'),'2 2 2 1 1 0 1')
 def test_codec_roundtrip_truncation_and_atomicity(self):self.assertEqual(self.run_case('wire'),'wire 200 atomic')
 def test_small_integer_struct_certification_codec(self):self.assertEqual(self.run_case('small'),'small struct certified; packed rejected')
 def test_mixed_and_sse_pair_structure_certification(self):self.assertEqual(self.run_case('mixedpair'),'mixed and SSE pairs certified and serialized')
 def test_integer_pair_structure_certification(self):self.assertEqual(self.run_case('pair'),'integer pair certified and serialized')
 def test_single_sse_structure_certification(self):self.assertEqual(self.run_case('sse'),'single SSE structure certified; invalid combined flags rejected')
 def test_irregular_small_structure_certification(self):self.assertEqual(self.run_case('irregular'),'sizes one through eight certified')
 def test_array_and_function_parameter_adjustment(self):self.assertEqual(self.run_case('adjust'),'adjusted scalar pointers')
 def test_genuine_version_one_inspection_compatibility(self):self.assertEqual(self.run_case('v1'),'v1 inspect only')
 def test_corrupt_wire_fields_preserve_output(self):self.assertEqual(self.run_case('corrupt'),'corrupt atomic')
 def test_unsupported_bitfield_preserves_output(self):self.assertEqual(self.run_case('badtype'),'-1 1')
if __name__=='__main__':unittest.main()
