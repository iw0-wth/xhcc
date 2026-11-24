"""C template integration against the already-built compiler (no build here).

An unchanged Wasm SHA emits different C after an external arithmetic rule edit;
when a host C compiler is present the two generated programs execute differently.
XH_C_NATIVE_CC selects the native compiler explicitly.
"""
from pathlib import Path
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RULES = ROOT / 'packages/c/c.rules'
COMPILER = ROOT / 'build/wasm/xhcc.wasm'


class CRules(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not COMPILER.is_file():
            raise unittest.SkipTest('build the compiler first')
        cls.workspace = tempfile.TemporaryDirectory(prefix='c-rules-', dir=ROOT)
        cls.work = Path(cls.workspace.name)
        cls.package_root = cls.work / 'packages'
        cls.package = cls.package_root / 'c_variant'
        cls.package.mkdir(parents=True)
        (cls.package / 'include').mkdir()
        shutil.copy2(ROOT / 'packages/c/target.rules', cls.package / 'target.rules')
        manifest = (ROOT / 'packages/c/package.ini').read_text(encoding='utf-8')
        manifest = manifest.replace('id=c\n', 'id=c_variant\n').replace('tools=xh-c-gcc', 'tools=c-variant-gcc')
        if not re.search(r'^templates=', manifest, re.M):
            manifest += '\ntemplates=c.rules\n'
        (cls.package / 'package.ini').write_text(manifest, encoding='utf-8')
        cls.variant = cls.package / 'c.rules'
        cls.original = RULES.read_text(encoding='utf-8')
        cls.source = cls.work / 'source.c'
        cls.source.write_text('int main(void) { int a = 10, b = 3; return a + b; }\n', encoding='utf-8')

    @classmethod
    def tearDownClass(cls):
        cls.workspace.cleanup()

    def compile(self, text, label='generated', success=True):
        self.variant.write_text(text, encoding='utf-8')
        output = self.work / (label + '.c')
        if output.exists():
            output.unlink()
        process = subprocess.run([
            sys.executable, 'tools/xhcc.py', '--package-root', str(self.package_root),
            'c-variant-gcc', '-O0', '-S', str(self.source.relative_to(ROOT)),
            '-o', str(output.relative_to(ROOT))], cwd=ROOT, capture_output=True, text=True, timeout=60)
        if success:
            self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
            self.assertTrue(output.is_file(), process.stdout + process.stderr)
        else:
            self.assertNotEqual(process.returncode, 0, process.stdout + process.stderr)
            self.assertFalse(output.exists(), process.stdout + process.stderr)
            self.assertRegex(process.stderr.lower(), r'template|semantic')
        return output

    def test_external_arithmetic_changes_generated_c_and_execution_same_sha(self):
        before = hashlib.sha256(COMPILER.read_bytes()).digest()
        base = self.compile(self.original, 'base')
        changed = self.original.replace('binary ADD 32 s seq.binary.ADD.32.s 2 1',
                                        'binary ADD 32 s seq.binary.SUB.32.s 2 1', 1)
        self.assertNotEqual(changed, self.original)
        variant = self.compile(changed, 'variant')
        self.assertIn('(ark_a + ark_c)', base.read_text(encoding='utf-8'))
        self.assertIn('(ark_a - ark_c)', variant.read_text(encoding='utf-8'))
        self.assertIn('text (ark_a + ark_c)', changed)  # old fragment remains unchanged
        self.assertNotIn('(ark_a + ark_c)', variant.read_text(encoding='utf-8'))
        self.assertNotEqual(base.read_bytes(), variant.read_bytes())
        self.assertEqual(hashlib.sha256(COMPILER.read_bytes()).digest(), before)
        compiler = os.environ.get('XH_C_NATIVE_CC') or shutil.which('gcc')
        if not compiler:
            for candidate in [r'C:\msys64\ucrt64\bin\gcc.exe', r'C:\msys64\mingw64\bin\gcc.exe',
                              r'D:\msys64\ucrt64\bin\gcc.exe', r'D:\msys64\mingw64\bin\gcc.exe']:
                if Path(candidate).is_file():
                    compiler = candidate
                    break
        if not compiler:
            arm = Path(os.environ.get('XH_C_ARM_CC', r'D:\arm-none-eabi\bin\arm-none-eabi-gcc.exe'))
            self.assertTrue(arm.is_file(), 'Set XH_C_NATIVE_CC or XH_C_ARM_CC to complete execution evidence')
            values = [self.run_arm_generated_c(arm, source) for source in [base, variant]]
            self.assertEqual(values, [13, 7])
            self.assertEqual(hashlib.sha256(COMPILER.read_bytes()).digest(), before)
            return
        env = os.environ.copy()
        env['PATH'] = str(Path(compiler).resolve().parent) + os.pathsep + env.get('PATH', '')
        values = []
        for source in [base, variant]:
            exe = source.with_suffix('.exe')
            process = subprocess.run([compiler, '-w', '-std=gnu99', str(source), '-lm', '-o', str(exe)],
                                     cwd=ROOT, env=env, capture_output=True, text=True, timeout=90)
            self.assertEqual(process.returncode, 0, process.stderr)
            values.append(subprocess.run([str(exe)], env=env, capture_output=True, timeout=30).returncode)
        self.assertEqual(values, [13, 7])
        self.assertEqual(hashlib.sha256(COMPILER.read_bytes()).digest(), before)

    def run_arm_generated_c(self, compiler, source):
        # Build the actual emitted C with the installed GNU ARM compiler. Dead
        # libc wrapper sections are discarded; execute ark_main's arithmetic
        # with real ARM instructions using Unicorn. No replacement C source.
        sys.path.insert(0, str(ROOT / 'build/test-deps'))
        from elftools.elf.elffile import ELFFile
        from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB
        from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0
        elf_path = source.with_suffix('.elf')
        process = subprocess.run([str(compiler), '-w', '-Wno-error=implicit-function-declaration', '-std=gnu99', '-O1', '-mcpu=cortex-m3',
                                  '-mthumb', '-ffunction-sections', '-fdata-sections', '-nostdlib',
                                  '-Wl,--gc-sections,-Ttext=0x10000,-e,ark_main', str(source), '-lc', '-lgcc',
                                  '-o', str(elf_path)], cwd=ROOT, capture_output=True, text=True, timeout=90)
        self.assertEqual(process.returncode, 0, process.stderr)
        with elf_path.open('rb') as stream:
            elf = ELFFile(stream)
            uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
            uc.mem_map(0x10000, 0x40000)
            uc.mem_map(0x700000, 0x20000)
            for segment in elf.iter_segments():
                if segment['p_type'] == 'PT_LOAD' and segment['p_filesz']:
                    uc.mem_write(segment['p_vaddr'], segment.data())
            uc.reg_write(UC_ARM_REG_SP, 0x71f000)
            uc.reg_write(UC_ARM_REG_LR, 0x4f001)
            uc.emu_start(elf.header['e_entry'] | 1, 0x4f000, count=100000)
            return uc.reg_read(UC_ARM_REG_R0) & 0xffffffff

    def test_hexadecimal_escape_preserves_following_hex_digit(self):
        original = self.source.read_text(encoding='utf-8')
        try:
            # Octal in input fixes the two bytes at AB,41. The generated C uses
            # a hexadecimal escape and must terminate it before the literal A.
            self.source.write_text(r'int main(void) { unsigned char *s = (unsigned char*)"\253A"; return s[0] == 171 && s[1] == 65 ? 73 : 9; }' + '\n', encoding='utf-8')
            output = self.compile(self.original, 'hex_boundary')
            self.assertIn(r'\xab""A', output.read_text(encoding='utf-8'))
            arm = Path(os.environ.get('XH_C_ARM_CC', r'D:\arm-none-eabi\bin\arm-none-eabi-gcc.exe'))
            self.assertTrue(arm.is_file(), 'Set XH_C_ARM_CC for generated C execution')
            self.assertEqual(self.run_arm_generated_c(arm, output), 73)
        finally:
            self.source.write_text(original, encoding='utf-8')

    def test_version_malformed_and_missing_rule_rejected(self):
        invalid = [
            self.original.replace('XHTEMPLATE 1', 'XHTEMPLATE 999', 1),
            self.original + '\nrule invalid\ntext \\q\nend\n',
            self.original + '\nrule invalid\ntext ${16}\nend\n',
            self.original + '\nrule invalid\ntext ${oops}\nend\n',
            self.original + '\nrule invalid\ntext trailing\\\nend\n',
            self.original + '\nrule file_header\ntext duplicate\nend\n',
            self.original.replace('rule tok_plus\n', 'rule renamed_plus\n', 1),
            self.original.replace('rule ir_push_int_value\n', 'rule renamed_push\n', 1),
            'XHTEMPLATE 1\n',
        ]
        for index, text in enumerate(invalid):
            with self.subTest(index=index):
                self.compile(text, 'invalid_' + str(index), success=False)

    def test_abi_contract_and_policy_rules(self):
        for key, value in [('abi_pointer_bits', '64'), ('abi_slot_bytes', '4'),
                           ('abi_frame_align', '3'), ('abi_varargs_offset', '7999'),
                           ('policy_frame_min', '8192'), ('policy_stack_margin', '0'),
                           ('policy_return_buffer_bytes', '99999999999999999999')]:
            with self.subTest(key=key):
                bad = re.sub(r'(rule ' + key + r'\ntext )[^\n]*', lambda m: m[1] + value, self.original)
                self.compile(bad, 'abi_invalid', success=False)
        changed = self.original.replace('rule policy_frame_min\ntext 8704',
                                        'rule policy_frame_min\ntext 9000')
        changed = changed.replace('rule policy_stack_min\ntext 256',
                                  'rule policy_stack_min\ntext 300')
        output = self.compile(changed, 'policy').read_text(encoding='utf-8')
        self.assertIn('char frame[9000]', output)
        self.assertIn('ark_slot stk[300]', output)

    def test_native_swprintf_wrapper_passes_capacity_before_format(self):
        compiler = os.environ.get('XH_C_NATIVE_CC') or shutil.which('gcc')
        if not compiler:
            for candidate in [r'D:\msys64\ucrt64\bin\gcc.exe', r'C:\msys64\ucrt64\bin\gcc.exe']:
                if Path(candidate).is_file():
                    compiler = candidate
                    break
        if not compiler:
            self.skipTest('native GCC required for platform wide printf wrapper')
        original = self.source.read_text(encoding='utf-8')
        try:
            self.source.write_text('int main(void) { return 0; }\n', encoding='utf-8')
            generated = self.compile(self.original, 'native_swprintf')
            # Exercise the actual externally loaded wrapper with host-owned
            # pointers, independently of the frontend's fixed pointer layout.
            text = generated.read_text(encoding='utf-8').replace('int main(int argc, char **argv)',
                                                                'int generated_entry(int argc, char **argv)')
            text += '\nint main(void) { wchar_t buffer[16] = {0}; long long n = ark_swprintf((long long)(intptr_t)buffer,16,(long long)(intptr_t)L"%lld",37,0,0,0,0); return n==2 && buffer[0]==L\'3\' && buffer[1]==L\'7\' && buffer[2]==0 ? 0 : 91; }\n'
            generated.write_text(text, encoding='utf-8')
            env = os.environ.copy()
            env['PATH'] = str(Path(compiler).resolve().parent) + os.pathsep + env.get('PATH', '')
            exe = generated.with_suffix('.exe')
            result = subprocess.run([compiler, '-w', '-std=gnu99', str(generated), '-lm', '-o', str(exe)],
                                    cwd=ROOT, env=env, capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], env=env, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
        finally:
            self.source.write_text(original, encoding='utf-8')

    def test_bitfield_selection_changes_signed_extraction_same_sha(self):
        original = self.source.read_text(encoding='utf-8')
        before = hashlib.sha256(COMPILER.read_bytes()).digest()
        try:
            self.source.write_text('struct Bits { signed int value:4; unsigned int tail:28; }; '
                                   'int main(void) { struct Bits b; b.value=-8; b.tail=7; return b.value<0 ? 41 : 9; }\n', encoding='utf-8')
            base = self.compile(self.original, 'bitfield_signed')
            variant = self.original.replace('bitfield LOAD 32 s seq.bitfield.LOAD.32.s 1 1',
                                            'bitfield LOAD 32 s seq.bitfield.LOAD.32.u 1 1', 1)
            self.assertNotEqual(variant, self.original)
            changed = self.compile(variant, 'bitfield_unsigned')
            arm = Path(os.environ.get('XH_C_ARM_CC', r'D:\arm-none-eabi\bin\arm-none-eabi-gcc.exe'))
            self.assertEqual([self.run_arm_generated_c(arm, output) for output in [base, changed]], [41, 9])
            self.assertEqual(hashlib.sha256(COMPILER.read_bytes()).digest(), before)
        finally:
            self.source.write_text(original, encoding='utf-8')

    def test_bitfield_full_width_and_last_bit_execute(self):
        original = self.source.read_text(encoding='utf-8')
        try:
            self.source.write_text('struct Full { signed int s:32; }; struct Last { unsigned int pad:31; unsigned int top:1; }; '
                                   'int main(void) { struct Full f; struct Last b; f.s=-2147483647-1; b.pad=0; b.top=1; '
                                   'return f.s<0 && b.top==1 ? 73 : 9; }\n', encoding='utf-8')
            output = self.compile(self.original, 'bitfield_boundaries')
            arm = Path(os.environ.get('XH_C_ARM_CC', r'D:\arm-none-eabi\bin\arm-none-eabi-gcc.exe'))
            self.assertEqual(self.run_arm_generated_c(arm, output), 73)
        finally:
            self.source.write_text(original, encoding='utf-8')

    def test_named_aggregate_self_pointer_is_finite(self):
        original = self.source.read_text(encoding='utf-8')
        try:
            self.source.write_text('struct Node { int value; struct Node *next; }; '
                                   'struct Node root; int main(void) { root.value=37; return root.value; }\n', encoding='utf-8')
            output = self.compile(self.original, 'named_self_pointer')
            text = output.read_text(encoding='utf-8')
            self.assertIn('struct Node *next', text)
            self.assertLess(len(text), 100000)
        finally:
            self.source.write_text(original, encoding='utf-8')

    def test_type_production_selection_changes_real_object_size_same_sha(self):
        original = self.source.read_text(encoding='utf-8')
        before = hashlib.sha256(COMPILER.read_bytes()).digest()
        try:
            self.source.write_text('char grammar_object = 7; int main(void) { return grammar_object; }\n', encoding='utf-8')
            base = self.compile(self.original, 'type_base')
            variant = self.original.replace('type CHAR 64 s seq.type.CHAR 0 0',
                                            'type CHAR 64 s seq.type.INT 0 0', 1)
            self.assertNotEqual(variant, self.original)
            changed = self.compile(variant, 'type_variant')
            self.assertIn('char grammar_object', base.read_text(encoding='utf-8'))
            self.assertIn('int grammar_object', changed.read_text(encoding='utf-8'))
            arm = Path(os.environ.get('XH_C_ARM_CC', r'D:\arm-none-eabi\bin\arm-none-eabi-gcc.exe'))
            sizes = []
            sys.path.insert(0, str(ROOT / 'build/test-deps'))
            from elftools.elf.elffile import ELFFile
            for output in [base, changed]:
                self.assertEqual(self.run_arm_generated_c(arm, output), 7)
                with output.with_suffix('.elf').open('rb') as stream:
                    elf = ELFFile(stream)
                    sizes.append(elf.get_section_by_name('.symtab').get_symbol_by_name('grammar_object')[0]['st_size'])
            self.assertEqual(sizes, [1, 4])
            self.assertEqual(hashlib.sha256(COMPILER.read_bytes()).digest(), before)
        finally:
            self.source.write_text(original, encoding='utf-8')

    def test_discarded_wide_values_execute_balanced_loop(self):
        original = self.source.read_text(encoding='utf-8')
        try:
            self.source.write_text('long long wide(void) { return 4294967297LL; } '
                                   'double real_value(void) { return 1.5; } '
                                   'void no_result(void) { } '
                                   'int main(void) { int i; for(i=0;i<5;i++) { '
                                   'wide(); real_value(); no_result(); '
                                   '(void)wide(); (void)real_value(); (void)no_result(); '
                                   '(void)4294967297LL; (void)1.5; } return i+70; }\n', encoding='utf-8')
            output = self.compile(self.original, 'discard_loop')
            arm = Path(os.environ.get('XH_C_ARM_CC', r'D:\arm-none-eabi\bin\arm-none-eabi-gcc.exe'))
            self.assertEqual(self.run_arm_generated_c(arm, output), 75)
        finally:
            self.source.write_text(original, encoding='utf-8')

    def test_external_call_argument_cast_changes_execution_same_sha(self):
        original = self.source.read_text(encoding='utf-8')
        before = hashlib.sha256(COMPILER.read_bytes()).digest()
        try:
            # Execution below uses a user function pointer and a void call;
            # changing the integer argument cast affects an observable value.
            self.source.write_text('void touch(int x) { } int id(long long x) { return x == 4294967295LL ? 41 : 9; } int main(void) { int (*p)(long long) = id; touch(3); return p(4294967295LL); }\n', encoding='utf-8')
            base = self.compile(self.original, 'call_base')
            variant = self.original.replace('callarg llong 64 s seq.callarg.llong.s 0 0',
                                            'callarg llong 64 s seq.callarg.integer.s 0 0', 1)
            self.assertNotEqual(variant, self.original)
            changed = self.compile(variant, 'call_variant')
            arm = Path(os.environ.get('XH_C_ARM_CC', r'D:\arm-none-eabi\bin\arm-none-eabi-gcc.exe'))
            self.assertEqual([self.run_arm_generated_c(arm, output) for output in [base, changed]], [41, 9])
            self.assertEqual(hashlib.sha256(COMPILER.read_bytes()).digest(), before)
        finally:
            self.source.write_text(original, encoding='utf-8')

    def test_memory_selector_changes_signed_load_execution_same_sha(self):
        original = self.source.read_text(encoding='utf-8')
        before = hashlib.sha256(COMPILER.read_bytes()).digest()
        try:
            self.source.write_text('int main(void) { int a = -1; return a < 0 ? 41 : 9; }\n', encoding='utf-8')
            base = self.compile(self.original, 'memory_signed')
            variant = self.original.replace('frame LOAD 4 s seq.frame.LOAD.4.s 0 1',
                                            'frame LOAD 4 s seq.frame.LOAD.4.u 0 1', 1)
            self.assertNotEqual(variant, self.original)
            changed = self.compile(variant, 'memory_unsigned')
            arm = Path(os.environ.get('XH_C_ARM_CC', r'D:\arm-none-eabi\bin\arm-none-eabi-gcc.exe'))
            self.assertEqual([self.run_arm_generated_c(arm, output) for output in [base, changed]], [41, 9])
            self.assertEqual(hashlib.sha256(COMPILER.read_bytes()).digest(), before)
        finally:
            self.source.write_text(original, encoding='utf-8')

    def test_semantic_selector_records_reject_invalid_and_missing(self):
        row = 'binary ADD 32 s seq.binary.ADD.32.s 2 1'
        variants = [
            self.original.replace('CSELECT 1', 'CSELECT 999', 1),
            self.original.replace(row, row + r'\n' + row, 1),
            self.original.replace(row, row.replace(' 2 1', ' 0 1'), 1),
            self.original.replace(row, row.replace('seq.binary.ADD.32.s', 'missing.sequence'), 1),
            self.original.replace(row, row.replace('ADD 32', 'UNKNOWN 32'), 1),
            self.original.replace(row, row.replace(' 32 ', ' 999 '), 1),
            self.original.replace('text ' + row + r'\n' + '\n', '', 1),
        ]
        for index, bad in enumerate(variants):
            with self.subTest(index=index):
                self.assertNotEqual(bad, self.original)
                self.compile(bad, 'selector_invalid', success=False)

    def test_placeholder_and_output_bounds_fail(self):
        # Caller supplies only two numeric args. A valid VM placeholder that is
        # outside this call's arity must fail when the instruction is emitted.
        bad = self.original.replace('text stk[sp++].i = ${0}LL;',
                                    'text stk[sp++].i = ${15}LL;', 1)
        self.assertNotEqual(bad, self.original)
        self.compile(bad, 'arity', success=False)
        # A long but valid template must fail the adapter's explicit output
        # capacity instead of silently truncating the generated C instruction.
        bad = self.original.replace('text stk[sp++].i = ${0}LL;',
                                    'text ' + 'x' * 17000 + '${0}', 1)
        self.compile(bad, 'capacity', success=False)


BACKEND_HARNESS = r'''
#include "config.h"
#include "diag.h"
#include "xh_template.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/backend/c/backend_c.c"
ArkccConfig g_config;
int ark_strlen(const char*s){return (int)strlen(s);}
int ark_streq(const char*a,const char*b){return a&&b&&!strcmp(a,b);}
int type_size(Type*t){return t?t->size:0;}
int fs_file_size(const char*p){FILE*f=fopen(p,"rb");long n;if(!f)return -1;fseek(f,0,SEEK_END);n=ftell(f);fclose(f);return(int)n;}
int fs_read_file(const char*p,char*b,int cap){FILE*f=fopen(p,"rb");int n;if(!f)return -1;n=(int)fread(b,1,cap-1,f);b[n]=0;fclose(f);return n;}
void diag_tool(DiagSeverity sev,const char*who,const char*msg,const char*a0,const char*a1,const char*a2,const char*a3,const char*a4,const char*a5){fprintf(stderr,"%s: %s\n",who,msg);}
void asmbuf_init(AsmBuffer*b){b->buf=malloc(100000);b->cap=100000;b->len=0;b->buf[0]=0;}
void asmbuf_puts(AsmBuffer*b,const char*s){int n=strlen(s);if(b->len+n+1>b->cap)abort();memcpy(b->buf+b->len,s,n+1);b->len+=n;}
void asmbuf_putc(AsmBuffer*b,char c){char s[2]={c,0};asmbuf_puts(b,s);}
void asmbuf_int(AsmBuffer*b,long long v){char s[32];snprintf(s,sizeof s,"%lld",v);asmbuf_puts(b,s);}
int main(int argc,char**argv){
 IrProgram prog={0};IrFunc f={0};IrFunc*funcs[1]={&f};IrInst inst={0};static char out[100000];int n;
 prog.funcs=funcs;prog.func_count=1;f.name="entry";f.code=&inst;f.code_count=1;
 inst.op=IR_PUSH_INT;inst.val=7;
 if(!strcmp(argv[1],"binary_slot64")){
  IrInst code[4]={{0}};code[0].op=IR_PUSH_INT;code[0].val=4294967296LL;
  code[1].op=IR_PUSH_INT;code[1].val=1;code[2].op=IR_BINARY;code[2].aux=TOK_SHR|0x100|IR_AUX_SLOT64;
  code[3].op=IR_RETURN;f.name="main";f.code=code;f.code_count=4;
  n=c_emit(&prog,out,sizeof out);if(n<0)return 1;fputs(out,stdout);return 0;
 }
 if(!strcmp(argv[1],"unreachable_flags")){
  IrInst code[4]={{0}};code[0].op=IR_JMP;code[0].val=1;code[1].op=IR_BINARY;code[1].aux=TOK_PLUS|0x200;
  code[2].op=IR_LABEL;code[2].val=1;code[3].op=IR_FUNC_END;f.code=code;f.code_count=4;
  out[0]='X';n=c_emit(&prog,out,sizeof out);printf("%d %d\n",n,(int)out[0]);return n<0&&out[0]==0?0:1;
 }
 if(!strcmp(argv[1],"neutral_div")||!strcmp(argv[1],"neutral_rem")){
  IrInst code[4]={{0}};code[0].op=IR_PUSH_INT;code[0].val=-1;
  code[1].op=IR_PUSH_INT;code[1].val=2;code[2].op=IR_BINARY;
  code[2].aux=(!strcmp(argv[1],"neutral_div")?TOK_DIV:TOK_MOD)|0x100;
  code[3].op=IR_RETURN;f.name="main";f.code=code;f.code_count=4;
  n=c_emit(&prog,out,sizeof out);if(n<0)return 1;fputs(out,stdout);return 0;
 }
 if(!strcmp(argv[1],"manylabels")){
  IrInst *code=calloc(601,sizeof(*code));char names[300][16];int j,maxdepth;
  if(xh_c_rules_begin())return 2;
  for(j=0;j<300;j++){snprintf(names[j],sizeof names[j],"node%d",j);code[j*2].op=IR_JMP_NAMED;code[j*2].name=names[j];code[j*2+1].op=IR_LABEL_NAMED;code[j*2+1].name=names[j];}
  code[600].op=IR_FUNC_END;f.code=code;f.code_count=601;maxdepth=compute_max_sp(&prog,&f);free(code);printf("%d\n",maxdepth);return 0;
 }
 if(!strncmp(argv[1],"cfg_",4)){
  IrInst code[8]={{0}};int count=0,maxdepth;
  if(xh_c_rules_begin())return 2;
  if(!strcmp(argv[1],"cfg_balanced")){
   code[0].op=IR_PUSH_INT;code[1].op=IR_JZ;code[1].val=1;
   code[2].op=IR_JMP;code[2].val=2;code[3].op=IR_LABEL;code[3].val=1;
   code[4].op=IR_LABEL;code[4].val=2;code[5].op=IR_PUSH_INT;code[6].op=IR_RETURN;count=7;
  }else if(!strcmp(argv[1],"cfg_loop")){
   code[0].op=IR_LABEL;code[0].val=0;code[1].op=IR_PUSH_INT;code[2].op=IR_JZ;code[2].val=1;
   code[3].op=IR_JMP;code[3].val=0;code[4].op=IR_LABEL;code[4].val=1;count=5;
  }else if(!strcmp(argv[1],"cfg_underflow")){code[0].op=IR_POP;count=1;}
  else if(!strcmp(argv[1],"cfg_join")){
   code[0].op=IR_PUSH_INT;code[1].op=IR_JZ;code[1].val=1;code[2].op=IR_PUSH_INT;code[3].op=IR_LABEL;code[3].val=1;count=4;
  }else if(!strcmp(argv[1],"cfg_cycle")){
   code[0].op=IR_LABEL;code[0].val=1;code[1].op=IR_PUSH_INT;code[2].op=IR_JMP;code[2].val=1;count=3;
  }else if(!strcmp(argv[1],"cfg_missing")){code[0].op=IR_JMP;code[0].val=99;count=1;}
  else if(!strcmp(argv[1],"cfg_named")){
   code[0].op=IR_JMP_NAMED;code[0].name="end";code[1].op=IR_POP;code[2].op=IR_LABEL_NAMED;code[2].name="end";count=3;
  }else if(!strcmp(argv[1],"cfg_peek")){code[0].op=IR_CMP_JMP_EQ;code[0].aux=1;code[1].op=IR_LABEL;code[1].val=1;count=2;}
  else if(!strcmp(argv[1],"cfg_duplicate")){code[0].op=IR_LABEL;code[0].val=0;code[1].op=IR_LABEL;code[1].val=0;count=2;}
  else return 3;
  f.code=code;f.code_count=count;maxdepth=compute_max_sp(&prog,&f);printf("%d\n",maxdepth);return 0;
 }
 if(!strcmp(argv[1],"calls")){
  int a,b,c,d,e,g;Type rt={0};if(xh_c_rules_begin())return 2;
  inst.op=IR_CALL;inst.name="entry";inst.val=3;rt.kind=TYPE_VOID;f.ret_type=&rt;
  a=ir_stack_delta_ctx(&prog,&inst);
  rt.kind=TYPE_INT;b=ir_stack_delta_ctx(&prog,&inst);
  inst.name="conj";inst.val=1;c=ir_stack_delta_ctx(&prog,&inst);
  inst.name="?";inst.val=2;d=ir_stack_delta_ctx(&prog,&inst);
  inst.name="entry";inst.val=0;e=ir_stack_delta_ctx(&prog,&inst);
  inst.val=16;g=ir_stack_delta_ctx(&prog,&inst);
  printf("%d %d %d %d %d %d\n",a,b,c,d,e,g);return a==-3&&b==-2&&c==0&&d==-1&&e==1&&g==-15?0:1;
 }
 if(!strcmp(argv[1],"effects")){
  int a,b,c,d;if(xh_c_rules_begin())return 2;
  inst.op=IR_TRUNC;a=ir_stack_delta(&inst);
  inst.op=IR_BINARY;inst.aux=TOK_PLUS;b=ir_stack_delta(&inst);
  inst.op=IR_FADD;inst.aux=0x900;c=ir_stack_delta(&inst);
  inst.op=IR_UNARY;inst.aux=TOK_MINUS;inst.val=0;d=ir_stack_delta(&inst);
  printf("%d %d %d %d\n",a,b,c,d);return a==0&&b==-1&&c==0&&d==0?0:1;
 }
 if(!strcmp(argv[1],"ir"))inst.op=(IrOp)99999;
 else if(!strcmp(argv[1],"call17")){inst.op=IR_CALL;inst.name="entry";inst.val=17;}
 else if(!strcmp(argv[1],"callnull")){inst.op=IR_CALL;inst.name=0;inst.val=0;}
 else if(!strcmp(argv[1],"indirect0")){inst.op=IR_CALL;inst.name="?";inst.val=0;}
 else if(!strcmp(argv[1],"bitzero")){inst.op=IR_LOAD_FLD;inst.val=0;inst.aux=0;}
 else if(!strcmp(argv[1],"bitwide")){inst.op=IR_STORE_FLD;inst.val=0;inst.aux=33;}
 else if(!strcmp(argv[1],"bitoffset")){inst.op=IR_LOAD_FLD;inst.val=31;inst.aux=2;}
 else if(!strcmp(argv[1],"bitflags")){inst.op=IR_LOAD_FLD;inst.val=0;inst.aux=0x401;}
 else if(!strcmp(argv[1],"simdflags")){inst.op=IR_SIMD_ADD8;inst.aux=1;}
 else if(!strcmp(argv[1],"binary")){inst.op=IR_BINARY;inst.aux=99999;}
 else if(!strcmp(argv[1],"binary_floatflag")){inst.op=IR_BINARY;inst.aux=TOK_PLUS|0x200;}
 else if(!strcmp(argv[1],"binary_complexflag")){inst.op=IR_BINARY;inst.aux=TOK_PLUS|0x800;}
 else if(!strcmp(argv[1],"unary")){inst.op=IR_UNARY;inst.aux=99999;}
 else if(!strcmp(argv[1],"compare")){inst.op=IR_FCMP;inst.aux=99999;}
 else if(!strcmp(argv[1],"compare64")){inst.op=IR_64CMP;inst.aux=99999;}
 else if(!strcmp(argv[1],"typecycle")||!strcmp(argv[1],"typedepth")){
  Type ty[70]={{0}};IrGlobal g={0};IrGlobal*globals[1]={&g};int j;
  for(j=0;j<69;j++){ty[j].kind=TYPE_PTR;ty[j].base=&ty[j+1];}ty[69].kind=TYPE_INT;
  if(!strcmp(argv[1],"typecycle"))ty[0].base=&ty[0];
  g.name="limited";g.type=ty;prog.globals=globals;prog.global_count=1;out[0]='X';n=c_emit(&prog,out,sizeof out);
  printf("%d %d\n",n,(int)out[0]);return n<0&&out[0]==0?0:1;
 }
 else if(!strcmp(argv[1],"pointer")||!strcmp(argv[1],"array")){
  Type ty[18]={{0}};IrGlobal g={0};IrGlobal*globals[1]={&g};char name[300];int i;
  memset(name,'a',sizeof name-1);name[sizeof name-1]=0;ty[17].kind=TYPE_INT;
  for(i=16;i>=0;i--){ty[i].kind=TYPE_ARRAY;ty[i].base=&ty[i+1];ty[i].size=1;}
  if(!strcmp(argv[1],"pointer")){ty[0].kind=TYPE_PTR;ty[0].base=&ty[17];g.name=name;}else g.name="many_dims";
  g.type=ty;prog.globals=globals;prog.global_count=1;out[0]='X';n=c_emit(&prog,out,sizeof out);
  printf("%d %d\n",n,(int)out[0]);return n<0&&out[0]==0?0:1;
 }
 out[0]='X';n=c_emit(&prog,out,sizeof out);printf("%d %d\n",n,(int)out[0]);
 return n<0&&out[0]==0?0:1;
}
'''

class CBackendRejections(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sdk = Path(os.environ.get('WASI_SDK_PATH', r'D:\wasi-sdk-34.0-x86_64-windows'))
        cls.wasmtime = os.environ.get('WASMTIME', r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
        cls.out = ROOT / 'build/c-rules-tests'
        cls.out.mkdir(parents=True, exist_ok=True)
        harness = cls.out / 'backend_harness.c'
        harness.write_text(BACKEND_HARNESS, encoding='utf-8')
        cls.wasm = cls.out / 'backend_harness.wasm'
        env = os.environ.copy()
        python_dir = Path(sys.executable).parent
        env['PATH'] = os.pathsep.join([str(python_dir), str(python_dir / 'Library/bin'), r'D:\miniconda3\Library\bin', env.get('PATH', '')])
        process = subprocess.run([str(sdk / 'bin/clang.exe'), '--target=wasm32-wasip1', '-O1',
                                  '-I.', '-Iinclude', '-Iconfig', '-include', 'include/xh_small.h', '-Wl,-z,stack-size=262144',
                                  str(harness), 'src/backend/rules/c_rules.c', 'src/backend/rules/template.c', 'src/backend/rules/semantic.c', 'src/backend/rules/target_rules.c',
                                  'src/ir/ir_contract_legacy.c', 'src/ir/ir_contract.c', '-ffunction-sections', '-Wl,--gc-sections',
                                  '-o', str(cls.wasm)], cwd=ROOT, env=env, capture_output=True, text=True, timeout=90)
        if process.returncode:
            raise AssertionError(process.stderr)

    def test_neutral_unsigned_division_remainder_mask_sign_extended_slots(self):
        for mode, expected in [('neutral_div', 2147483647), ('neutral_rem', 1), ('binary_slot64', 2147483648)]:
            with self.subTest(mode=mode):
                result = subprocess.run([self.wasmtime, 'run', '--dir=.', str(self.wasm), mode],
                                        cwd=ROOT, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr)
                source = self.out / (mode + '.c')
                source.write_text(result.stdout, encoding='utf-8')
                arm = Path(os.environ.get('XH_C_ARM_CC', r'D:\arm-none-eabi\bin\arm-none-eabi-gcc.exe'))
                self.assertEqual(CRules.run_arm_generated_c(self, arm, source), expected)

    def test_trunc_and_selected_variant_stack_effects(self):
        result = subprocess.run([self.wasmtime, 'run', '--dir=.', str(self.wasm), 'effects'],
                                cwd=ROOT, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), '0 -1 0 0')

    def test_cfg_worklist_rejects_bad_depths_and_resolves_labels(self):
        expected = {'manylabels': 1, 'cfg_balanced': 1, 'cfg_named': 1, 'cfg_loop': 1, 'cfg_underflow': -1, 'cfg_join': -1,
                    'cfg_cycle': -1, 'cfg_missing': -1, 'cfg_peek': -1, 'cfg_duplicate': -1}
        for mode, depth in expected.items():
            with self.subTest(mode=mode):
                result = subprocess.run([self.wasmtime, 'run', '--dir=.', str(self.wasm), mode],
                                        cwd=ROOT, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stdout.strip(), str(depth))
                if depth < 0:
                    self.assertRegex(result.stderr, r'c backend:|template expansion exceeds output capacity')

    def test_dynamic_call_stack_effects_use_real_return_shape(self):
        result = subprocess.run([self.wasmtime, 'run', '--dir=.', str(self.wasm), 'calls'],
                                cwd=ROOT, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), '-3 -2 0 -1 1 -15')

    def test_invalid_ir_operator_and_type_limits_reject_without_output(self):
        for mode in ['ir', 'binary', 'binary_floatflag', 'binary_complexflag', 'unreachable_flags', 'unary', 'compare', 'compare64', 'pointer', 'array', 'call17', 'callnull', 'indirect0', 'typecycle', 'typedepth', 'bitzero', 'bitwide', 'bitoffset', 'bitflags', 'simdflags']:
            with self.subTest(mode=mode):
                result = subprocess.run([self.wasmtime, 'run', '--dir=.', str(self.wasm), mode],
                                        cwd=ROOT, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stdout.strip(), '-1 0')
                self.assertRegex(result.stderr, r'c backend:|template expansion exceeds output capacity')


if __name__ == '__main__':
    unittest.main()
