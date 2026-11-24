"""Non-C frontend -> neutral/legacy codecs -> AMD64, ARM and RV32 CPUs.
Builds isolated target harnesses, never changes the unified compiler binary.
"""
from pathlib import Path
import os, struct, subprocess, sys, unittest
ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/ir-contract-tests'
SDK = Path(os.environ.get('WASI_SDK_PATH', r'D:\wasi-sdk-34.0-x86_64-windows')) / 'bin'
RUNTIME = os.environ.get('WASMTIME', r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
sys.path.insert(0, str(ROOT / 'build/test-deps'))
# This producer translation unit cannot see legacy IR, C Type or token headers.
FRONTEND = r'''
#include "xh_ir_contract.h"
void synthetic_frontend(XhIrCoreProgram *p, XhIrCoreInst *code, int mode) {
    XhIrValueType t={XH_IR_VALUE_INTEGER,4,mode?XH_IR_SIGN_UNSIGNED:XH_IR_SIGN_SIGNED,0,0};
    unsigned k;
    for(k=0;k<4;k++){code[k].opcode=0;code[k].binary_op=0;code[k].type=t;code[k].bits=0;}
    p->version=XH_IR_CORE_VERSION;p->result=t;p->code=code;p->code_count=4;
    code[0].opcode=code[1].opcode=XH_IR_CORE_CONST;
    code[0].bits=mode?0xffffffffu:6;code[1].bits=mode?2:7;
    code[2].opcode=XH_IR_CORE_BINARY;
    code[2].binary_op=mode==2?XH_IR_BIN_GT:mode?XH_IR_BIN_DIV:XH_IR_BIN_MUL;
    code[3].opcode=XH_IR_CORE_RETURN;
}
'''
HARNESS = r'''
#include "xh_ir_contract.h"
#include "xh_ir_legacy_adapter.h"
#include "irser.h"
#include "backend.h"
#include "memory.h"
#include "config.h"
#include "xh_template.h"
#include "xh_rules.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
ArkccConfig g_config;
#ifndef TEST_ARM
int g_xhcc_elf_mode=1;
#endif
#if defined(TEST_ARM)
#define TEST_BACKEND ark_backend_arm
#define TEST_TARGET "arm"
#define TEST_RULES "packages/arm/arm.rules"
#elif defined(TEST_RV)
#define TEST_BACKEND ark_backend_riscv32
#define TEST_TARGET "riscv32"
#define TEST_RULES "packages/riscv32/rv.rules"
#else
#define TEST_BACKEND ark_backend_amd64
#define TEST_TARGET "x86_64"
#define TEST_RULES "packages/x86_64/x86.rules"
#endif
void diag_tool(int s,const char*w,const char*m,const char*a,const char*b,const char*c,const char*d,const char*e,const char*f){fprintf(stderr,"%s: %s\n",w,m);}
int ark_strlen(const char*s){return(int)strlen(s);}int ark_streq(const char*a,const char*b){return a&&b&&!strcmp(a,b);}
const char*diag_num(char*b,int n){sprintf(b,"%d",n);return b;}
void ark_log(const char*fmt,...){(void)fmt;}
static unsigned char arena[8*1024*1024];static unsigned used;
void*mem_pool_alloc(int n){void*p;n=(n+7)&~7;if(n<0||used+(unsigned)n>sizeof(arena))exit(90);p=arena+used;used+=n;memset(p,0,n);return p;}
void mem_pool_free(void*p,int size){(void)p;(void)size;}
void*mem_realloc(void*p,int old,int n){void*q=mem_pool_alloc(n);if(p)memcpy(q,p,old<n?old:n);return q;}
char*mem_strdup(const char*s){int n=strlen(s)+1;char*p=mem_pool_alloc(n);memcpy(p,s,n);return p;}
int fs_file_size(const char*p){FILE*f=fopen(p,"rb");long n;if(!f)return -1;fseek(f,0,SEEK_END);n=ftell(f);fclose(f);return(int)n;}
int fs_read_file(const char*p,char*b,int c){FILE*f=fopen(p,"rb");int n;if(!f)return -1;n=fread(b,1,c-1,f);b[n]=0;fclose(f);return n;}
static unsigned char wire[32768],legacy[32768];static char assembly[65536];
#define CHECK(x) do{if(!(x)){fprintf(stderr,"check line %d\n",__LINE__);return 1;}}while(0)
void synthetic_frontend(XhIrCoreProgram*,XhIrCoreInst*,int);
int main(int argc,char**argv){
 XhIrCoreInst code[6]={{0}},storage[128];XhIrCoreProgram p={1},copy;XhIrCoreError err;
 IrProgram *old,*back;int n,k,mode=argc>1?atoi(argv[1]):0;FILE*f;
 synthetic_frontend(&p,code,mode);
 CHECK(xh_ir_core_validate(&p,&err)==XH_IR_CORE_OK);
 CHECK(xh_ir_core_effects(&code[2])==(mode==1?XH_IR_EFFECT_MAY_TRAP:XH_IR_EFFECT_PURE));
 for(k=1;k<=16;k++)CHECK(xh_ir_binary_from_c_token(xh_ir_binary_to_c_token((XhIrBinaryOp)k))==(XhIrBinaryOp)k);
 CHECK(xh_ir_binary_to_c_token((XhIrBinaryOp)999)==-1);
 CHECK(xh_ir_binary_from_c_token(-1)==XH_IR_BIN_INVALID);
 /* The production consumer boundary accepts ONLY the documented wire bits. */
 {IrInst inst={0};unsigned flags,bit;int op;inst.op=IR_BINARY;
  CHECK(xh_ir_legacy_binary(0)==XH_IR_BIN_INVALID);
  for(op=1;op<=16;op++)for(flags=0;flags<4;flags++){
   inst.aux=xh_ir_binary_to_c_token((XhIrBinaryOp)op)|((flags&1)?0x100:0)|((flags&2)?IR_AUX_SLOT64:0);
   CHECK(xh_ir_legacy_binary(&inst)==(XhIrBinaryOp)op);
  }
  inst.aux=TOK_PLUS;
  inst.op=IR_64BINARY;CHECK(xh_ir_legacy_binary(&inst)==XH_IR_BIN_INVALID);
  inst.op=IR_FADD;CHECK(xh_ir_legacy_binary(&inst)==XH_IR_BIN_INVALID);
  inst.op=IR_PUSH_INT;CHECK(xh_ir_legacy_binary(&inst)==XH_IR_BIN_INVALID);
  inst.op=IR_BINARY;
  for(bit=9;bit<32;bit++)if((1u<<bit)!=(unsigned)IR_AUX_SLOT64){
   inst.aux=(int)((unsigned)TOK_PLUS|(1u<<bit));
   CHECK(xh_ir_legacy_binary(&inst)==XH_IR_BIN_INVALID);
  }
  inst.aux=-1;CHECK(xh_ir_legacy_binary(&inst)==XH_IR_BIN_INVALID);
  inst.aux=255;CHECK(xh_ir_legacy_binary(&inst)==XH_IR_BIN_INVALID);
  inst.aux=TOK_AND;CHECK(xh_ir_legacy_binary(&inst)==XH_IR_BIN_INVALID);
  inst.aux=TOK_OR;CHECK(xh_ir_legacy_binary(&inst)==XH_IR_BIN_INVALID);
  inst.aux=TOK_ADD_ASSIGN;CHECK(xh_ir_legacy_binary(&inst)==XH_IR_BIN_INVALID);
 }

 /* Failures matter: reject typed stack mismatch, underflow and unsupported width. */
 code[1].type.signedness=mode?XH_IR_SIGN_SIGNED:XH_IR_SIGN_UNSIGNED;
 CHECK(xh_ir_core_validate(&p,&err)==XH_IR_CORE_TYPE_MISMATCH&&err.instruction==2);
 code[1].type=code[0].type;code[0].opcode=XH_IR_CORE_BINARY;code[0].binary_op=XH_IR_BIN_ADD;code[0].bits=0;
 CHECK(xh_ir_core_validate(&p,&err)==XH_IR_CORE_STACK_UNDERFLOW);
 code[0].opcode=XH_IR_CORE_CONST;code[0].binary_op=0;code[0].bits=mode?0xffffffffu:6;code[0].type.byte_width=8;
 CHECK(xh_ir_core_validate(&p,&err)==XH_IR_CORE_UNSUPPORTED_TYPE);code[0].type.byte_width=4;
 code[2].binary_op=999;CHECK(xh_ir_core_validate(&p,&err)==XH_IR_CORE_BAD_OPERAND);code[2].binary_op=mode==2?XH_IR_BIN_GT:mode?XH_IR_BIN_DIV:XH_IR_BIN_MUL;
 n=xh_ir_core_encode(&p,wire,sizeof(wire));CHECK(n==184);CHECK(!memcmp(wire,"XHIRCORE",8));
 CHECK(xh_ir_core_decode(wire,n,&copy,storage,128,&err)==XH_IR_CORE_OK);
 CHECK(xh_ir_core_encode(&copy,legacy,sizeof(legacy))==n&&!memcmp(wire,legacy,n));
 CHECK(xh_ir_core_decode(wire,n-1,&copy,storage,128,&err)==XH_IR_CORE_BAD_WIRE&&!copy.code);
 CHECK(xh_ir_core_decode(wire,n,&copy,storage,1,&err)==XH_IR_CORE_LIMIT);
 wire[8]=99;CHECK(xh_ir_core_decode(wire,n,&copy,storage,128,&err)==XH_IR_CORE_BAD_VERSION);wire[8]=1;
 CHECK(xh_ir_core_decode(wire,n,&copy,storage,128,&err)==XH_IR_CORE_OK);
 f=fopen("build/ir-contract-tests/core.bin","wb");CHECK(f);fwrite(wire,1,n,f);fclose(f);
 old=xh_ir_core_to_legacy(&copy,"entry",&err);CHECK(old);
 CHECK(xh_ir_legacy_binary(&old->funcs[0]->code[2])==(XhIrBinaryOp)copy.code[2].binary_op);
 n=ir_serialize(old,legacy,sizeof(legacy),"synthetic");CHECK(n>0);
 back=ir_deserialize(legacy,n,"synthetic");CHECK(back);
 CHECK(xh_ir_legacy_binary(&back->funcs[0]->code[2])==(XhIrBinaryOp)copy.code[2].binary_op);
 CHECK(xh_template_set_path(TEST_TARGET,TEST_RULES)==0);
#ifdef TEST_RV
 if(argc>2)CHECK(xh_rules_set_selection_path("build/ir-contract-tests/empty.selection")==0);
#endif
 /* Consumer integration must reject unknown flags instead of masking them. */
 {int saved=back->funcs[0]->code[2].aux;unsigned bad;
  for(bad=0x200;bad<=0x800;bad<<=2){
   back->funcs[0]->code[2].aux=saved|(int)bad;
   CHECK(TEST_BACKEND()->emit_asm(back,assembly,sizeof(assembly))<0);
  }
  back->funcs[0]->code[2].aux=saved;
 }
 n=TEST_BACKEND()->emit_asm(back,assembly,sizeof(assembly));CHECK(n>0);
 f=fopen("build/ir-contract-tests/output.s","wb");CHECK(f);fwrite(assembly,1,n,f);fclose(f);
 puts("non-C frontend: core validate + canonical codec + legacy roundtrip + real " TEST_TARGET " emission OK");return 0;
}
'''

class IrContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        (OUT / 'harness.c').write_text(HARNESS)
        (OUT / 'frontend.c').write_text(FRONTEND)
        (OUT / 'empty.selection').write_text('XHSELECT 2\n')
        cls.env = os.environ.copy()
        pd = Path(sys.executable).parent
        cls.env['PATH'] = os.pathsep.join([str(pd), str(pd / 'Library/bin'), cls.env.get('PATH', '')])
        common = ['src/ir/ir_contract.c', 'src/ir/ir_contract_legacy.c', 'src/ir/ir.c', 'src/ir/irser.c',
                  'src/frontend/c/type.c', 'src/core/codegen_common.c', 'src/backend/rules/abiplan_wire.c',
                  'src/backend/rules/template.c', 'src/backend/rules/semantic.c', 'src/backend/rules/abi.c',
                  'src/backend/rules/encoder.c', 'src/backend/rules/relocation.c', 'src/backend/rules/selector.c', 'src/frontend/c/agginit.c']
        targets = {
            'x86_64': ([], ['src/backend/x86_64/backend_x86_64.c', 'src/backend/rules/x86_rules.c',
                           'src/backend/rules/sysv_plan.c', 'src/backend/rules/abiplan.c', 'src/backend/rules/typeclass.c']),
            'arm': (['-DTEST_ARM'], ['src/backend/arm/backend_arm.c', 'src/backend/rules/arm_rules.c', 'src/core/scatter.c']),
            'riscv32': (['-DTEST_RV'], ['src/backend/riscv32/backend_riscv32.c', 'src/backend/rules/rv_rules.c'])}
        selected = os.environ.get('XH_IR_TEST_TARGETS', 'x86_64,arm,riscv32').split(',')
        cls.selected_targets = selected
        for target, (defines, sources) in targets.items():
            if target not in selected:
                continue
            args = [str(SDK / 'clang.exe'), '--target=wasm32-wasip1', '-O2', '-ffunction-sections',
                    '-fdata-sections', '-Iinclude', '-Iconfig', '-Iobject', '-include', 'include/xh_small.h', *defines,
                    str(OUT / 'harness.c'), str(OUT / 'frontend.c'), *common, *sources,
                    '-Wl,--gc-sections', '-o', str(OUT / (target + '.wasm'))]
            p = subprocess.run(args, cwd=ROOT, env=cls.env, capture_output=True, text=True)
            if p.returncode:
                raise RuntimeError(target + ': ' + p.stdout + p.stderr)

    def test_synthetic_frontend_cpu_results(self):
        from elftools.elf.elffile import ELFFile
        from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_ARCH_RISCV, UC_MODE_RISCV32
        from unicorn.x86_const import UC_X86_REG_RSP, UC_X86_REG_RAX
        from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0
        from unicorn.riscv_const import UC_RISCV_REG_SP, UC_RISCV_REG_RA, UC_RISCV_REG_A0
        targets = {
            'x86_64': ('x86_64-unknown-linux-gnu', [], 'elf_x86_64', UC_ARCH_X86, UC_MODE_64),
            'arm': ('thumbv7em-none-eabi', ['-mcpu=cortex-m4', '-mthumb'], 'armelf', UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS),
            'riscv32': ('riscv32-unknown-elf', ['-march=rv32im', '-mabi=ilp32'], 'elf32lriscv', UC_ARCH_RISCV, UC_MODE_RISCV32)}
        clang = os.environ.get('XH_NATIVE_CLANG', r'D:\msys64\ucrt64\bin\clang.exe')
        for target, (triple, flags, emulation, arch, cpu_mode) in targets.items():
            if target not in self.selected_targets:
                continue
            cases = [(0, 42, False), (1, 2147483647, False), (2, 1, False)]
            if target == 'riscv32':
                cases.append((1, 2147483647, True))
            for mode, expected, fallback in cases:
                with self.subTest(target=target, mode=mode, fallback=fallback):
                    extra = ['fallback'] if fallback else []
                    p = subprocess.run([RUNTIME, 'run', '--dir=.', str(OUT / (target + '.wasm')), str(mode), *extra],
                                       cwd=ROOT, env=self.env, capture_output=True, text=True)
                    self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
                    self.assertIn('real ' + target + ' emission OK', p.stdout)
                    self.assertEqual((OUT / 'core.bin').read_bytes()[:8], b'XHIRCORE')
                    for args in [
                        [clang, '--target=' + triple, *flags, '-c', str(OUT / 'output.s'), '-o', str(OUT / 'output.o')],
                        [str(SDK / 'ld.lld.exe'), '-m', emulation, '--no-relax', '--image-base=0', '-Ttext=0x10000', '-e', 'entry', '-o', str(OUT / 'output.elf'), str(OUT / 'output.o')]]:
                        p = subprocess.run(args, cwd=ROOT, env=self.env, capture_output=True, text=True)
                        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
                    with (OUT / 'output.elf').open('rb') as f:
                        elf = ELFFile(f)
                        entry = elf.get_section_by_name('.symtab').get_symbol_by_name('entry')[0]['st_value']
                        uc = Uc(arch, cpu_mode)
                        uc.mem_map(0x10000, 0x30000); uc.mem_map(0x700000, 0x20000)
                        for sec in elf.iter_sections():
                            if sec['sh_flags'] & 2 and sec['sh_type'] != 'SHT_NOBITS':
                                uc.mem_write(sec['sh_addr'], sec.data())
                        if target == 'x86_64':
                            uc.reg_write(UC_X86_REG_RSP, 0x71eff8)
                            uc.mem_write(0x71eff8, struct.pack('<Q', 0x3f000))
                            result_register = UC_X86_REG_RAX
                        elif target == 'arm':
                            uc.reg_write(UC_ARM_REG_SP, 0x71f000)
                            uc.reg_write(UC_ARM_REG_LR, 0x3f001)
                            entry |= 1
                            result_register = UC_ARM_REG_R0
                        else:
                            uc.reg_write(UC_RISCV_REG_SP, 0x71f000)
                            uc.reg_write(UC_RISCV_REG_RA, 0x3f000)
                            result_register = UC_RISCV_REG_A0
                        uc.emu_start(entry, 0x3f000, count=200000)
                        self.assertEqual(uc.reg_read(result_register) & 0xffffffff, expected)

if __name__ == '__main__':
    unittest.main()
