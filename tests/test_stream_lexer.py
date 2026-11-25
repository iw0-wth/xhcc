"""Independent native/WASI lexer harness; never builds the compiler app."""
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
#include "lexer.h"
#include "config.h"
#include "diag.h"
#define CHECK(x) do { if(!(x)){printf("failed line %d: %s\n",__LINE__,#x);return 1;} } while(0)
ArkccConfig g_config;
static unsigned char arena[200000];static unsigned used,errors,calls,max_request,pack;
void *mem_pool_alloc(int n){void *p=arena+used;if(n<0||used+(unsigned)n>sizeof(arena))abort();used+=(unsigned)n;return p;}
char *mem_strndup(const char *s,int n){char *p=mem_pool_alloc(n+1);memcpy(p,s,n);p[n]=0;return p;}
void diag_report_msg(DiagSeverity s,const DiagLoc*l,const char*m,const char*a,const char*b,const char*c){(void)s;(void)l;(void)a;(void)b;(void)c;errors++;printf("diagnostic: %s\n",m);}
void diag_report(int code,DiagSeverity s,const DiagLoc*l,const char*a,const char*b,const char*c){(void)code;diag_report_msg(s,l,"invalid token",a,b,c);}
void diag_report_at_eof(int code,DiagSeverity s,const DiagLoc*l){diag_report(code,s,l,0,0,0);}
int type_pragma_pack(const char *p){if(strstr(p,"(2)"))pack=2;return 1;}
int fs_file_size(const char*p){(void)p;return -1;}
int fs_read_range(const char*p,unsigned off,void*b,unsigned n){(void)p;(void)off;(void)b;(void)n;return -1;}
static int range(void*ctx,uint32_t off,unsigned char*b,uint32_t n){calls++;if(n>max_request)max_request=n;memcpy(b,(char*)ctx+off,n);return (int)n;}
static int fault(void*ctx,uint32_t off,unsigned char*b,uint32_t n){(void)ctx;(void)off;(void)b;(void)n;return -1;}
static int virtual_range(void*ctx,uint32_t off,unsigned char*b,uint32_t n){uint32_t i,size=*(uint32_t*)ctx;calls++;if(n>max_request)max_request=n;for(i=0;i<n;i++){uint32_t p=off+i;b[i]=p==0?'/':p==1?'*':p==size-2?'*':p==size-1?'/':p%1024==1023?'\r':p%1024==0?'\n':'x';}return (int)n;}
static int suite(uint32_t cap){
    char text[12000];unsigned char window[1024];XhSourceReader r;Lexer lex;Token t,saved;uint32_t i,size;
    const char *code="name_123 18446744073709551615ULL 01.5f 0x1.fffffffffffffp+0 '\\x41' \"hello\\u4e2d\" /*cross*/ \"world\" >>= ... ++ != L\"wide\" ";
    memset(text,' ',sizeof(text));memcpy(text+1020,code,strlen(code));size=1020+(uint32_t)strlen(code);
    g_config.enable_wchar=1;used=0;errors=0;calls=0;max_request=0;
    CHECK(xh_source_init(range,text,size,window,cap,&r)==1);CHECK(lexer_init_source(&lex,&r)==0);
    saved=lexer_next(&lex);CHECK(saved.kind==TOK_IDENT&&!strcmp(saved.str,"name_123")&&saved.src_offset==1020&&saved.col==1021);
    t=lexer_next(&lex);CHECK(t.kind==TOK_NUM&&(unsigned long long)t.value==~0ULL&&t.lit_unsigned&&t.lit_llong);
    t=lexer_next(&lex);CHECK(t.kind==TOK_FCONST&&t.fvalue==1.5&&t.is_float);
    t=lexer_next(&lex);CHECK(t.kind==TOK_FCONST&&t.fvalue==0x1.fffffffffffffp+0);
    t=lexer_next(&lex);CHECK(t.kind==TOK_NUM&&t.value==65);
    t=lexer_next(&lex);CHECK(t.kind==TOK_STR&&!strcmp(t.str,"hello\xe4\xb8\xadworld"));
    CHECK(lexer_next(&lex).kind==TOK_SHR_ASSIGN);CHECK(lexer_next(&lex).kind==TOK_ELLIPSIS);CHECK(lexer_next(&lex).kind==TOK_INC);CHECK(lexer_next(&lex).kind==TOK_NE);
    t=lexer_next(&lex);CHECK(t.kind==TOK_STR&&t.value==1&&!strcmp(t.str,"wide"));CHECK(lexer_next(&lex).kind==TOK_EOF);
    memset(window,0xa5,sizeof(window));CHECK(!strcmp(saved.str,"name_123"));CHECK(!lex.error&&errors==0&&lex.lookahead_tokens==2&&r.peak_window_bytes<=cap&&max_request<=cap);
    /* CRLF and UTF-8 display columns cross arbitrary refill boundaries. */
    lexer_init(&lex,"/*中*/ x\r\n  y\r z\nq");t=lexer_next(&lex);CHECK(t.line==1&&t.col==8&&t.src_start[0]=='x');t=lexer_next(&lex);CHECK(t.line==2&&t.col==3);t=lexer_next(&lex);CHECK(t.line==3&&t.col==2);t=lexer_next(&lex);CHECK(t.line==4&&t.col==1);
    /* Complete 80+ digit mantissa reaches libc without truncation. */
    lexer_init(&lex,"1.0000000000000001110223024625156540423631668090820312500000000000000000000000000000000001");t=lexer_next(&lex);CHECK(t.kind==TOK_FCONST&&t.fvalue==0x1.0000000000001p+0);
    lexer_init(&lex,"0 0u 0x0 0.0 -0.0 2147483648 0xffffffffu");CHECK(lexer_next(&lex).value==0);t=lexer_next(&lex);CHECK(t.value==0&&t.lit_unsigned);CHECK(lexer_next(&lex).value==0);CHECK(lexer_next(&lex).fvalue==0.0);CHECK(lexer_next(&lex).kind==TOK_MINUS);CHECK(lexer_next(&lex).fvalue==0.0);CHECK(lexer_next(&lex).value==2147483648LL);t=lexer_next(&lex);CHECK(t.value==4294967295LL&&t.lit_unsigned);
    lexer_init(&lex,"#pragma pack(2)\r\n   int x;");CHECK(pack==2);t=lexer_next(&lex);CHECK(t.kind==TOK_INT&&t.line==2&&t.col==4);
    /* Both input-size and resident arena independence: 8MB comment, zero token allocations. */
    size=8u*1024u*1024u+17u;used=0;calls=0;
    CHECK(xh_source_init(virtual_range,&size,size,window,cap,&r)==1);CHECK(lexer_init_source(&lex,&r)==0);CHECK(lexer_peek(&lex).kind==TOK_EOF&&used==0&&lex.peak_token_bytes==0);CHECK(calls<=(size+cap-1)/cap+8);
    printf("cap=%u payload_peak=%u lexer_bytes=%u comment_bytes=%u calls=%u arena=%u lookahead=%u\n",cap,r.peak_window_bytes,(unsigned)sizeof(lex),size,calls,used,lex.lookahead_tokens);
    used=0;errors=0;for(i=0;i<4097;i++)text[i]='a';text[4097]=0;lexer_init(&lex,text);CHECK(lex.error&&errors==1&&lex.peak_token_bytes==4096);
    errors=0;CHECK(xh_source_init(fault,0,32,window,cap,&r)==1);CHECK(lexer_init_source(&lex,&r)==ARK_ERR_IO);CHECK(lex.error==ARK_ERR_IO&&r.error==XH_SOURCE_IO&&errors==1);
    used=0;errors=0;strcpy(text,"{\n mov r0, r1\n}");CHECK(xh_source_init(range,text,(uint32_t)strlen(text),window,cap,&r)==1);CHECK(lexer_init_source(&lex,&r)==0);CHECK(!strcmp(lexer_copy_source_span(&lex,1,13),"\n mov r0, r1\n"));CHECK(!lexer_copy_source_span(&lex,0,XH_LEX_ASM_MAX+1)&&lex.error);
    return 0;
}
int main(void){CHECK(suite(1)==0);CHECK(suite(7)==0);CHECK(suite(1024)==0);puts("cross-window syntax, stable arena, float full lexeme, bounded comments/token, I/O errors OK");return 0;}
'''

class StreamLexerTests(unittest.TestCase):
    def test_native(self):
        self.run_harness(True)

    def test_wasi(self):
        self.run_harness(False)

    def run_harness(self, native):
        sdk = Path(os.environ.get('WASI_SDK_PATH', r'D:\wasi-sdk-34.0-x86_64-windows'))
        cc = (os.environ.get('XH_NATIVE_CLANG') or shutil.which('clang') or r'D:\msys64\ucrt64\bin\clang.exe') if native else (os.environ.get('WASI_CC') or str(sdk / 'bin/clang.exe'))
        runtime = os.environ.get('WASMTIME') or shutil.which('wasmtime') or r'C:\Program Files\Wasmtime\bin\wasmtime.exe'
        env = os.environ.copy()
        env['PATH'] = os.pathsep.join([str(Path(cc).parent), str(Path(sys.executable).parent), str(Path(sys.executable).parent / 'Library/bin'), r'D:\msys64\ucrt64\bin', env.get('PATH', '')])
        with tempfile.TemporaryDirectory(prefix='stream-lexer-', dir=ROOT / 'build') as tmp:
            directory=Path(tmp); source=directory/'harness.c';binary=directory/('harness.exe' if native else 'harness.wasm')
            source.write_text(HARNESS,encoding='utf-8')
            command=[cc]+([] if native else ['--target=wasm32-wasip1'])+['-std=c99','-Wall','-Wextra','-Werror','-I'+str(ROOT/'include'),str(source),str(ROOT/'src/frontend/c/lexer.c'),str(ROOT/'src/frontend/c/source_reader.c'),'-o',str(binary)]
            result=subprocess.run(command,env=env,capture_output=True,text=True,timeout=60)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result=subprocess.run([str(binary)] if native else [runtime,'run',str(binary)],env=env,capture_output=True,text=True,timeout=60)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('I/O errors OK',result.stdout)
            print(result.stdout.strip())

if __name__=='__main__':
    unittest.main()
