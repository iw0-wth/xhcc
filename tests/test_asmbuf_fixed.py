"""Exercise actual AsmBuffer constructors, exact capacity and guarded failure."""
from pathlib import Path
import os,sys,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
SDK=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'))/'bin'
WASM=os.environ.get('WASMTIME',r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
HARNESS=r'''
#include "codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int allocations;
void *mem_pool_alloc(int n){allocations++;return calloc(1,n);}
void *mem_realloc(void*p,int old,int n){allocations++;return realloc(p,n);}
int ark_strlen(const char*s){return (int)strlen(s);}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"failed line %d\n",__LINE__);return 1;}}while(0)
int main(void){AsmBuffer b,d;unsigned char guard[20];int cap;
 for(cap=0;cap<=8;cap++){
  memset(guard,0xa5,sizeof guard);asmbuf_init_fixed(&b,(char*)guard+4,cap);
  if(!cap){CHECK(asmbuf_failed(&b));asmbuf_puts(&b,"abc");CHECK(guard[4]==0xa5);}
  else{char text[9];memset(text,'x',cap-1);text[cap-1]=0;CHECK(!asmbuf_require(&b,cap-1));asmbuf_puts(&b,text);CHECK(b.len==cap-1&&!asmbuf_failed(&b));asmbuf_putc(&b,'!');CHECK(asmbuf_failed(&b)&&b.len==cap-1&&b.buf[b.len]==0);asmbuf_puts(&b,"later");CHECK(b.len==cap-1);}
  CHECK(guard[3]==0xa5&&guard[4+cap]==0xa5);
 }
 asmbuf_init_fixed(&b,0,8);CHECK(asmbuf_failed(&b));asmbuf_putc(&b,'!');
 memset(guard,0xa5,sizeof guard);asmbuf_init_fixed(&b,(char*)guard+4,8);CHECK(asmbuf_require(&b,-1)<0);CHECK(guard[12]==0xa5);
 asmbuf_init_fixed(&b,(char*)guard+4,8);CHECK(asmbuf_require(&b,2147483647)<0);CHECK(guard[12]==0xa5);
 CHECK(allocations==0);
 asmbuf_init(&d);asmbuf_puts(&d,"ab");asmbuf_int(&d,-123);CHECK(!strcmp(d.buf,"ab-123"));CHECK(!asmbuf_require(&d,9000));CHECK(d.cap>=9001&&!asmbuf_failed(&d)&&allocations==2);puts("PASS fixed canaries exact fit sticky failure no allocations and dynamic growth");return 0;}
'''
class FixedAsmBuffer(unittest.TestCase):
 def test_production_buffer(self):
  with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='asmbuf-fixed-') as tmp:
   out=Path(tmp);source=out/'h.c';source.write_text(HARNESS);env=os.environ.copy();pd=Path(sys.executable).parent;env['PATH']=os.pathsep.join([str(pd),str(pd/'Library/bin'),env.get('PATH','')]);vm=out/'h.wasm'
   p=subprocess.run([str(SDK/'clang.exe'),'--target=wasm32-wasip1','-O2','-Iinclude','-include','include/xh_small.h',str(source),'src/core/codegen_common.c','-o',str(vm)],cwd=ROOT,env=env,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr)
   p=subprocess.run([WASM,'run',str(vm)],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr);self.assertIn('PASS',p.stdout)
if __name__=='__main__':unittest.main()
