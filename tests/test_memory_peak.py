"""Arena peak accounting survives in-place growth and subsequent release."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MemoryPeakTests(unittest.TestCase):
    def test_in_place_growth_retains_peak_after_free(self):
        sdk = Path(os.environ.get('WASI_SDK_PATH', r'D:\wasi-sdk-34.0-x86_64-windows'))
        compiler = os.environ.get('WASI_CC') or str(sdk / 'bin' / ('clang.exe' if os.name == 'nt' else 'clang'))
        runtime = os.environ.get('WASMTIME') or shutil.which('wasmtime') or r'C:\Program Files\Wasmtime\bin\wasmtime.exe'
        environment = os.environ.copy()
        if os.name == 'nt':
            python_dir = Path(sys.executable).parent
            environment['PATH'] = os.pathsep.join([str(python_dir), str(python_dir/'Library/bin'), environment.get('PATH','')])
        with tempfile.TemporaryDirectory(prefix='memory-peak-', dir=ROOT/'build') as temporary:
            directory = Path(temporary)
            source = directory/'peak.c'
            source.write_text(r'''
#include "memory.h"
#include <stdio.h>
#include <stdlib.h>
void ark_abort(const char *message) { fprintf(stderr,"%s\n",message); exit(90); }
int main(void) {
    unsigned char *allocation, *grown;
    mem_pool_reset();
    allocation = mem_pool_alloc(1024);
    allocation[0] = 17;
    grown = mem_realloc(allocation, 1024, 2048);
    if (grown != allocation || grown[0] != 17 || grown[2047] != 0) return 1;
    if (mem_pool_used() != 2048) return 2;
    mem_pool_free(grown, 2048);
    if (mem_pool_used() != 0) return 3;
    if (mem_pool_high() < 2048) return 4;
    printf("peak=%d used=%d\n",mem_pool_high(),mem_pool_used());
    mem_pool_reset();
    if (mem_pool_high() != 0) return 5;
    return 0;
}
''', encoding='utf-8')
            output = directory/'peak.wasm'
            built = subprocess.run([compiler,'--target=wasm32-wasip1','-std=c99',
                '-I'+str(ROOT/'include'),str(ROOT/'src/core/memory.c'),str(source),
                '-o',str(output)],env=environment,capture_output=True,text=True,timeout=30)
            self.assertEqual(built.returncode,0,built.stdout+built.stderr)
            result = subprocess.run([runtime,'run',str(output)],capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('peak=2048 used=0',result.stdout)


if __name__ == '__main__':
    unittest.main()
