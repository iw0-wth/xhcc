"""Real WASI filesystem replacement with injected stdio/rename failures."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]

class AtomicOutputTests(unittest.TestCase):
 def test_success_and_injected_failures_preserve_destination(self):
  sdk=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'))
  cc=os.environ.get('WASI_CC') or str(sdk/'bin/clang.exe' if os.name=='nt' else sdk/'bin/clang')
  runtime=os.environ.get('WASMTIME') or shutil.which('wasmtime') or r'C:\Program Files\Wasmtime\bin\wasmtime.exe'
  env=os.environ.copy();pd=Path(sys.executable).parent
  if os.name=='nt':env['PATH']=os.pathsep.join([str(pd),str(pd/'Library/bin'),env.get('PATH','')])
  with tempfile.TemporaryDirectory(prefix='atomic-output-',dir=ROOT/'build') as tmp:
   directory=Path(tmp);source=directory/'atomic.c';binary=directory/'atomic.wasm'
   source.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
static int mode, writes;
static size_t injected_write(const void *p,size_t size,size_t count,FILE *f) {
 if(mode==1) { if(writes++) {errno=ENOSPC;return 0;} return fwrite(p,size,count>2?2:count,f); }
 if(mode==5) return fwrite(p,size,count>2?2:count,f);
 return fwrite(p,size,count,f);
}
static int injected_flush(FILE *f) {if(mode==2){errno=EIO;return EOF;}return fflush(f);}
static int injected_close(FILE *f) {int result=fclose(f);if(mode==3){errno=EIO;return EOF;}return result;}
static int injected_rename(const char *a,const char *b) {if(mode==4){errno=EACCES;return -1;}return rename(a,b);}
#define fwrite injected_write
#define fflush injected_flush
#define fclose injected_close
#define rename injected_rename
#include "flash_fs_host.c"
#undef fwrite
#undef fflush
#undef fclose
#undef rename
void ark_log(const char *format,...) {(void)format;}
int main(int argc,char **argv) {
 const unsigned char text[]="complete-new-output";
 int result;
 if(argc!=2)return 91;
 mode=atoi(argv[1]);
 result=fs_write_file("output.bin",text,mode==6?0:(int)sizeof(text)-1);
 if(mode>=1&&mode<=4) {if(result!=-1)return 92;printf("expected failure: %s\n",fs_last_error());}
 else if(result!=(mode==6?0:(int)sizeof(text)-1))return 93;
 return 0;
}
''',encoding='utf-8')
   built=subprocess.run([cc,'--target=wasm32-wasip1','-std=c99','-D_POSIX_C_SOURCE=200809L',
    '-I'+str(ROOT/'include'),'-I'+str(ROOT/'src/platform/wasi'),str(source),'-o',str(binary)],
    env=env,capture_output=True,text=True,timeout=30)
   self.assertEqual(built.returncode,0,built.stdout+built.stderr)
   for mode in (1,2,3,4,0,5,6):
    with self.subTest(mode=mode):
     output=directory/'output.bin';output.write_bytes(b'old-valid-output')
     result=subprocess.run([runtime,'run','--dir=.',str(binary),str(mode)],cwd=directory,capture_output=True,text=True,timeout=30)
     self.assertEqual(result.returncode,0,result.stdout+result.stderr)
     self.assertEqual(output.read_bytes(),b'old-valid-output' if mode in (1,2,3,4) else b'' if mode==6 else b'complete-new-output')
     self.assertEqual(list(directory.glob('*.xhcc-tmp-*')),[])

if __name__=='__main__':unittest.main()
