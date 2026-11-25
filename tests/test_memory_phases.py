"""Allocation-free phase labels, checkpoints, and clean oversized-request failure."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include "memory.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
void ark_log(const char *fmt, ...) { va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap); }
void ark_abort(const char *msg) { fprintf(stderr,"abort=%s used=%d high=%d\n",msg,mem_pool_used(),mem_pool_high()); exit(90); }
int main(int argc,char **argv) {
  unsigned char *p; int gen; static const char phase[]="parse";
  mem_pool_reset(); gen=mem_pool_generation();
  if(!ark_streq(mem_pool_phase(),"unclassified"))return 1;
  mem_pool_set_phase(phase);
  if(mem_pool_phase()!=phase||mem_pool_used()||mem_pool_high())return 2;
  if(argc>1) {
    p=mem_pool_alloc(56);
    if(argv[1][0]=='g')mem_realloc(p,56,INT_MAX-7);
    else if(argv[1][0]=='o')mem_pool_alloc(INT_MAX);
    else if(argv[1][0]=='r')mem_realloc(0,0,INT_MAX-6);
    else if(argv[1][0]=='e'){mem_pool_alloc(8);mem_pool_alloc(1);}
    else mem_pool_alloc(9);
    return 3;
  }
  p=mem_pool_alloc(1); if(mem_pool_used()!=8||p[0])return 4; p[0]=17;
  if(mem_realloc(p,1,24)!=p||p[0]!=17||p[23])return 5;
  if(mem_realloc(p,24,8)!=p||mem_pool_high()!=24)return 6;
  mem_pool_free(p,8); if(mem_pool_used()||mem_pool_high()!=24)return 7;
  mem_pool_report_phase();
  mem_pool_set_phase(0);if(!ark_streq(mem_pool_phase(),"unclassified"))return 8;
  mem_pool_set_phase(phase);mem_pool_reset();
  if(mem_pool_used()||mem_pool_high()||mem_pool_generation()!=gen+1||!ark_streq(mem_pool_phase(),"unclassified"))return 9;
  mem_pool_alloc(0);if(mem_pool_used()!=8)return 10;
  puts("phase-ok");return 0;
}
'''

class MemoryPhaseTests(unittest.TestCase):
    def test_phases_and_failure_accounting(self):
        sdk=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'))
        cc=os.environ.get('WASI_CC') or str(sdk/'bin'/('clang.exe' if os.name=='nt' else 'clang'))
        runtime=os.environ.get('WASMTIME') or shutil.which('wasmtime') or r'C:\Program Files\Wasmtime\bin\wasmtime.exe'
        environment=os.environ.copy()
        if os.name=='nt':
            python_dir=Path(sys.executable).parent
            environment['PATH']=os.pathsep.join([str(python_dir),str(python_dir/'Library/bin'),environment.get('PATH','')])
        with tempfile.TemporaryDirectory(prefix='memory-phases-',dir=ROOT/'build') as temp:
            folder=Path(temp);source=folder/'phases.c';output=folder/'phases.wasm'
            source.write_text(SOURCE,encoding='utf-8')
            built=subprocess.run([cc,'--target=wasm32-wasip1','-std=c99','-DARK_POOL_SIZE=64','-I'+str(ROOT/'include'),str(ROOT/'src/core/memory.c'),str(source),'-o',str(output)],env=environment,capture_output=True,text=True,timeout=30)
            self.assertEqual(built.returncode,0,built.stdout+built.stderr)
            def run(*args):
                return subprocess.run([runtime,'run',str(output),*args],env=environment,capture_output=True,text=True,timeout=30)
            result=run();self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('phase-ok',result.stdout)
            self.assertIn('stage=parse used=0 high=24 capacity=64',result.stderr)
            for mode,request,aligned,used in [('fail',9,16,56),('oversized',2147483647,-1,56),('realloc',2147483641,-1,56),('grow',2147483640,2147483640,56),('exact',1,8,64)]:
                with self.subTest(mode=mode):
                    result=run(mode);self.assertEqual(result.returncode,90,result.stdout+result.stderr)
                    self.assertIn(f'memory pool exhausted: phase=parse request={request} aligned={aligned} used={used} high={used} capacity=64',result.stderr)
                    self.assertIn(f'abort=memory pool exhausted used={used} high={used}',result.stderr)

if __name__=='__main__':unittest.main()
