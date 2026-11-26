"""Independent exact CFG tests; no production compiler build or activation.
Set HOST_CC to a native C compiler. Tests compile only stack_depth.c + harness.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "xh_ir_stack.h"
#include <stdio.h>
#include <string.h>
int ark_streq(const char *a,const char *b){return a&&b&&!strcmp(a,b);}
static int effects(void *ctx,const IrFunc *f,int k,XhStackEffect *e){
 const IrInst *i=&f->code[k]; (void)ctx;
 switch(i->op){
 case IR_NOP:case IR_LABEL:case IR_LABEL_NAMED:break;
 case IR_PUSH_INT:e->push=1;break;
 case IR_64PUSH:e->push=2;break;
 case IR_POP:e->pop=1;break;
 case IR_DUP:e->pop=i->val>0?(int)i->val:1;e->push=e->pop*2;break;
 case IR_SWAP:e->pop=2;e->push=2;break;
 case IR_BINARY:e->pop=2;e->push=1;break;
 case IR_ALLOCA:e->pop=1;e->push=1;break;
 case IR_CALL:e->pop=(int)i->val;e->push=i->aux;break;
 case IR_RETURN:e->pop=1;e->flow=XH_STACK_STOP;break;
 case IR_JMP:case IR_JMP_NAMED:e->flow=XH_STACK_JUMP;break;
 case IR_JZ:case IR_JNZ:e->pop=e->taken_pop=1;e->flow=XH_STACK_BRANCH;break;
 case IR_CMP_JMP_EQ:e->taken_pop=1;e->flow=XH_STACK_BRANCH;break;
 default:return -1;
 }return 0;
}
#define I(op,v,a,n) {op,v,n,a,0,0,0}
#define P I(IR_PUSH_INT,1,0,0)
#define A I(IR_ALLOCA,0,0,0)
#define R I(IR_RETURN,0,0,0)
#define L(n) I(IR_LABEL,n,0,0)
#define J(n) I(IR_JMP,n,0,0)
#define Z(n) I(IR_JZ,n,0,0)
#define O I(IR_POP,0,0,0)
static int run(const char *name,IrInst *code,int n,int expected,int idx,int live){
 int depths[128],queue[128],targets[128];IrFunc f;XhStackError err;
 XhStackWorkspace w={depths,queue,targets,128,64};int s;
 memset(&f,0,sizeof f);f.code=code;f.code_count=n;
 s=xh_ir_stack_validate(&f,effects,0,&w,&err);
 if(s!=expected || (!s&&idx>=0&&xh_ir_stack_alloca_live_words(&f,&w,idx)!=live)){
  fprintf(stderr,"%s: status %d expected %d at %d\n",name,s,expected,err.instruction);return 1;
 }return 0;
}
#define T(name,expected,idx,live,...) do {IrInst c[]={__VA_ARGS__};errors+=run(name,c,sizeof(c)/sizeof(c[0]),expected,idx,live);count++;}while(0)
int main(void){int errors=0,count=0;
 T("alloc0",0,1,0,P,A,R);
 T("live1",0,2,1,P,P,A,O,R);
 T("live2",0,2,2,I(IR_64PUSH,0,0,0),P,A,O,O,R);
 T("diamond",0,7,0,P,Z(1),P,J(2),L(1),P,L(2),A,R);
 T("join_bad",XH_STACK_JOIN_MISMATCH,-1,0,P,Z(1),P,L(1),P,A,R);
 T("balanced_loop",0,6,0,L(1),P,Z(2),J(1),L(2),P,A,R);
 T("growing_loop",XH_STACK_JOIN_MISMATCH,-1,0,L(1),P,J(1));
 T("underflow",XH_STACK_UNDERFLOW,-1,0,O);
 T("binary_net_underflow",XH_STACK_UNDERFLOW,-1,0,P,I(IR_BINARY,0,0,0));
 T("dup_underflow",XH_STACK_UNDERFLOW,-1,0,I(IR_DUP,1,0,0));
 T("swap_underflow",XH_STACK_UNDERFLOW,-1,0,P,I(IR_SWAP,0,0,0));
 T("missing_label",XH_STACK_BAD_TARGET,-1,0,J(4));
 T("duplicate_label",XH_STACK_DUPLICATE_LABEL,-1,0,L(1),L(1));
 T("named",0,3,0,I(IR_JMP_NAMED,0,0,"ok"),I(IR_LABEL_NAMED,0,0,"ok"),P,A,R);
 T("unknown_named",XH_STACK_BAD_TARGET,-1,0,I(IR_JMP_NAMED,0,0,"bad"));
 T("duplicate_named",XH_STACK_DUPLICATE_LABEL,-1,0,I(IR_LABEL_NAMED,0,0,"x"),I(IR_LABEL_NAMED,0,0,"x"));
 T("empty_named",XH_STACK_BAD_LABEL,-1,0,I(IR_LABEL_NAMED,0,0,""));
 T("negative_label",XH_STACK_BAD_LABEL,-1,0,L(-1));
 T("unknown_reachable",XH_STACK_UNSUPPORTED,-1,0,I(IR_ASM,0,0,0));
 T("unknown_unreachable",XH_STACK_UNSUPPORTED,-1,0,P,R,I(IR_ASM,0,0,0));
 T("taken_effect",0,-1,0,P,I(IR_CMP_JMP_EQ,0,2,0),R,L(2),P,R);
 T("taken_underflow",XH_STACK_UNDERFLOW,-1,0,I(IR_CMP_JMP_EQ,0,2,0),L(2));
 T("callargs",0,3,2,P,P,P,A,I(IR_CALL,3,1,0),R);
 T("callvoid",0,1,0,P,A,I(IR_CALL,1,0,0),P,R);
 T("unreachable_alloca",0,3,-1,P,R,P,A);
 T("depth_budget",XH_STACK_LIMIT,-1,0,P,I(IR_DUP,1,0,0),I(IR_DUP,2,0,0),I(IR_DUP,4,0,0),I(IR_DUP,8,0,0),I(IR_DUP,16,0,0),I(IR_DUP,32,0,0),P);
 printf("%d CFG cases, %d failures\n",count,errors);return errors?1:0;
}
'''

class IrStackDepthTests(unittest.TestCase):
    def test_native_cfg_contract(self):
        cc = os.environ.get('HOST_CC') or shutil.which('clang') or shutil.which('gcc')
        if not cc:
            for candidate in (r'D:\msys64\ucrt64\bin\clang.exe', r'D:\msys64\ucrt64\bin\gcc.exe'):
                if Path(candidate).exists():
                    cc = candidate
                    break
        if not cc:
            self.skipTest('native compiler unavailable; set HOST_CC')
        env = os.environ.copy()
        env['PATH'] = str(Path(cc).parent) + os.pathsep + env.get('PATH', '')
        with tempfile.TemporaryDirectory(prefix='stack-depth-') as temp:
            directory = Path(temp)
            source = directory / 'harness.c'
            exe = directory / 'harness.exe'
            source.write_text(HARNESS, encoding='utf-8')
            build = subprocess.run([cc, '-std=c99', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'include'), str(source),
                str(ROOT / 'src/ir/stack_depth.c'), '-o', str(exe)],
                capture_output=True, text=True, env=env, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True,
                                    env=env, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('26 CFG cases, 0 failures', result.stdout)

if __name__ == '__main__':
    unittest.main()
