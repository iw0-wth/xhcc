"""Exercise bounded shared template parser in an independent Wasm harness."""
from pathlib import Path
import os,subprocess,sys,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
SDK=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'))
WASM=os.environ.get('WASMTIME',r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
class Templates(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='template-');cls.dir=Path(cls.tmp.name);cls.rel=cls.dir.relative_to(ROOT).as_posix()
  c=cls.dir/'h.c';c.write_text(r'''
#include "xh_template.h"
#include <stdio.h>
#include <stdarg.h>
int fs_file_size(const char*p){FILE*f=fopen(p,"rb");int n;if(!f)return -1;fseek(f,0,SEEK_END);n=(int)ftell(f);fclose(f);return n;}
int fs_read_file(const char*p,char*b,int cap){FILE*f=fopen(p,"rb");int n;if(!f)return -1;n=(int)fread(b,1,cap-1,f);b[n]=0;fclose(f);return n;}
void diag_tool(int sev,const char*who,const char*msg,const char*a0,const char*a1,const char*a2,const char*a3,const char*a4,const char*a5){(void)sev;(void)a0;(void)a1;(void)a2;(void)a3;(void)a4;(void)a5;fprintf(stderr,"%s: %s\n",who,msg);}
int main(int argc,char**argv){char out[128];const char*args[]={"value"};int n;if(argc!=2)return 2;if(xh_template_set_path("test",argv[1]))return 2;n=xh_template_emit("test","hello",args,1,out,sizeof(out));if(n<0)return 1;fwrite(out,1,n,stdout);xh_template_release();return 0;}
''')
  env=os.environ.copy();pd=Path(sys.executable).parent;env['PATH']=str(pd)+os.pathsep+str(pd/'Library/bin')+os.pathsep+env.get('PATH','')
  subprocess.run([str(SDK/'bin/clang.exe'),'-Oz','-I'+str(ROOT/'include'),str(c),str(ROOT/'src/backend/rules/template.c'),'-o',str(cls.dir/'h.wasm')],env=env,check=True)
 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup()
 def invoke(self,text,ok):
  (self.dir/'rules').write_text(text)
  r=subprocess.run([WASM,'run','--dir=.',str(self.dir/'h.wasm'),self.rel+'/rules'],cwd=ROOT,capture_output=True,text=True)
  self.assertEqual(r.returncode==0,ok,r.stdout+r.stderr);return r
 def test_expansion_and_fragments(self):
  r=self.invoke('XHTEMPLATE 1\nrule hello\ntext hi ${0} $$ \\x41\\t\ntext end\\n\nend\n',True)
  self.assertEqual(r.stdout,'hi value $ A\tend\n')
 def test_bad_tables(self):
  for body in ['XHTEMPLATE 2\n','XHTEMPLATE 1\nrule hello\ntext ${16}\nend\n','XHTEMPLATE 1\nrule hello\ntext \\q\nend\n','XHTEMPLATE 1\nrule hello\ntext ${1}\nend\n','XHTEMPLATE 1\nrule hello\nend\nrule hello\nend\n','XHTEMPLATE 1\nrule hello\ntext '+('x'*200)+'\nend\n']:
   with self.subTest(body=body[:80]):self.invoke(body,False)
if __name__=='__main__':unittest.main()
