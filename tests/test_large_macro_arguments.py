"""Bounded aggregate macro arguments, using standalone native/WASI frontend."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from test_stream_preproc import HARNESS, ROOT


class LargeMacroArgumentsTests(unittest.TestCase):
    def run_suite(self, native):
        sdk = Path(os.environ.get('WASI_SDK_PATH', r'D:\wasi-sdk-34.0-x86_64-windows'))
        cc = (os.environ.get('XH_NATIVE_CLANG') or shutil.which('clang') or r'D:\msys64\ucrt64\bin\clang.exe') if native else str(sdk/'bin/clang.exe')
        runtime = shutil.which('wasmtime') or r'C:\Program Files\Wasmtime\bin\wasmtime.exe'
        env = os.environ.copy()
        env['PATH'] = os.pathsep.join([str(Path(cc).parent), str(Path(sys.executable).parent), str(Path(sys.executable).parent/'Library/bin'), env.get('PATH', '')])
        # Format diagnostics so failures identify the actual inner macro.
        harness = HARNESS.replace('(void)a;(void)b;(void)c;if(sev>=DIAG_SEV_ERROR){errors++;fprintf(stderr,"%s:%d: %s\\n",l->file?l->file:"?",l->line,m);}', '(void)c;if(sev>=DIAG_SEV_ERROR){errors++;fprintf(stderr,"%s:%d: ",l->file?l->file:"?",l->line);fprintf(stderr,m,a,b);fputc(10,stderr);}')
        with tempfile.TemporaryDirectory(prefix='large-args-', dir=ROOT/'build') as tmp:
            d = Path(tmp)
            src = d/'harness.c'
            binary = d/('pp.exe' if native else 'pp.wasm')
            src.write_text(harness, encoding='utf8')
            cmd = [cc] + ([] if native else ['--target=wasm32-wasip1']) + ['-std=c99', '-DXHCC', '-DPP_INC_BUF=32768', '-DPP_BUF_SIZE=32768', '-I'+str(ROOT/'include'), str(src), str(ROOT/'src/frontend/c/preproc.c'), str(ROOT/'src/frontend/c/source_reader.c'), str(ROOT/'src/platform/wasi/flash_fs_host.c'), '-o', str(binary)]
            build = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout+build.stderr)

            def run(text, mode='stream', success=True):
                (d/'input.c').write_text(text, encoding='utf8')
                cmd = ([str(binary)] if native else [runtime, 'run', '--dir=.', str(binary)]) + [mode, 'input.c', 'output.txt']
                result = subprocess.run(cmd, cwd=d, env=env, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode == 0, success, result.stdout+result.stderr)
                return (d/'output.txt').read_text(), result.stderr, result.stdout

            for count in (38, 40, 64):
                params = ','.join('p'+str(i) for i in range(count))
                values = ','.join(str(i) for i in range(count))
                text = f'#define SELECT({params}) p0 + p{count-1}\nSELECT({values})\n'
                for mode in ('legacy', 'stream'):
                    out, _, _ = run(text, mode)
                    self.assertIn(f'0 + {count-1}', out)
                # The final empty argument still counts as the 64th argument.
                out, _, _ = run(f'#define SELECT({params}) p0 + p{count-1}\nSELECT({values.rsplit(",",1)[0]},)\n')
                self.assertIn('0 + ', out)
            _, err, _ = run('#define MANY(...) __VA_ARGS__\nMANY('+','.join('x' for _ in range(65))+')\n', success=False)
            self.assertIn("macro 'MANY'", err)
            self.assertIn('64 arguments', err)
            # Exact upstream selector and grammar-padding definitions, read only.
            mpy = ROOT.parent/'sd/micropython/py'
            if mpy.exists():
                lines = (mpy/'obj.h').read_text(encoding='utf8').splitlines()
                names = ('MP_DEFINE_CONST_OBJ_TYPE_EXPAND(', 'MP_DEFINE_CONST_OBJ_TYPE_NARGS(', 'MP_DEFINE_CONST_OBJ_TYPE(...)')
                definitions = '\n'.join(line for line in lines if line.startswith('#define ') and any(name in line for name in names))
                out, _, _ = run(definitions+'\n#define MP_DEFINE_CONST_OBJ_TYPE_NARGS_5(...) selected_five\nMP_DEFINE_CONST_OBJ_TYPE(mp_type_int, MP_QSTR_int, FLAGS, make_new, f, print, g, unary_op, h, binary_op, i, locals_dict, j)\n')
                self.assertIn('selected_five', out)
                lines = (mpy/'parse.c').read_text(encoding='utf8').splitlines()
                definitions = '\n'.join(line for line in lines if line.startswith('#define RULE_PADDING') or line.startswith('#define RULE_EXPAND'))
                out, _, _ = run(definitions+'\nRULE_PADDING(decorated, rule(decorators), rule(decorated_body))\n')
                self.assertIn('PAD1_decorated', out)
            payload = 'word '*650
            for mode in ('legacy', 'stream'):
                out, _, _ = run('#define ID(x) x\nID('+payload+')\n', mode)
                self.assertIn(payload.rstrip(), out)
                out, _, _ = run('#define ID(x) x\n#define BOTH(a,b) ID(a) + ID(b)\nBOTH(first,second)\n', mode)
                self.assertIn('first + second', out)
                out, _, _ = run('#define SECOND(a,b) b\nSECOND("a,\\\"b)", (4,5))\n', mode)
                self.assertIn('(4,5)', out)
            # Legacy has no logical-unit cap: exercise aggregate overflow itself.
            _, err, _ = run('#define ID(x) x\nID('+'x'*8192+')\n', 'legacy', False)
            self.assertIn('8192 bytes of aggregate storage', err)
            _, err, _ = run('#define ID(x) x\nID('+'x'*8192+')\n', success=False)
            self.assertIn('8190 bytes', err)
            # Replacement growth can overflow aggregate storage below file-unit cap.
            text = '#define BIG '+('x '*1500)+'\n#define DROP(...) 0\n#define PASS(...) DROP(__VA_ARGS__)\nPASS(BIG,BIG,BIG)\n'
            _, err, _ = run(text, success=False)
            self.assertTrue('aggregate storage' in err or 'expands past 8192 bytes' in err, err)
            # Type-only compatibility must match each target's size_t width.
            for target in ('arm', 'c', 'riscv32', 'x86', 'x86_64'):
                header = ROOT/'packages'/target/'include/unistd.h'
                check = d/'types.c'
                check.write_text('#include <unistd.h>\n#include <unistd.h>\ntypedef char signed_check[((ssize_t)-1 < 0) ? 1 : -1];\ntypedef char width_check[(sizeof(ssize_t)==sizeof(size_t)) ? 1 : -1];\n', encoding='utf8')
                result = subprocess.run([cc, '-std=c99', '-fsyntax-only', '-nostdinc', '-I'+str(header.parent), str(check)], env=env, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_native(self):
        self.run_suite(True)

    def test_wasi(self):
        self.run_suite(False)


if __name__ == '__main__':
    unittest.main()
