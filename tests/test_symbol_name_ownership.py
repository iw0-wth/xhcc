"""Owned AST symbol names borrow storage; generic symbols retain copy semantics."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
SOURCE=r'''
#include "symtab.h"
#include "memory.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void ark_log(const char *fmt,...){(void)fmt;}
void ark_abort(const char *s){fprintf(stderr,"%s\n",s);exit(90);}
int main(void){char temporary[32]="temporary_name";char *owned;Symbol *copied,*borrowed,*same;int start,copied_cost,borrowed_cost;
 mem_pool_reset();sym_init();copied=sym_add(SYM_VAR,temporary,0,7);
 if(copied->name==temporary)return 1;
 temporary[0]='X';if(!ark_streq(copied->name,"temporary_name")||sym_find("temporary_name")!=copied)return 2;
 owned=mem_strdup("arena_owned_name");start=mem_pool_used();
 same=sym_add(SYM_VAR,owned,0,9);copied_cost=mem_pool_used()-start;
 start=mem_pool_used();borrowed=sym_add_borrowed(SYM_VAR,owned,0,9);borrowed_cost=mem_pool_used()-start;
 if(borrowed->name!=owned||same->name==owned||sym_find(owned)!=borrowed)return 3;
 if(copied_cost-borrowed_cost!=((ark_strlen(owned)+1+7)&~7))return 4;
 /* The common initializer must preserve every field besides name/link. */
 {Symbol a=*same,b=*borrowed;a.name=b.name;a.next=b.next;
 if(memcmp(&a,&b,sizeof(Symbol)))return 5;}
 sym_enter_scope();start=mem_pool_used();sym_add_borrowed(SYM_TYPEDEF,owned,0,0);
 sym_leave_scope();if(mem_pool_used()<=start||sym_find(owned)!=borrowed||!ark_streq(owned,"arena_owned_name"))return 6;
 sym_init();if(sym_find(owned)||!ark_streq(owned,"arena_owned_name"))return 7;
 mem_pool_reset();sym_init();owned=mem_strdup("next_tu");borrowed=sym_add_borrowed(SYM_VAR,owned,0,0);
 if(sym_find("arena_owned_name")||sym_find("next_tu")!=borrowed||borrowed->name!=owned)return 8;
 printf("symbol-name-ok saved=%d\n",copied_cost-borrowed_cost);return 0;
}
'''
class SymbolNameOwnershipTests(unittest.TestCase):
 def test_copy_and_borrow_lifetimes(self):
  sdk=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'))
  cc=os.environ.get('WASI_CC') or str(sdk/'bin'/('clang.exe' if os.name=='nt' else 'clang'))
  runtime=os.environ.get('WASMTIME') or shutil.which('wasmtime') or r'C:\Program Files\Wasmtime\bin\wasmtime.exe'
  env=os.environ.copy()
  if os.name=='nt':
   pd=Path(sys.executable).parent;env['PATH']=os.pathsep.join([str(pd),str(pd/'Library/bin'),env.get('PATH','')])
  with tempfile.TemporaryDirectory(prefix='symbol-names-',dir=ROOT/'build') as temp:
   folder=Path(temp);source=folder/'names.c';output=folder/'names.wasm';source.write_text(SOURCE,encoding='utf-8')
   built=subprocess.run([cc,'--target=wasm32-wasip1','-std=c99','-DARK_POOL_SIZE=4096','-DARK_LINK_BUF_SIZE=128','-include',str(ROOT/'include/xh_small.h'),'-I'+str(ROOT/'include'),str(ROOT/'src/core/memory.c'),str(ROOT/'src/frontend/c/symtab.c'),str(source),'-o',str(output)],env=env,capture_output=True,text=True,timeout=30)
   self.assertEqual(built.returncode,0,built.stdout+built.stderr)
   run=subprocess.run([runtime,'run',str(output)],env=env,capture_output=True,text=True,timeout=30);self.assertEqual(run.returncode,0,run.stdout+run.stderr);self.assertIn('symbol-name-ok saved=24',run.stdout)
if __name__=='__main__':unittest.main()
