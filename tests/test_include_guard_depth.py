"""Guarded reentry at the include budget, without increasing that budget."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from test_stream_preproc import HARNESS, ROOT


class IncludeGuardDepthTests(unittest.TestCase):
    def run_suite(self, native):
        sdk = Path(os.environ.get('WASI_SDK_PATH', r'D:\wasi-sdk-34.0-x86_64-windows'))
        cc = (os.environ.get('XH_NATIVE_CLANG') or shutil.which('clang') or r'D:\msys64\ucrt64\bin\clang.exe') if native else str(sdk/'bin/clang.exe')
        runtime = shutil.which('wasmtime') or r'C:\Program Files\Wasmtime\bin\wasmtime.exe'
        env = os.environ.copy()
        py = Path(sys.executable).parent
        env['PATH'] = os.pathsep.join([str(Path(cc).parent), str(py), str(py/'Library/bin'), env.get('PATH', '')])
        # Print the dependency list after preprocessing, including skipped paths.
        harness = HARNESS.replace('fclose(f);printf(', 'fclose(f);{const char *deps[256];int i,n=preproc_get_included_files(deps,256);for(i=0;i<n;i++)fprintf(stderr,"DEP:%s\\n",deps[i]);}printf(')
        with tempfile.TemporaryDirectory(prefix='guard-depth-', dir=ROOT/'build') as tmp:
            d = Path(tmp)
            src = d/'harness.c'
            binary = d/('pp.exe' if native else 'pp.wasm')
            src.write_text(harness, encoding='utf8')
            cmd = [cc]+([] if native else ['--target=wasm32-wasip1'])+['-std=c99','-Oz','-DXHCC','-DPP_MAX_INC_DEPTH=8','-DPP_INC_BUF=32768','-DPP_BUF_SIZE=32768','-I'+str(ROOT/'include'),str(src),str(ROOT/'src/frontend/c/preproc.c'),str(ROOT/'src/frontend/c/source_reader.c'),str(ROOT/'src/platform/wasi/flash_fs_host.c'),'-o',str(binary)]
            if not native: cmd += ['-Wl,-z,stack-size=131072']
            r = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout+r.stderr)

            def write(name, text):
                (d/name).write_text(text, encoding='utf8')

            def run(text, success=True):
                write('input.c', text)
                cmd = ([str(binary)] if native else [runtime,'run','--dir=.',str(binary)])+['stream','input.c','output.txt']
                r = subprocess.run(cmd, cwd=d, env=env, capture_output=True, text=True, timeout=30)
                self.assertEqual(r.returncode == 0, success, r.stdout+r.stderr)
                return (d/'output.txt').read_text(encoding='utf8'), r.stderr

            # Header is completed before the eight-frame chain attempts reentry.
            write('guard.h', '/* license */\n#ifndef GUARD_H\n#define GUARD_H\nint guarded_body;\n#endif\n/* tail */\n')
            for i in range(8):
                write(f'level{i}.h', f'#include "level{i+1}.h"\n' if i<7 else '#include "./guard.h"\nint deepest_tail;\n')
            out, err = run('#include "guard.h"\n#include "level0.h"\n')
            self.assertEqual(out.count('guarded_body'), 1)
            self.assertIn('deepest_tail', out)
            self.assertIn('DEP:./guard.h', err)
            # Current macro state is consulted: undef forces actual processing.
            out, _ = run('#include "guard.h"\n#undef GUARD_H\n#include "guard.h"\n')
            self.assertEqual(out.count('guarded_body'), 2)
            # A genuinely new ninth file still fails.
            write('level7.h', '#include "new.h"\n')
            write('new.h', 'int ninth_file;\n')
            run('#include "level0.h"\n', False)
            # False guards must never suppress tokens after the wrapper or else.
            invalid = [
                '#ifndef GUARD_H\n#define GUARD_H\nint inside;\n#endif\nint outside;\n',
                '#ifndef GUARD_H\n#define GUARD_H\nint inside;\n#else\nint alternate;\n#endif\n',
                '#if !defined(GUARD_H)\n#define GUARD_H\nint inside;\n#endif\n',
                '#ifndef GUARD_H\n#define OTHER_H\nint inside;\n#endif\n',
                '#ifndef GUARD_H\n#define GUARD_H\n#endif extra\n',
            ]
            write('level7.h', '#include "guard.h"\n')
            for text in invalid:
                write('guard.h', text)
                run('#include "guard.h"\n#include "level0.h"\n', False)
            # Body directive length does not affect the whole-file wrapper.
            # This reproduces obj.h's long macro definition (first at line 388).
            for edge in (158, 159, 160, 161, 1023, 1024, 1100):
                header = ('#ifndef GUARD_H\n#define GUARD_H\n#define LONG_BODY '+('x '*edge)+
                          '\n#if 1'+(' '*edge)+'\nint long_guard_body;\n#endif\n#endif /*'+('c'*1100)+'*/\n')
                write('guard.h', header)
                out, _ = run('#include "guard.h"\n#include "level0.h"\n')
                self.assertEqual(out.count('long_guard_body'), 1)
                out, _ = run('#include "guard.h"\n#undef GUARD_H\n#include "guard.h"\n')
                self.assertEqual(out.count('long_guard_body'), 2)
            mpy = ROOT.parent/'sd/micropython/py/obj.h'
            if mpy.exists():
                definitions = '\n'.join(line for line in mpy.read_text(encoding='utf8').splitlines() if line.startswith('#define MP_DEFINE_CONST_OBJ_TYPE_NARGS_') or line.startswith('#define MP_OBJ_FUN_MAKE_SIG'))
                write('guard.h', '#ifndef GUARD_H\n#define GUARD_H\n'+definitions+'\nint real_macro_body;\n#endif\n')
                out, _ = run('#include "guard.h"\n#include "level0.h"\n')
                self.assertEqual(out.count('real_macro_body'), 1)
            # Long outer endif trailing tokens cannot be hidden by the prefix cap.
            write('guard.h', '#ifndef GUARD_H\n#define GUARD_H\n#endif'+(' '*200)+'extra\n')
            run('#include "guard.h"\n#include "level0.h"\n', False)
            # First-read self recursion stays conservative until EOF validates it.
            write('self.h', '#ifndef SELF_H\n#define SELF_H\n#include "self.h"\n#endif\n')
            out, _ = run('#include "self.h"\n')
            self.assertNotIn('ninth_file', out)
            # Nested conditionals and quoted comment markers do not fool observer.
            write('guard.h', '\ufeff/* first */\n#ifndef GUARD_H\n#define GUARD_H\n#if 1\nchar *s="/* not comment */";\n#endif\n#endif\n')
            out, _ = run('#include "guard.h"\n#include "level0.h"\n')
            self.assertEqual(out.count('not comment'), 1)

    def test_native(self): self.run_suite(True)
    def test_wasi(self): self.run_suite(False)


if __name__ == '__main__': unittest.main()
