"""Output publication policies against a simulated destructive rename.

The harness tests the single-writer contract, not concurrent no-replace or a
real NuttX/FAT device. Default atomic policy still depends on the host FS.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]

class OutputPolicyTests(unittest.TestCase):
 def test_both_publication_apis_and_destructive_rename(self):
  sdk=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'))
  cc=os.environ.get('WASI_CC') or str(sdk/('bin/clang.exe' if os.name=='nt' else 'bin/clang'))
  runtime=os.environ.get('WASMTIME') or shutil.which('wasmtime') or r'C:\Program Files\Wasmtime\bin\wasmtime.exe'
  env=os.environ.copy();pd=Path(sys.executable).parent
  if os.name=='nt':env['PATH']=os.pathsep.join([str(pd),str(pd/'Library/bin'),env.get('PATH','')])
  with tempfile.TemporaryDirectory(prefix='output-policy-',dir=ROOT/'build') as tmp:
   directory=Path(tmp);source=directory/'policy.c';binary=directory/'policy.wasm'
   source.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
static int rename_calls, destructive;
static int simulated_rename(const char *a,const char *b) {
 rename_calls++;
 if(destructive){remove(b);errno=EIO;return -1;}
 return rename(a,b);
}
#define rename simulated_rename
#include "flash_fs_host.c"
#undef rename
void ark_log(const char *format,...) {(void)format;}
int main(int argc,char **argv) {
 const unsigned char text[]="new-complete-output";
 int rc,expected_calls,expected_ok;
 if(argc!=5)return 90;
 destructive=atoi(argv[2]);expected_calls=atoi(argv[3]);expected_ok=atoi(argv[4]);
 if(!strcmp(argv[1],"spool")) {
  FsSpool spool;
  if(fs_spool_create(&spool,0,"output.bin")||fs_spool_append(&spool,text,sizeof(text)-1)||fs_spool_finish(&spool))return 91;
  rc=fs_spool_publish(&spool,"output.bin");
  fs_spool_destroy(&spool);
 } else rc=fs_write_file("output.bin",text,sizeof(text)-1)<0?-1:0;
 if((rc==0)!=expected_ok||rename_calls!=expected_calls){fprintf(stderr,"rc=%d renamecalls=%d expected=%d/%d error=%s\n",rc,rename_calls,expected_ok,expected_calls,fs_last_error());return 92;}
 if(rc && !fs_last_error()[0])return 93;
 return 0;
}
''',encoding='utf-8')
   built=subprocess.run([cc,'--target=wasm32-wasip1','-std=c99','-D_POSIX_C_SOURCE=200809L',
    '-I'+str(ROOT/'include'),'-I'+str(ROOT/'src/platform/wasi'),str(source),'-o',str(binary)],
    env=env,capture_output=True,text=True,timeout=30)
   self.assertEqual(built.returncode,0,built.stdout+built.stderr)
   # policy, prior target, destructive rename, expected calls, expected success.
   cases=[('no-replace',True,True,0,False),('no-replace',False,False,1,True),
          ('invalid',True,True,0,False),('',True,True,0,False),
          ('atomic-replace',True,False,1,True),(None,True,False,1,True)]
   for api in ('write','spool'):
    for policy,exists,destructive,calls,ok in cases:
     with self.subTest(api=api,policy=policy,exists=exists):
      output=directory/'output.bin';output.unlink(missing_ok=True)
      if exists:output.write_bytes(b'old-valid-output')
      command=[runtime,'run','--dir=.']
      if policy is not None:command+=['--env','XHCC_OUTPUT_POLICY='+policy]
      command += [str(binary),api,str(int(destructive)),str(calls),str(int(ok))]
      result=subprocess.run(command,cwd=directory,capture_output=True,text=True,timeout=30)
      self.assertEqual(result.returncode,0,result.stdout+result.stderr)
      self.assertEqual(output.read_bytes(),b'new-complete-output' if ok else b'old-valid-output')
      self.assertEqual(list(directory.glob('*.xhcc-tmp-*')),[])

if __name__=='__main__':unittest.main()
