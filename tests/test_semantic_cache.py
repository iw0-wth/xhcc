"""Same-process semantic/template cache switching and generation invalidation."""
from pathlib import Path
import os,sys,subprocess,unittest
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/semantic-cache-tests';SDK=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'));WASM=os.environ.get('WASMTIME',r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
HARNESS=r'''
#include "xh_semantic.h"
#include "xh_template.h"
#include <stdio.h>
#include <string.h>
int fs_file_size(const char*p){FILE*f=fopen(p,"rb");long n;if(!f)return -1;fseek(f,0,SEEK_END);n=ftell(f);fclose(f);return(int)n;}
int fs_read_file(const char*p,char*b,int c){FILE*f=fopen(p,"rb");int n;if(!f)return -1;n=fread(b,1,c-1,f);b[n]=0;fclose(f);return n;}
void diag_tool(int s,const char*w,const char*m,const char*a,const char*b,const char*c,const char*d,const char*e,const char*f){fprintf(stderr,"%s: %s\n",w,m);}
static int put(const char*path,const char*text){FILE*f=fopen(path,"wb");if(!f)return -1;fputs(text,f);fclose(f);return 0;}
int main(void){XhSemanticInput a={"unary","neg","v","stack",32},b={"unary","neg","v","-",32};XhSemanticResult first,other,again,reloaded;unsigned generation;
 const char*pa="build/semantic-cache-tests/a.rules",*pb="build/semantic-cache-tests/b.rules";
 const char*original="XHTEMPLATE 1\nrule selectors\ntext XHSEMANTIC 1\\nunary neg 32 v stack a.neg 1 1\\n\nend\nrule a.neg\ntext A\nend\n";
 const char*changed="XHTEMPLATE 1\nrule selectors\ntext XHSEMANTIC 1\\nunary neg 32 v stack a.not 1 1\\n\nend\nrule a.not\ntext NEW\nend\n";
 const char*second="XHTEMPLATE 1\nrule selectors\ntext CSELECT1\\nunary neg 32 v b.neg 1 1\\n\nend\nrule b.neg\ntext B\nend\n";
 if(put(pa,original)||put(pb,second)||xh_template_set_path("a",pa)||xh_template_set_path("b",pb))return 1;
 if(xh_semantic_select("a","selectors",&a,&first)!=1||strcmp(first.template_key,"a.neg"))return 2;
 generation=xh_template_generation();
 if(xh_semantic_select("a","selectors",&a,&again)!=1||xh_template_generation()!=generation)return 3;
 if(xh_semantic_select("b","selectors",&b,&other)!=1||strcmp(other.template_key,"b.neg")||!strcmp(first.template_key,other.template_key)||strcmp(first.template_key,"a.neg"))return 4;
 if(xh_semantic_select("a","selectors",&a,&again)!=1||strcmp(again.template_key,"a.neg"))return 5;
 /* Changing the file at the same path requires explicit template release.
  * A plain same-path registration keeps the documented active cache. */
 if(put(pa,changed))return 6;generation=xh_template_generation();xh_template_release();
 if(xh_template_generation()==generation)return 7;
 if(xh_semantic_select("a","selectors",&a,&reloaded)!=1||strcmp(reloaded.template_key,"a.not"))return 8;
 if(strcmp(first.template_key,"a.neg")||strcmp(other.template_key,"b.neg")||strcmp(again.template_key,"a.neg"))return 9;
 xh_semantic_release();xh_template_release();puts("PASS same-process a/b/a, stable generation, same-path reload, copied result lifetime");return 0;}
'''
class SemanticCache(unittest.TestCase):
 def test_switch_reload_and_copied_results(self):
  OUT.mkdir(parents=True,exist_ok=True);(OUT/'harness.c').write_text(HARNESS);env=os.environ.copy();pd=Path(sys.executable).parent;env['PATH']=str(pd)+os.pathsep+str(pd/'Library/bin')+os.pathsep+env.get('PATH','')
  p=subprocess.run([str(SDK/'bin/clang.exe'),'--target=wasm32-wasip1','-O2','-Iinclude','-include','include/xh_small.h','build/semantic-cache-tests/harness.c','src/backend/rules/template.c','src/backend/rules/semantic.c','-o','build/semantic-cache-tests/cache.wasm'],cwd=ROOT,env=env,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr)
  p=subprocess.run([WASM,'run','--dir=.','build/semantic-cache-tests/cache.wasm'],cwd=ROOT,env=env,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stdout+p.stderr);self.assertIn('PASS same-process',p.stdout)
if __name__=='__main__':unittest.main()
