"""Restricted Thumb-1 legalization: real IR and production C, GNU M0, CPU.
Run after the unified build for production C coverage. No compiler rebuild here.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'build/test-deps'))
from test_ir_contract import HARNESS as IR_SUPPORT
SDK = Path(os.environ.get('WASI_SDK_PATH', r'D:\wasi-sdk-34.0-x86_64-windows')) / 'bin'
GNU = Path(os.environ.get('GNUARM', r'D:\arm-none-eabi')) / 'bin'
RUNTIME = os.environ.get('WASMTIME', r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
# Reuse only isolated arena/diagnostic/filesystem support, not its IR fixtures.
SUPPORT = IR_SUPPORT.split('static unsigned char wire')[0]
HARNESS = SUPPORT + r'''
static char assembly[262144];
static const CpuInfo m0={"Cortex-M0",6,0,0,0,0,1,0,0,0};
static void push(IrFunc*f,int n){ir_emit(f,IR_PUSH_INT,n,0,0);}
static void load(IrFunc*f,int off){ir_emit(f,IR_PUSH_VAR,off,0,4);}
static void store(IrFunc*f,int off){ir_emit(f,IR_STORE,off,0,4);}
static void binary(IrFunc*f,XhIrBinaryOp op){ir_emit(f,IR_BINARY,0,0,xh_ir_binary_to_c_token(op));}
int main(int argc,char**argv){
 IrProgram*p=ir_program_new();IrFunc*f=ir_func_new("entry"),*helper=ir_func_new("helper");
 FILE*out;int n,division=argc>1?atoi(argv[1]):0;
 arm_set_target_features(&m0,1);
 if(xh_template_set_path("arm","packages/arm/arm.rules"))return 1;
 f->ret_type=type_int();f->local_size=8;f->label_count=2;
 if(division){push(f,20);push(f,3);ir_emit(f,IR_DIV,0,0,1);ir_emit(f,IR_RETURN,0,0,0);}
 else{
  push(f,0);store(f,0);push(f,0);store(f,4);
  ir_emit(f,IR_LABEL,0,0,0);
  load(f,0);push(f,10);binary(f,XH_IR_BIN_LT);ir_emit(f,IR_JZ,1,0,0);
  load(f,4);load(f,0);binary(f,XH_IR_BIN_ADD);store(f,4);
  load(f,0);push(f,1);binary(f,XH_IR_BIN_ADD);store(f,0);
  ir_emit(f,IR_JMP,0,0,0);ir_emit(f,IR_LABEL,1,0,0);
  load(f,4);ir_emit(f,IR_CALL,0,"helper",0);binary(f,XH_IR_BIN_ADD);ir_emit(f,IR_RETURN,0,0,0);
  helper->ret_type=type_int();push(helper,0x12345678);ir_emit(helper,IR_RETURN,0,0,0);
 }
 ir_program_add(p,f);if(!division)ir_program_add(p,helper);
 n=ark_backend_arm()->emit_asm(p,assembly,sizeof(assembly));
 if(n<=0)return 3;
 out=fopen(XH_IR_TEST_DIR "/ir.s","wb");if(!out)return 4;fwrite(assembly,1,n,out);fclose(out);
 puts("Thumb-1 IR emission ready");return 0;
}
'''
SOURCE = '''
static int helper(void) { return 0x12345678; }
int entry(void) {
    int i=0, sum=0;
    while (i<10) { sum=sum+i; i=i+1; }
    return sum+helper();
}
'''

DIVISION_SOURCE = '''
int signed_divide(int a,int b) { return a/b; }
int signed_remainder(int a,int b) { return a%b; }
unsigned unsigned_divide(unsigned a,unsigned b) { return a/b; }
unsigned unsigned_remainder(unsigned a,unsigned b) { return a%b; }
static int canary(int x) { return x+7; }
int normal(void) { int a=210,b=5; return a/b; }
int nested(int a,int b) { return 100+signed_divide(a,b)+signed_remainder(a,b)+canary(3); }
'''

class Thumb1Codegen(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        (ROOT / 'build').mkdir(exist_ok=True)
        cls.workspace = tempfile.TemporaryDirectory(prefix='thumb1-', dir=ROOT / 'build')
        cls.addClassCleanup(cls.workspace.cleanup)
        cls.out = Path(cls.workspace.name)
        cls.env = os.environ.copy()
        pd = Path(sys.executable).parent
        cls.env['PATH'] = os.pathsep.join([str(pd), str(pd / 'Library/bin'), cls.env.get('PATH', '')])
        harness = cls.out / 'harness.c'
        harness.write_text(HARNESS)
        sources = ['src/ir/ir.c', 'src/ir/ir_contract_legacy.c', 'src/ir/ir_contract.c',
                   'src/frontend/c/type.c', 'src/frontend/c/agginit.c', 'src/core/codegen_common.c',
                   'src/backend/arm/backend_arm.c', 'src/backend/arm/thumb1.c', 'src/backend/rules/arm_rules.c',
                   'src/backend/rules/template.c', 'src/backend/rules/semantic.c', 'src/backend/rules/abi.c',
                   'src/backend/rules/encoder.c', 'src/backend/rules/relocation.c', 'src/backend/rules/selector.c',
                   'src/backend/rules/abiplan_wire.c', 'src/core/scatter.c']
        define = '-DXH_IR_TEST_DIR="' + cls.out.relative_to(ROOT).as_posix() + '"'
        p = subprocess.run([str(SDK / 'clang.exe'), '--target=wasm32-wasip1', '-O2', '-ffunction-sections',
                            '-fdata-sections', '-Iinclude', '-Iconfig', '-Iobject', '-include', 'include/xh_small.h',
                            '-DTEST_ARM', define, str(harness), *sources, '-Wl,--gc-sections',
                            '-o', str(cls.out / 'vm.wasm')], cwd=ROOT, env=cls.env, capture_output=True, text=True)
        if p.returncode:
            raise RuntimeError(p.stdout + p.stderr)

    def run_command(self, args, success=True):
        p = subprocess.run(list(map(str, args)), cwd=ROOT, env=self.env, capture_output=True, text=True)
        if success:
            self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        else:
            self.assertNotEqual(p.returncode, 0, p.stdout + p.stderr)
        return p

    def assemble_execute(self, source, expected=0x12345678+45, entry_name='entry', args=()):
        from elftools.elf.elffile import ELFFile
        from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
        from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_R1
        obj = source.with_suffix('.o'); exe = source.with_suffix('.elf')
        # GNU's M0-only assembler independently rejects any leaked Thumb-2 ISA.
        self.run_command([GNU / 'arm-none-eabi-as.exe', '-mcpu=cortex-m0', '-mthumb', source, '-o', obj])
        # External GNU archive is intentional: this does not test built-in .a loading.
        libgcc = self.run_command([GNU / 'arm-none-eabi-gcc.exe', '-mcpu=cortex-m0', '-mthumb',
                                   '-print-libgcc-file-name']).stdout.strip()
        self.run_command([GNU / 'arm-none-eabi-ld.exe', '-Ttext=0x10000', '-e', entry_name,
                          obj, libgcc, '-o', exe])
        with exe.open('rb') as stream:
            elf = ELFFile(stream)
            entry = elf.get_section_by_name('.symtab').get_symbol_by_name(entry_name)[0]['st_value']
            uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
            uc.mem_map(0x10000, 0x40000); uc.mem_map(0x700000, 0x20000)
            for sec in elf.iter_sections():
                if sec['sh_flags'] & 2 and sec['sh_type'] != 'SHT_NOBITS':
                    uc.mem_write(sec['sh_addr'], sec.data())
            uc.reg_write(UC_ARM_REG_SP, 0x71f000); uc.reg_write(UC_ARM_REG_LR, 0x4f001)
            for reg, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1), args):
                uc.reg_write(reg, value & 0xffffffff)
            uc.emu_start(entry | 1, 0x4f000, count=100000)
            self.assertEqual(uc.reg_read(UC_ARM_REG_R0) & 0xffffffff, expected & 0xffffffff)

    def test_ir_local_loop_call_constant_and_libgcc_div_cpu(self):
        self.run_command([RUNTIME, 'run', '--dir=.', self.out / 'vm.wasm', '0'])
        self.assemble_execute(self.out / 'ir.s')
        self.run_command([RUNTIME, 'run', '--dir=.', self.out / 'vm.wasm', '1'])
        self.assemble_execute(self.out / 'ir.s', 6)

    def test_production_c_to_m0_assembly_cpu(self):
        from test_driver import command
        source = self.out / 'program.c'; assembly = self.out / 'program.s'
        source.write_text(SOURCE)
        self.run_command([*command(), 'arm-none-eabi-gcc', '-mcpu=cortex-m0', '-O0', '-S',
                          source.relative_to(ROOT), '-o', assembly.relative_to(ROOT)])
        self.assemble_execute(assembly)

    def test_production_m0_division_remainder_libgcc_cpu(self):
        from test_driver import command
        source = self.out / 'division.c'; assembly = self.out / 'division.s'
        source.write_text(DIVISION_SOURCE)
        self.run_command([*command(), 'arm-none-eabi-gcc', '-mcpu=cortex-m0', '-O0', '-S',
                          source.relative_to(ROOT), '-o', assembly.relative_to(ROOT)])
        cases = [('signed_divide', (-17, 5), -3), ('signed_remainder', (-17, 5), -2),
                 ('unsigned_divide', (0xffffffff, 2), 2147483647),
                 ('unsigned_remainder', (0xffffffff, 2), 1),
                 ('signed_divide', (17, 5), 3), ('normal', (), 42), ('nested', (-17, 5), 105)]
        for name, args, expected in cases:
            with self.subTest(entry=name, args=args):
                self.assemble_execute(assembly, expected, name, args)

    def test_production_m0_raw_sdiv_rejected(self):
        from test_driver import command
        source = self.out / 'unsupported.c'; assembly = self.out / 'unsupported.s'
        source.write_text('int entry(void){asm volatile("sdiv r0, r0, r1");return 0;}\n')
        self.run_command([*command(), 'arm-none-eabi-gcc', '-mcpu=cortex-m0', '-O0', '-S',
                          source.relative_to(ROOT), '-o', assembly.relative_to(ROOT)], success=False)
        self.assertFalse(assembly.exists())

if __name__ == '__main__':
    unittest.main()
