"""Production file frontend regression tests; never builds the application.

Run after the coordinated frontend freeze/build. Needs the existing WASM,
pyelftools/Unicorn and full Clang, LLD and llvm-ar for ARM/RV CPU tests.
Temporary disk holds the expanded TU; no all-RAM frontend claim is made:
AST/IR/code generation memory still scales with program complexity.
"""
from pathlib import Path
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'build/test-deps'))
COMPILER = Path(os.environ.get('XHCC_WASM', str(ROOT/'build/wasm/xhcc.wasm')))
WASMTIME = os.environ.get('WASMTIME') or shutil.which('wasmtime') or r'C:\Program Files\Wasmtime\bin\wasmtime.exe'
CLANG = os.environ.get('XH_NATIVE_CLANG') or shutil.which('clang') or r'D:\msys64\ucrt64\bin\clang.exe'
SDK = Path(os.environ.get('WASI_SDK_PATH', r'D:\wasi-sdk-34.0-x86_64-windows'))
LLVM_AR = os.environ.get('LLVM_AR') or shutil.which('llvm-ar') or str(SDK/('bin/llvm-ar.exe' if os.name == 'nt' else 'bin/llvm-ar'))


class SpoolStorageTests(unittest.TestCase):
    def test_exclusive_append_finish_range_publish_and_failures(self):
        sdk = Path(os.environ.get('WASI_SDK_PATH', r'D:\wasi-sdk-34.0-x86_64-windows'))
        cc = str(sdk/('bin/clang.exe' if os.name == 'nt' else 'bin/clang'))
        (ROOT/'build').mkdir(exist_ok=True)
        env = os.environ.copy()
        py = Path(sys.executable).parent
        env['PATH'] = os.pathsep.join([str(sdk/'bin'), str(py), str(py/'Library/bin'), env.get('PATH', '')])
        with tempfile.TemporaryDirectory(prefix='spool-api-', dir=ROOT/'build') as tmp:
            directory = Path(tmp)
            source = directory/'spool.c'
            source.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
static int mode, writes;
static size_t short_write(const void*p,size_t s,size_t n,FILE*f){
 if(mode==1 && writes++){errno=ENOSPC;return 0;}
 return fwrite(p,s,n>3?3:n,f);
}
static int fail_flush(FILE*f){if(mode==2){errno=EIO;return -1;}return fflush(f);}
static int fail_close(FILE*f){int r=fclose(f);if(mode==3){errno=EIO;return -1;}return r;}
static int fail_rename(const char*a,const char*b){if(mode==4){errno=EACCES;return -1;}return rename(a,b);}
#define fwrite short_write
#define fflush fail_flush
#define fclose fail_close
#define rename fail_rename
#include "flash_fs_host.c"
#undef fwrite
#undef fflush
#undef fclose
#undef rename
void ark_log(const char*f,...){(void)f;}
int main(int argc,char**argv){
 FsSpool s={0};unsigned char b[32];int rc;
 if(argc!=2)return 90;mode=atoi(argv[1]);
 if(fs_spool_create(&s,"readonly.c","output.bin"))return 91;
 rc=fs_spool_append(&s,(const unsigned char*)"complete-new-output",19);
 if(mode==5 && !rc)exit(7); /* open stream, no wrapper return */
 if(!rc)rc=fs_spool_finish(&s);
 if(mode==6 && !rc)exit(7); /* closed stream still owns temporary */
 if(!rc && mode!=4){
  if(fs_spool_range(&s,9,b,3)!=3||memcmp(b,"new",3))return 92;
  if(fs_spool_rewind(&s)||fs_spool_read(&s,b,32)!=19)return 93;
 }
 if(!rc)rc=fs_spool_publish(&s,"output.bin");
 fs_spool_destroy(&s);
 if(mode>=1&&mode<=4)return rc<0?0:94;
 return rc?95:0;
}
''', encoding='utf-8')
            binary = directory/'spool.wasm'
            built = subprocess.run([cc, '--target=wasm32-wasip1', '-std=c99', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror',
                                    '-I'+str(ROOT/'include'), '-I'+str(ROOT/'src/platform/wasi'),
                                    str(source), '-o', str(binary)], env=env,
                                   capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout+built.stderr)
            collision = directory/'output.bin.xhcc-tmp-0-1'
            collision.write_bytes(b'user-owned collision')
            for mode in (0, 1, 2, 3, 4, 5, 6):
                output = directory/'output.bin'
                output.write_bytes(b'old-output')
                ran = subprocess.run([WASMTIME, 'run', '--dir=.', str(binary), str(mode)],
                                     cwd=directory, capture_output=True, text=True, timeout=30)
                self.assertEqual(ran.returncode, 7 if mode in (5, 6) else 0, ran.stdout+ran.stderr)
                self.assertEqual(output.read_bytes(), b'old-output' if mode else b'complete-new-output')
                self.assertEqual(collision.read_bytes(), b'user-owned collision')
                self.assertEqual(list(directory.glob('*.xhcc-tmp-*')), [collision])


class StreamCompileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        (ROOT/'build').mkdir(exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='stream-compile-', dir=ROOT/'build')
        cls.directory = Path(cls.temp.name)
        cls.digest = hashlib.sha256(COMPILER.read_bytes()).digest()

    @classmethod
    def tearDownClass(cls):
        try:
            if hashlib.sha256(COMPILER.read_bytes()).digest() != cls.digest:
                raise AssertionError('test unexpectedly changed production compiler')
        finally:
            cls.temp.cleanup()

    def command(self, args, success=True, env=None):
        child_env = (os.environ if env is None else env).copy()
        python_dir = Path(sys.executable).parent
        child_env['PATH'] = os.pathsep.join([
            str(python_dir), str(python_dir/'Library/bin'), str(SDK/'bin'),
            child_env.get('PATH', '')])
        result = subprocess.run(list(map(str, args)), cwd=ROOT, env=child_env,
                                capture_output=True, timeout=180)
        self.assertEqual(result.returncode == 0, success,
                         result.stdout.decode(errors='replace') + result.stderr.decode(errors='replace'))
        return result

    def cc(self, triple, *args, success=True):
        env = os.environ.copy()
        env['XHCC_TMPDIR'] = str(self.directory.relative_to(ROOT))
        result = self.command([sys.executable, 'tools/xhcc.py', '--compiler', COMPILER,
                               '--wasmtime', WASMTIME, triple+'-gcc', *args], success, env)
        self.assertFalse(list(self.directory.glob('*.xhcc-tmp-*')), 'temporary spool leaked')
        return result

    def relative(self, path):
        return path.relative_to(ROOT)

    def fixture(self, name, data):
        path = self.directory/name
        path.write_bytes(data)
        return self.relative(path)

    def test_large_preprocessed_atomic_output_and_failure(self):
        source = self.fixture('expanded.c', b'/*' + b'x'*(1024*1024+83) +
                              b'*/\n' + b'\n'*40000 + b'int tail(void){return 73;}\n')
        output = self.directory/'expanded.i'
        self.cc('arm-none-eabi', '-E', source, '-o', self.relative(output))
        expanded = output.read_bytes()
        self.assertGreater(len(expanded), 1024*1024)
        self.assertIn(b'int tail(void){return 73;}', expanded)
        stdout = self.cc('arm-none-eabi', '-E', source).stdout
        self.assertEqual(stdout, expanded)
        old = b'previous valid output\x00\xff'
        for name, bad in [('embedded-zero.c', b'int ok;\n\x00int hidden;\n'),
                          ('bad.c', b'#error explicit failure\n'),
                          ('too-long.c', b'#define EXCESS '+b'x'*10000+b'\n')]:
            path = self.fixture(name, bad)
            output.write_bytes(old)
            result = self.cc('arm-none-eabi', '-E', path, '-o', self.relative(output), success=False)
            self.assertEqual(output.read_bytes(), old)
            self.assertTrue(result.stderr)
            if name == 'embedded-zero.c':
                self.assertIn(b'NUL', result.stderr)
                self.assertIn(b'2', result.stderr)

    def test_window_edges_forced_includes_dependencies_and_small_equivalence(self):
        forced = self.fixture('forced.h', b'#define FORCED 9\n')
        for edge in (1019, 1023, 1024, 2047):
            body = (b'#define CAT(a,b) a##b\n#define PLUS(x) ((x)+FORCED)\n' +
                    b' '*edge + b'\nchar *s="escaped\\\" string";\n'
                    b'int CAT(tai,l)(void){return PLUS(64);}\n')
            source = self.fixture('edge.c', body)
            plain = self.fixture('plain.c', body.replace(b' '*edge, b''))
            a, b = self.directory/'edge.o', self.directory/'plain.o'
            dep = self.directory/'edge.d'
            self.cc('arm-none-eabi', '-O0', '-include', forced, '-DFROM_ARG=1',
                    '-MMD', '-MF', self.relative(dep), '-c', source, '-o', self.relative(a))
            self.cc('arm-none-eabi', '-O0', '-include', forced, '-DFROM_ARG=1',
                    '-c', plain, '-o', self.relative(b))
            from elftools.elf.elffile import ELFFile
            def code(path):
                return ELFFile(io.BytesIO(path.read_bytes())).get_section_by_name('.text').data()
            self.assertEqual(code(a), code(b))
            self.assertIn(b'forced.h', dep.read_bytes())

    def test_large_main_and_include_tail_execute_arm_and_rv_mixed_archive(self):
        from elftools.elf.elffile import ELFFile
        from unicorn import Uc, UC_ARCH_ARM, UC_ARCH_RISCV, UC_MODE_THUMB, UC_MODE_MCLASS, UC_MODE_RISCV32
        from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0
        from unicorn.riscv_const import UC_RISCV_REG_SP, UC_RISCV_REG_RA, UC_RISCV_REG_A0
        self.fixture('huge.h', b'/*'+b'h'*(1024*1024+29)+b'*/\n#define HEADER_TAIL 61\nextern int library_value(int);\n')
        source = self.fixture('huge.c', b'/*'+b'm'*(1024*1024+17)+b'*/\n'+b'\n'*40000+
                              b'#include "huge.h"\nint tail(void){return library_value(HEADER_TAIL)+5;}\n')
        library = self.fixture('library.c', b'int library_value(int x){return x+7;}\n')
        unused = self.fixture('unused.c', b'extern int unavailable(void);\nint unused_member(void){return unavailable();}\n')
        for triple, target, cpu_args, arch, mode, regs in [
            ('arm-none-eabi', 'arm-none-eabi', ['-mcpu=cortex-m3', '-mthumb'], UC_ARCH_ARM,
             UC_MODE_THUMB | UC_MODE_MCLASS, (UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0)),
            ('riscv32-unknown-elf', 'riscv32-unknown-elf', ['-march=rv32im', '-mabi=ilp32', '-mno-relax'],
             UC_ARCH_RISCV, UC_MODE_RISCV32, (UC_RISCV_REG_SP, UC_RISCV_REG_RA, UC_RISCV_REG_A0))]:
            obj = self.directory/(triple+'.o')
            # The modern default file frontend must reach both huge tails.
            self.cc(triple, '-O0', '-c', source, '-o', self.relative(obj))
            elf = ELFFile(io.BytesIO(obj.read_bytes()))
            self.assertEqual(elf['e_type'], 'ET_REL')
            names = {s.name for s in elf.get_section_by_name('.symtab').iter_symbols()}
            self.assertIn('tail', names)
            members = []
            for src in (library, unused):
                member = self.directory/(triple+'-'+src.stem+'.o')
                self.command([CLANG, '--target='+target, *cpu_args, '-O0', '-ffreestanding',
                              '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
                              '-c', src, '-o', member])
                members.append(member)
            archive = self.directory/(triple+'.a')
            self.command([LLVM_AR, 'rcs', archive, *members])
            image = self.directory/(triple+'.elf')
            lld = os.environ.get('LD_LLD') or shutil.which('ld.lld') or str(SDK/('bin/ld.lld.exe' if os.name == 'nt' else 'bin/ld.lld'))
            self.command([lld, '-e', 'tail', '-Ttext=0x10000', obj, archive, '-o', image])
            view = ELFFile(io.BytesIO(image.read_bytes()))
            names = {s.name for s in view.get_section_by_name('.symtab').iter_symbols()}
            self.assertNotIn('unused_member', names)
            uc = Uc(arch, mode)
            pages = set()
            loads = [s for s in view.iter_segments() if s['p_type'] == 'PT_LOAD']
            for segment in loads:
                pages.update(range(segment['p_vaddr'] & ~4095,
                                   (segment['p_vaddr']+segment['p_memsz']+4095) & ~4095, 4096))
            for page in pages:
                uc.mem_map(page, 4096)
            for segment in loads:
                if segment['p_filesz']:
                    uc.mem_write(segment['p_vaddr'], segment.data())
            uc.mem_map(0x21000000, 0x10000)
            uc.mem_map(0x0fff0000, 4096)
            uc.reg_write(regs[0], 0x2100fff0)
            uc.reg_write(regs[1], 0x0fff0001 if arch == UC_ARCH_ARM else 0x0fff0000)
            uc.emu_start(view['e_entry'], 0x0fff0000, count=100000)
            self.assertEqual(uc.reg_read(regs[2]) & 0xffffffff, 73)


if __name__ == '__main__':
    unittest.main()
