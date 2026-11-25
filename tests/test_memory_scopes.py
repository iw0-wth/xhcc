"""Temporary arena marks and complete unreachable-analysis scratch lifetime."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
SOURCE=r'''
#include "mirpass.h"
#include "memory.h"
#include "platform.h"
#include "diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void ark_log(const char *fmt,...){(void)fmt;}
void ark_abort(const char *s){fprintf(stderr,"%s\n",s);exit(90);}
const char *diag_num(char *b,int n){sprintf(b,"%d",n);return b;}
void diag_report_msg(DiagSeverity s,const DiagLoc *l,const char *m,const char *a,const char *b,const char *c){
 (void)c;printf("warning=%d line=%d ",s,l->line);printf(m,a,b);puts("");
}
static IrInst code[7],saved[7];
static unsigned hash(const void *p,unsigned n){const unsigned char *b=p;unsigned h=2166136261u;while(n--)h=(h^*b++)*16777619u;return h;}
int main(void){MemPoolMark mark,forward,bad;unsigned char *live;IrFunc f={0},empty={0};unsigned before;int i;
 mem_pool_reset();live=mem_pool_alloc(8);live[0]=42;mark=mem_pool_mark();
 mem_pool_alloc(16);forward=mem_pool_mark();
 if(mem_pool_rewind(mark)||mem_pool_used()!=8||mem_pool_high()!=24||live[0]!=42)return 1;
 if(mem_pool_rewind(forward)!=-1||mem_pool_used()!=8)return 2;
 bad=mark;bad.offset=1;if(mem_pool_rewind(bad)!=-1)return 3;
 bad.offset=-8;if(mem_pool_rewind(bad)!=-1)return 4;
 mem_pool_reset();live=mem_pool_alloc(8);live[0]=42;
 if(mem_pool_rewind(mark)!=-1||mem_pool_used()!=8||live[0]!=42)return 5;
 code[0].op=IR_FUNC_BEGIN;code[1].op=IR_PUSH_INT;code[1].val=1;
 code[2].op=IR_RETURN;code[3].op=IR_CALL;code[3].name="dead";code[3].line=77;
 code[4].op=IR_POP;code[5].op=IR_RETURN;code[6].op=IR_FUNC_END;
 f.name="probe";f.code=code;f.code_count=7;f.code_capacity=7;
 memcpy(saved,code,sizeof(code));before=hash(&f,sizeof(f));
 for(i=0;i<12;i++){
   mirpass_unreachable(&f);
   if(memcmp(saved,code,sizeof(code))||before!=hash(&f,sizeof(f))||live[0]!=42)return 6;
#ifndef BASELINE
   if(mem_pool_used()!=8)return 7;
#endif
 }
 mirpass_unreachable(&empty);mirpass_unreachable(0);
 printf("used=%d high=%d\n",mem_pool_used(),mem_pool_high());return 0;
}
'''
class MemoryScopeTests(unittest.TestCase):
 def test_marks_and_analysis_equivalence(self):
  sdk=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'))
  cc=os.environ.get('WASI_CC') or str(sdk/'bin'/('clang.exe' if os.name=='nt' else 'clang'))
  runtime=os.environ.get('WASMTIME') or shutil.which('wasmtime') or r'C:\Program Files\Wasmtime\bin\wasmtime.exe'
  env=os.environ.copy()
  if os.name=='nt':
   pd=Path(sys.executable).parent;env['PATH']=os.pathsep.join([str(pd),str(pd/'Library/bin'),env.get('PATH','')])
  with tempfile.TemporaryDirectory(prefix='memory-scopes-',dir=ROOT/'build') as temp:
   folder=Path(temp);source=folder/'scope.c';source.write_text(SOURCE,encoding='utf-8')
   production=ROOT/'src/ir/mirpass.c';baseline=folder/'baseline.c'
   baseline.write_text(production.read_text(encoding='utf-8').replace('mem_pool_rewind(scratch);','(void)scratch;'),encoding='utf-8')
   results=[]
   for name,pass_source in [('before',baseline),('after',production)]:
    output=folder/(name+'.wasm')
    argv=[cc,'--target=wasm32-wasip1','-std=c99','-ffunction-sections','-Wl,--gc-sections','-DARK_POOL_SIZE=8192','-DARK_LINK_BUF_SIZE=128','-include',str(ROOT/'include/xh_small.h'),'-I'+str(ROOT/'include'),str(ROOT/'src/core/memory.c'),str(ROOT/'src/ir/mir.c'),str(pass_source),str(source),'-o',str(output)]
    if name=='before':argv.append('-DBASELINE=1')
    built=subprocess.run(argv,env=env,capture_output=True,text=True,timeout=30);self.assertEqual(built.returncode,0,built.stdout+built.stderr)
    run=subprocess.run([runtime,'run',str(output)],env=env,capture_output=True,text=True,timeout=30);self.assertEqual(run.returncode,0,run.stdout+run.stderr);results.append(run.stdout.splitlines())
   self.assertEqual(results[0][:-1],results[1][:-1]);self.assertEqual(len(results[1][:-1]),12)
   self.assertTrue(all('3 instruction(s)' in line and 'line=77' in line for line in results[1][:-1]))
   def stats(line):return dict(item.split('=') for item in line.split())
   before,after=stats(results[0][-1]),stats(results[1][-1])
   self.assertEqual(int(after['used']),8);self.assertGreater(int(before['used']),8)
   self.assertGreater(int(before['high']),int(after['high']))
   print('MIR scope before',results[0][-1],'after',results[1][-1])
if __name__=='__main__':unittest.main()
