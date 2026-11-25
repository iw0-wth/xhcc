"""Standalone WASI reader test: real range I/O, bounded memory, injected faults.

Does not build or invoke the compiler app. Its source stays an ordinary .c
translation unit so undefined symbols and accidental allocation fail the link.
"""
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
#include <string.h>
#include <limits.h>
#include "xh_source_reader.h"
#include "flash_fs.h"
#define CHECK(x) do { if (!(x)) { printf("failed line %d: %s\n",__LINE__,#x); return 1; } } while (0)
void ark_log(const char *format, ...) { (void)format; }
/* These symbols deliberately poison allocation if source_reader gains any. */
#define SIZE (1024u*1024u+1031u)
static unsigned char expected(uint32_t p) {
    uint32_t m=p%1024u;
    if(m==1023u)return '\r'; if(m==0u)return '\n';
    if(m==1u)return 0xe4; if(m==2u)return 0xb8; if(m==3u)return 0xad;
    if(m==4u)return 0; return (unsigned char)('a'+p%26u);
}
static uint32_t real_calls,real_max;
static int counted_range(void *context,uint32_t off,unsigned char *buf,uint32_t count) {
    real_calls++;if(count>real_max)real_max=count;
    return fs_read_range((const char *)context,off,buf,count);
}
typedef struct { int mode, calls; uint32_t max_count; } Fault;
static int callback(void *context,uint32_t off,unsigned char *buf,uint32_t count) {
    Fault *f=(Fault *)context;uint32_t n=count,i;
    f->calls++;if(count>f->max_count)f->max_count=count;
    if(f->mode==1)return -1;
    if(f->mode==2)return 0;
    if(f->mode==3)return (int)count+1;
    if(f->mode==4 && off>=7)return 0;
    if(n>7)n=7;
    for(i=0;i<n;i++)buf[i]=expected(off+i);
    return (int)n;
}
static int virtual_end(void *context,uint32_t off,unsigned char *buf,uint32_t count) {
    (void)context; if(off!=(uint32_t)INT_MAX-1u||count!=1)return -1;
    buf[0]=42; return 1;
}
int main(void) {
    struct { unsigned char before[8],window[1024],after[8]; } guard;
    XhSourceReader r;XhSourceMark mark;unsigned char b=0xa5;
    uint32_t i,line=1,column=1;int cr=0;Fault f={0,0,0};
    memset(&guard,0xa5,sizeof(guard));
    CHECK(xh_source_open("large.bin",guard.window,sizeof(guard.window),&r)==XH_SOURCE_OK);
    CHECK(r.size==SIZE && r.peak_window_bytes==0 && r.context==&r);
    CHECK(xh_source_peek(&r,1024,&b)==XH_SOURCE_LOOKAHEAD);
    CHECK(xh_source_read_byte(&r,UINT32_MAX,&b)==XH_SOURCE_INVALID);
    for(i=0;i<SIZE;i++) {
        if(i%8191u==0) {
            uint32_t p=(i*37u)%SIZE;
            CHECK(xh_source_read_byte(&r,p,&b)==XH_SOURCE_OK && b==expected(p));
            CHECK(r.offset==i && r.line==line && r.column==column);
        }
        CHECK(xh_source_consume(&r,&b)==XH_SOURCE_OK && b==expected(i));
        if(b=='\r'){line++;column=1;cr=1;}
        else if(b=='\n'){if(!cr)line++;column=1;cr=0;}
        else {column++;cr=0;}
        CHECK(r.line==line && r.column==column);
    }
    b=0xa5;CHECK(xh_source_consume(&r,&b)==XH_SOURCE_EOF && b==0xa5);
    CHECK(xh_source_read_byte(&r,SIZE,&b)==XH_SOURCE_EOF);
    CHECK(xh_source_read_byte(&r,SIZE+1u,&b)==XH_SOURCE_INVALID);
    CHECK(r.peak_window_bytes==1024);
    for(i=0;i<8;i++)CHECK(guard.before[i]==0xa5 && guard.after[i]==0xa5);
    printf("real range bytes=%u payload_peak=%u reader_metadata=%u total_owned=%u\n",
           SIZE,r.peak_window_bytes,(unsigned)sizeof(r),(unsigned)(sizeof(r)+sizeof(guard.window)));
    CHECK(xh_source_init(counted_range,"large.bin",SIZE,guard.window,1024,&r)==XH_SOURCE_OK);
    for(i=0;i<SIZE;i++)CHECK(xh_source_consume(&r,&b)==XH_SOURCE_OK && b==expected(i));
    CHECK(real_calls==(SIZE+1023u)/1024u && real_max==1024);
    printf("instrumented fs_read_range calls=%u max_request=%u no_readall\n",real_calls,real_max);
    xh_source_close(&r);CHECK(xh_source_peek(&r,0,&b)==XH_SOURCE_INVALID);
    CHECK(xh_source_open("missing.bin",guard.window,1024,&r)==XH_SOURCE_IO);
    CHECK(xh_source_init(callback,&f,INT_MAX,guard.window,0,&r)==XH_SOURCE_INVALID);
    CHECK(xh_source_init(callback,&f,UINT32_MAX,guard.window,1024,&r)==XH_SOURCE_INVALID);
    CHECK(xh_source_init(callback,&f,1,guard.window,4097,&r)==XH_SOURCE_INVALID);
    CHECK(xh_source_init(callback,&f,0,guard.window,1,&r)==XH_SOURCE_OK);
    CHECK(xh_source_consume(&r,&b)==XH_SOURCE_EOF && f.calls==0);
    CHECK(xh_source_init(callback,&f,5000,guard.window,1024,&r)==XH_SOURCE_OK);
    CHECK(xh_source_consume(&r,&b)==XH_SOURCE_OK && b==expected(0));
    mark=xh_source_mark(&r);
    CHECK(xh_source_consume(&r,&b)==XH_SOURCE_OK);
    CHECK(xh_source_restore(&r,mark)==XH_SOURCE_OK && r.offset==1 && r.line==2);
    CHECK(xh_source_consume(&r,&b)==XH_SOURCE_OK && b==0xe4);
    CHECK(xh_source_read_byte(&r,3000,&b)==XH_SOURCE_OK && b==expected(3000));
    CHECK(xh_source_restore(&r,mark)==XH_SOURCE_LOOKAHEAD);
    CHECK(xh_source_consume(&r,&b)==XH_SOURCE_OK && b==0xb8);
    CHECK(f.calls>100 && f.max_count<=1024 && r.peak_window_bytes==1024);
    for(f.mode=1;f.mode<=4;f.mode++) {
        CHECK(xh_source_init(callback,&f,100,guard.window,16,&r)==XH_SOURCE_OK);
        b=0xa5;CHECK(xh_source_consume(&r,&b)==XH_SOURCE_IO && b==0xa5 && r.offset==0);
        i=(uint32_t)f.calls;
        CHECK(xh_source_read_byte(&r,0,&b)==XH_SOURCE_IO && (uint32_t)f.calls==i);
    }
    f.mode=0;
    CHECK(xh_source_init(callback,&f,4,guard.window,1,&r)==XH_SOURCE_OK);
    for(i=0;i<4;i++)CHECK(xh_source_consume(&r,&b)==XH_SOURCE_OK && b==expected(i));
    CHECK(xh_source_consume(&r,&b)==XH_SOURCE_EOF && r.peak_window_bytes==1);
    /* A two-byte window splits the UTF-8 sequence; CRLF crosses refills too. */
    CHECK(xh_source_init(callback,&f,1030,guard.window,2,&r)==XH_SOURCE_OK);
    for(i=0;i<1030;i++)CHECK(xh_source_consume(&r,&b)==XH_SOURCE_OK && b==expected(i));
    CHECK(r.line==3 && r.column==6 && r.peak_window_bytes==2);
    CHECK(xh_source_init(virtual_end,0,INT_MAX,guard.window,1024,&r)==XH_SOURCE_OK);
    CHECK(xh_source_read_byte(&r,INT_MAX-1u,&b)==XH_SOURCE_OK && b==42);
    CHECK(xh_source_read_byte(&r,INT_MAX,&b)==XH_SOURCE_EOF);
    puts("short-read, truncated EOF, callback faults, CRLF, UTF-8, NUL and marks OK");
    return 0;
}
'''


class SourceReaderTests(unittest.TestCase):
    def test_standalone_wasi(self):
        self.run_reader(False)

    def test_standalone_native(self):
        self.run_reader(True)

    def run_reader(self, native):
        sdk = Path(os.environ.get('WASI_SDK_PATH', r'D:\wasi-sdk-34.0-x86_64-windows'))
        cc = os.environ.get('WASI_CC') or str(sdk / ('bin/clang.exe' if os.name == 'nt' else 'bin/clang'))
        runtime = os.environ.get('WASMTIME') or shutil.which('wasmtime') or r'C:\Program Files\Wasmtime\bin\wasmtime.exe'
        if native:
            cc = os.environ.get('XH_NATIVE_CLANG') or shutil.which('clang') or r'D:\msys64\ucrt64\bin\clang.exe'
        env = os.environ.copy()
        if os.name == 'nt':
            py_dir = Path(sys.executable).parent
            env['PATH'] = os.pathsep.join([str(Path(cc).parent), str(py_dir), str(py_dir / 'Library/bin'), env.get('PATH', '')])
        with tempfile.TemporaryDirectory(prefix='source-reader-', dir=ROOT / 'build') as tmp:
            directory = Path(tmp)
            source, binary = directory / 'reader.c', directory / ('reader.exe' if native else 'reader.wasm')
            size = 1024 * 1024 + 1031
            block = bytearray(ord('a') + i % 26 for i in range(1024 * 13))
            # LCM(1024,26) preserves both boundary bytes and alphabet pattern.
            for i in range(len(block)):
                m = i % 1024
                if m in (0, 1, 2, 3, 4, 1023):
                    block[i] = {0: 10, 1: 0xe4, 2: 0xb8, 3: 0xad, 4: 0, 1023: 13}[m]
            with (directory / 'large.bin').open('wb') as target:
                remaining = size
                while remaining:
                    part = block[:min(remaining, len(block))]
                    target.write(part)
                    remaining -= len(part)
            source.write_text(HARNESS, encoding='utf-8')
            command = [cc] + ([] if native else ['--target=wasm32-wasip1']) + ['-std=c99', '-Wall', '-Wextra', '-Werror',
                       '-D_POSIX_C_SOURCE=200809L', '-Dmalloc=xh_forbidden_malloc',
                       '-Dcalloc=xh_forbidden_calloc', '-Drealloc=xh_forbidden_realloc',
                       '-I' + str(ROOT / 'include'),
                       str(source), str(ROOT / 'src/frontend/c/source_reader.c'),
                       str(ROOT / 'src/platform/wasi/flash_fs_host.c'), '-o', str(binary)]
            built = subprocess.run(command, env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            execute = [str(binary)] if native else [runtime, 'run', '--dir=.', str(binary)]
            result = subprocess.run(execute, cwd=directory, env=env,
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('payload_peak=1024', result.stdout)
            self.assertIn('callback faults', result.stdout)
            print(result.stdout.strip())


if __name__ == '__main__':
    unittest.main()
