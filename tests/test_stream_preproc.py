"""Standalone preprocessing parity and bounded source/sink tests; no app build."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "preproc.h"
#include "xh_pp_stream.h"
#include "xh_target_rules.h"
#include "config.h"
#include "diag.h"
#include "memory.h"
static unsigned char pool[4*1024*1024];static int used,gen,errors;
void *mem_pool_alloc(int n){void *p;if(n<0||used+n>(int)sizeof(pool))abort();p=pool+used;used=(used+n+7)&~7;return p;}
void *mem_realloc(void *p,int a,int b){void *q=mem_pool_alloc(b);if(p)memcpy(q,p,a<b?a:b);return q;}
char *mem_strdup(const char *p){int n=strlen(p)+1;char *q=mem_pool_alloc(n);memcpy(q,p,n);return q;}
int mem_pool_generation(void){return gen;}
void ark_abort(const char *s){fprintf(stderr,"abort %s\n",s);exit(9);}
void ark_log(const char *fmt,...){(void)fmt;}
void ark_get_date_time(char *d,char *t){strcpy(d,"Nov 25 2026");strcpy(t,"12:00:00");}
int ark_streq(const char*a,const char*b){return strcmp(a,b)==0;}
int ark_strlen(const char*s){return (int)strlen(s);}
void diag_set_source_file(const char*a,const char*b){(void)a;(void)b;}
void diag_clear_source_provider(void){}
void type_pack_reset(void){}
void diag_pragma_suppress(int c){(void)c;} void diag_pragma_default(int c){(void)c;}
void diag_report(int c,DiagSeverity sev,const DiagLoc *l,const char*a,const char*b,const char*d){(void)a;(void)b;(void)d;if(sev>=DIAG_SEV_ERROR){errors++;fprintf(stderr,"diag %d at %s:%d\n",c,l->file?l->file:"?",l->line);}}
void diag_report_msg(DiagSeverity sev,const DiagLoc*l,const char*m,const char*a,const char*b,const char*c){(void)a;(void)b;(void)c;if(sev>=DIAG_SEV_ERROR){errors++;fprintf(stderr,"%s:%d: %s\n",l->file?l->file:"?",l->line,m);}}
void diag_tool(DiagSeverity sev,const char*o,const char*m,const char*a,const char*b,const char*c,const char*d,const char*e,const char*f){(void)sev;(void)o;(void)m;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;errors++;}
const char *diag_num(char *b,int n){sprintf(b,"%d",n);return b;}
void diag_report_no_final_newline(int c,const char*f,int l,const char*s){(void)c;(void)f;(void)l;(void)s;}
ArkccConfig g_config;
const XhTargetDescriptor *xh_target_rules_get(const char *s){static XhTargetDescriptor t;(void)s;strcpy(t.id,"arm");return &t;}
static int calls,max_write,fail;static unsigned bytes;
static int sink(void *ctx,const char *s,int n){FILE*f=ctx;calls++;bytes+=n;if(n>max_write)max_write=n;if(fail)return -1;return fwrite(s,1,n,f)==(size_t)n?0:-1;}
int main(int argc,char **argv){FILE*f;int r=0;if(argc<4)return 2;preproc_reset();preproc_set_main_file(argv[2]);g_config.enable_trigraph=1;
 f=fopen(argv[3],"wb");if(!f)return 3;
 if(!strcmp(argv[1],"legacy")){static char src[32768];FILE*i=fopen(argv[2],"rb");size_t n;if(!i)return 4;n=fread(src,1,sizeof(src)-1,i);src[n]=0;fclose(i);fputs(preproc_process(src),f);}
 else {fail=!strcmp(argv[1],"fail");r=preproc_stream_file(argv[2],0,0,sink,f,0,0);}
 fclose(f);printf("r=%d errors=%d bytes=%u calls=%d max_write=%d pool=%d\n",r,errors,bytes,calls,max_write,used);return r||errors?1:0;}
'''

class StreamPreprocTests(unittest.TestCase):
    def run_suite(self, native):
        sdk = Path(os.environ.get('WASI_SDK_PATH', r'D:\wasi-sdk-34.0-x86_64-windows'))
        cc = (os.environ.get('XH_NATIVE_CLANG') or shutil.which('clang') or r'D:\msys64\ucrt64\bin\clang.exe') if native else str(sdk/'bin/clang.exe')
        runtime = shutil.which('wasmtime') or r'C:\Program Files\Wasmtime\bin\wasmtime.exe'
        env=os.environ.copy();py=Path(sys.executable).parent
        env['PATH']=os.pathsep.join([str(Path(cc).parent),str(py),str(py/'Library/bin'),env.get('PATH','')])
        with tempfile.TemporaryDirectory(prefix='stream-pp-',dir=ROOT/'build') as tmp:
            d=Path(tmp);src=d/'harness.c';binary=d/('pp.exe' if native else 'pp.wasm');src.write_text(HARNESS,encoding='utf8')
            cmd=[cc]+([] if native else ['--target=wasm32-wasip1'])+['-std=c99','-DXHCC','-DPP_INC_BUF=32768','-DPP_BUF_SIZE=32768','-I'+str(ROOT/'include'),str(src),str(ROOT/'src/frontend/c/preproc.c'),str(ROOT/'src/frontend/c/source_reader.c'),str(ROOT/'src/platform/wasi/flash_fs_host.c'),'-o',str(binary)]
            result=subprocess.run(cmd,env=env,capture_output=True,text=True,timeout=60)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            def run(mode,name,success=True):
                out=d/(name+'.'+mode+'.out');args=[mode,name,out.name]
                cmd=([str(binary)] if native else [runtime,'run','--dir=.',str(binary)])+args
                r=subprocess.run(cmd,cwd=d,env=env,capture_output=True,text=True,timeout=60)
                self.assertEqual(r.returncode==0,success,r.stdout+r.stderr)
                return out.read_bytes(),r.stdout
            fixtures=[b'#define A 7\n#if A == 7\nint a=A;\n#else\nwrong\n#endif\n',
                      b'#define CAT(a,b) a##b\n#define S(x) #x\n#define F(x,y) ((x)+(y))\nint CAT(ab,c)=F(1,\n2);\nchar *s=S(hello world);\n',
                      b'/*abc*/int a; // hi\n/*a\nb*/int b;\n',
                      b'#define A(x) ((x)+1)\n#define B A\nint a=B(2);\n',
                      b'#define A 1 ??/\n + 2\nint b=A;\n',
                      b'#pragma pack(push,1)\nstruct S {char a;int b;};\n#pragma pack(pop)\n']
            for i,text in enumerate(fixtures):
                name=f'small{i}.c';(d/name).write_bytes(text)
                old,_=run('legacy',name);new,_=run('stream',name)
                self.assertEqual(new,old,f'fixture {i}')
            asm=b'__asm int entry(void){\nB .Ldest\nNOP\n.Ldest:\nMOV r0, #17\nBX lr\n}\n'
            for prefix in (b'',b' '*1023+b'\n',b'/*'+b'x'*8193+b'*/\n'):
                (d/'asm.c').write_bytes(prefix+asm)
                old,_=run('legacy','asm.c');new,_=run('stream','asm.c');self.assertEqual(new,old)
                self.assertIn(b'MOV r0, #17',new)
            (d/'hash.c').write_bytes(b'int a #17;\nint b; /*comment*/ #19\n')
            out,_=run('stream','hash.c');self.assertIn(b'int a #17;',out);self.assertIn(b'#19',out)
            (d/'huge.h').write_bytes(b'/*'+b'x'*(1024*1024+31)+b'*/\n#define TAIL 73\nint header_tail=TAIL;\n')
            (d/'large.c').write_bytes(b'/*'+b'x'*(1024*1024+17)+b'*/\n#include "huge.h"\nint main_tail=TAIL;\n'+b'int repeated;\n'*5000)
            out,stats=run('stream','large.c')
            self.assertIn(b'int header_tail=73;',out);self.assertIn(b'int main_tail=73;',out)
            self.assertGreater(len(out),2*1024*1024);self.assertEqual(out.count(b'int repeated;'),5000)
            for edge in range(1018,1027):
                name='edge.c';(d/name).write_bytes(b' ' * edge+b'/*x*/\n#define M(x) ((x)+2)\nint a=M(\n4);\n')
                old,_=run('legacy',name);new,_=run('stream',name);self.assertEqual(new,old)
            (d/'bad.c').write_bytes(b'int a;\x00int b;\n');run('stream','bad.c',False)
            (d/'cap.c').write_bytes(b'#define LONG '+b'x'*9000+b'\n');run('stream','cap.c',False)
            run('fail','small0.c',False)
            (d/'comments.c').write_bytes(b'#define A(x) ((x)+1)\nint x=A(2 /* , ) */);\nint a=__LINE__;\\\nint b=__LINE__;\n')
            out,_=run('stream','comments.c');self.assertIn(b'int x=((2  )+1);',out);self.assertIn(b'int b=4;',out)
            (d/'argcap.c').write_bytes(b'#define A(x) x\nA('+b'x'*1100+b')\n');run('stream','argcap.c')
            (d/'argcap.c').write_bytes(b'#define A(x) x\nA('+b'x'*8200+b')\n');run('stream','argcap.c',False)
            for edge in range(8185,8198):
                (d/'commentedge.c').write_bytes(b'/*'+b'x'*edge+b'*/int tail=7;\n')
                old,_=run('legacy','commentedge.c');new,_=run('stream','commentedge.c');self.assertEqual(new,old)
            (d/'whitespace.c').write_bytes(b' '*(32*1024)+b'\nint large_tail=17;\n')
            out,_=run('stream','whitespace.c');self.assertEqual(out,b' '*(32*1024)+b'\nint large_tail=17;\n')
            (d/'indent.c').write_bytes(b' \t'*16000+b'#define V 19\nint tail=V;\n')
            out,_=run('stream','indent.c');self.assertEqual(out,b'\nint tail=19;\n')
            for indent in (8189,8190,8191,16000):
                (d/'indent.c').write_bytes(b' '*indent+b'#define V 19\nint tail=V;\n')
                old,_=run('legacy','indent.c');new,_=run('stream','indent.c');self.assertEqual(new,old)
            print(('native ' if native else 'WASI ')+stats.strip())
    def test_native(self):self.run_suite(True)
    def test_wasi(self):self.run_suite(False)

if __name__=='__main__':unittest.main()
