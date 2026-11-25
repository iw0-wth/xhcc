"""MIR reachability releases its private queue without changing graph results."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
SOURCE=r'''
#include "mir.h"
#include "memory.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
void ark_log(const char *fmt,...) {(void)fmt;}
void ark_abort(const char *s){fprintf(stderr,"%s\n",s);exit(90);}
static MirBlock blocks[100];
static unsigned char reach[100];
int main(void){MirFunc m={0};int i,j,n;unsigned char *sentinel;
  m.blk=blocks;m.blk_count=100;
  for(i=0;i<100;i++){blocks[i].succ_count=i<99?1:0;blocks[i].succ[0]=i+1;}
  mem_pool_reset();
  for(j=0;j<12;j++){
    n=mir_reachable(&m,reach);if(n!=100)return 1;
    for(i=0;i<100;i++)if(!reach[i])return 2;
    if(mem_pool_used()!=0)return 3;
  }
  if(mem_pool_high()!=400)return 4;
  sentinel=mem_pool_alloc(8);sentinel[0]=42;
  /* A cycle and a branch which reconverges must enqueue each block once. */
  blocks[0].succ_count=2;blocks[0].succ[0]=1;blocks[0].succ[1]=2;
  blocks[99].succ_count=1;blocks[99].succ[0]=0;
  if(mir_reachable(&m,reach)!=100||mem_pool_used()!=8||sentinel[0]!=42)return 5;
  /* Disconnected suffix, singleton and empty graph. */
  blocks[49].succ_count=0;
  if(mir_reachable(&m,reach)!=50||reach[50]||mem_pool_used()!=8)return 6;
  m.blk_count=1;blocks[0].succ_count=0;
  if(mir_reachable(&m,reach)!=1||!reach[0]||mem_pool_used()!=8)return 7;
  m.blk_count=0;if(mir_reachable(&m,reach)!=0||mem_pool_used()!=8)return 8;
  puts("mir-queue-ok runs=12 blocks=100 used=0 high=400");return 0;
}
'''
class MirMemoryTests(unittest.TestCase):
    def test_queue_reclaimed_across_graph_walks(self):
        sdk=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'))
        cc=os.environ.get('WASI_CC') or str(sdk/'bin'/('clang.exe' if os.name=='nt' else 'clang'))
        runtime=os.environ.get('WASMTIME') or shutil.which('wasmtime') or r'C:\Program Files\Wasmtime\bin\wasmtime.exe'
        environment=os.environ.copy()
        if os.name=='nt':
            pd=Path(sys.executable).parent
            environment['PATH']=os.pathsep.join([str(pd),str(pd/'Library/bin'),environment.get('PATH','')])
        with tempfile.TemporaryDirectory(prefix='mir-memory-',dir=ROOT/'build') as temp:
            directory=Path(temp);source=directory/'mir.c';output=directory/'mir.wasm'
            source.write_text(SOURCE,encoding='utf-8')
            built=subprocess.run([cc,'--target=wasm32-wasip1','-std=c99','-ffunction-sections','-Wl,--gc-sections','-DARK_POOL_SIZE=512','-DARK_LINK_BUF_SIZE=128','-include',str(ROOT/'include/xh_small.h'),'-I'+str(ROOT/'include'),str(ROOT/'src/core/memory.c'),str(ROOT/'src/ir/mir.c'),str(source),'-o',str(output)],env=environment,capture_output=True,text=True,timeout=30)
            self.assertEqual(built.returncode,0,built.stdout+built.stderr)
            result=subprocess.run([runtime,'run',str(output)],env=environment,capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('mir-queue-ok',result.stdout)
if __name__=='__main__':unittest.main()
