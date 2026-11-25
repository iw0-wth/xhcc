"""Bounded file-range reads over real large files and injected stdio failures."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]

class FileRangeTests(unittest.TestCase):
 def test_large_file_small_ranges_and_failures(self):
  sdk=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'))
  cc=os.environ.get('WASI_CC') or str(sdk/('bin/clang.exe' if os.name=='nt' else 'bin/clang'))
  runtime=os.environ.get('WASMTIME') or shutil.which('wasmtime') or r'C:\Program Files\Wasmtime\bin\wasmtime.exe'
  env=os.environ.copy();pd=Path(sys.executable).parent
  if os.name=='nt':env['PATH']=os.pathsep.join([str(pd),str(pd/'Library/bin'),env.get('PATH','')])
  with tempfile.TemporaryDirectory(prefix='fs-range-',dir=ROOT/'build') as tmp:
   directory=Path(tmp);source=directory/'range.c';binary=directory/'range.wasm'
   block=bytes(range(251));size=600*1024+37
   (directory/'large.bin').write_bytes((block*(size//len(block)+1))[:size])
   source.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <limits.h>
static int fault, closes;
static int injected_seek(FILE *f,long off,int origin) {if(fault==1){errno=EIO;return -1;}return fseek(f,off,origin);}
static size_t injected_read(void *p,size_t s,size_t n,FILE *f) {if(fault==2){errno=EIO;return 0;}return fread(p,s,n,f);}
static int injected_error(FILE *f) {return fault==2?1:ferror(f);}
static int injected_close(FILE *f) {int rc=fclose(f);closes++;if(fault==3){errno=EIO;return EOF;}return rc;}
#define fseek injected_seek
#define fread injected_read
#define ferror injected_error
#define fclose injected_close
#include "flash_fs_host.c"
#undef fseek
#undef fread
#undef ferror
#undef fclose
void ark_log(const char *format,...) {(void)format;}
static int unchanged(unsigned char *p,unsigned start,unsigned end) {unsigned i;for(i=start;i<end;i++)if(p[i]!=0xa5)return 0;return 1;}
int main(void) {
 unsigned char storage[40];unsigned i;int n;uint32_t size=600u*1024u+37u,offset=550u*1024u+13u;
 memset(storage,0xa5,sizeof(storage));n=fs_read_range("large.bin",offset,storage+4,17);
 if(n!=17||!unchanged(storage,0,4)||!unchanged(storage,21,40))return 1;
 for(i=0;i<17;i++)if(storage[4+i]!=(unsigned char)((offset+i)%251))return 2;
 memset(storage,0xa5,sizeof(storage));n=fs_read_range("large.bin",size-3,storage+4,17);
 if(n!=3||!unchanged(storage,0,4)||!unchanged(storage,7,40))return 3;
 memset(storage,0xa5,sizeof(storage));if(fs_read_range("large.bin",size,storage+4,17)!=0||!unchanged(storage,0,40))return 4;
 if(fs_read_range("large.bin",0,0,0)!=0)return 5;
 if(fs_read_range("missing.bin",0,storage,1)!=-1||!fs_last_error()[0])return 6;
 if(fs_read_range(0,0,storage,1)!=-1||fs_read_range("",0,storage,1)!=-1||fs_read_range("large.bin",0,0,1)!=-1)return 7;
 if(fs_read_range("large.bin",UINT32_MAX,storage,1)!=-1||fs_read_range("large.bin",0,storage,UINT32_MAX)!=-1||fs_read_range("large.bin",INT_MAX,storage,1)!=-1)return 8;
 for(fault=1;fault<=3;fault++) {int old=closes;if(fs_read_range("large.bin",0,storage,1)!=-1||closes!=old+1||!fs_last_error()[0])return 9;}
 puts("range reads and error cleanup OK");return 0;
}
''',encoding='utf-8')
   built=subprocess.run([cc,'--target=wasm32-wasip1','-std=c99','-D_POSIX_C_SOURCE=200809L','-I'+str(ROOT/'include'),'-I'+str(ROOT/'src/platform/wasi'),str(source),'-o',str(binary)],env=env,capture_output=True,text=True,timeout=30)
   self.assertEqual(built.returncode,0,built.stdout+built.stderr)
   result=subprocess.run([runtime,'run','--dir=.',str(binary)],cwd=directory,capture_output=True,text=True,timeout=30)
   self.assertEqual(result.returncode,0,result.stdout+result.stderr)
   self.assertIn('range reads and error cleanup OK',result.stdout)

if __name__=='__main__':unittest.main()
