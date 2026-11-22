"""Build xhcc using WASI SDK; no make or shell dependency."""
from pathlib import Path
import argparse, os, shutil, subprocess, sys
# An installed Python distribution may supply the VC runtime needed by clang.
if os.name == 'nt':
    python_dir = Path(sys.executable).parent
    os.environ['PATH'] = os.pathsep.join([str(python_dir), str(python_dir/'Library/bin'), os.environ.get('PATH','')])
ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--sdk', default=os.environ.get('WASI_SDK_PATH', r'D:\wasi-sdk-34.0-x86_64-windows'))
p.add_argument('--cc', help='Override compiler (for native builds)')
p.add_argument('--native', action='store_true')
p.add_argument('--verbose', action='store_true')
p.add_argument('--wasm-validate', default=os.environ.get('WASM_VALIDATE') or shutil.which('wasm-validate') or (r'D:\wabt-1.0.42\bin\wasm-validate.exe' if os.name=='nt' else 'wasm-validate'))
p.add_argument('--wasm-opt', default=os.environ.get('WASM_OPT') or shutil.which('wasm-opt') or (r'D:\binaryen-version_133\bin\wasm-opt.exe' if os.name=='nt' else 'wasm-opt'))
a = p.parse_args()
cc = a.cc or str(Path(a.sdk) / 'bin' / ('clang.exe' if os.name == 'nt' else 'clang'))
out = ROOT / 'build' / ('native' if a.native else 'wasm')
out.mkdir(parents=True, exist_ok=True)
sources = sorted((ROOT/'src').rglob('*.c')) + sorted((ROOT/'object').glob('*.c'))
flags = ['-std=c99', '-Oz', '-g0', '-ffunction-sections', '-fdata-sections', '-include', str(ROOT/'include/xh_small.h'), '-I'+str(ROOT/'include'), '-I'+str(ROOT/'object'), '-Wno-unused-value', '-Wno-incompatible-pointer-types']
if not a.native:
    flags += ['--target=wasm32-wasip1', '-mno-bulk-memory', '-mno-sign-ext', '-mno-nontrapping-fptoint', '-mno-simd128', '-mno-atomics']
objects = []
for source in sources:
    obj = out / (str(source.relative_to(ROOT)).replace('\\','_').replace('/','_') + '.o')
    cmd = [cc, *flags, '-c', str(source), '-o', str(obj)]
    if a.verbose: print(' '.join(cmd), flush=True)
    result = subprocess.run(cmd)
    if result.returncode: sys.exit(result.returncode)
    objects.append(str(obj))
output = out / ('xhcc.exe' if a.native and os.name=='nt' else 'xhcc' if a.native else 'xhcc.wasm')
link = [cc, *objects, '-o', str(output)]
if not a.native:
    link += ['--target=wasm32-wasip1', '-Wl,--gc-sections', '-Wl,-z,stack-size=131072', '-Wl,--initial-memory=3145728', '-Wl,--max-memory=3145728', '-Wl,-Map='+str(out/'xhcc.map')]
result = subprocess.run(link)
if result.returncode: sys.exit(result.returncode)
if not a.native:
    lowered = out/'xhcc.lowered.wasm'
    cmd = [a.wasm_opt, str(output), '-Oz', '--signext-lowering', '--llvm-nontrapping-fptoint-lowering', '--llvm-memory-copy-fill-lowering', '--disable-reference-types', '--disable-multivalue', '--disable-extended-const', '--disable-bulk-memory', '--disable-bulk-memory-opt', '--disable-sign-ext', '--disable-nontrapping-float-to-int', '-o', str(lowered)]
    result = subprocess.run(cmd)
    if result.returncode: sys.exit(result.returncode)
    result = subprocess.run([a.wasm_validate, '--disable-bulk-memory', '--disable-reference-types', '--disable-multi-value', '--disable-sign-extension', '--disable-saturating-float-to-int', '--disable-simd', '--disable-extended-const', '--disable-tail-call', '--disable-memory64', '--disable-multi-memory', str(lowered)])
    if result.returncode: sys.exit(result.returncode)
    # Keep the linked artifact for diagnosis; atomically install the lowered module.
    shutil.copy2(output, out/'xhcc.sdk.wasm')
    os.replace(lowered, output)
    shutil.copy2(output, ROOT/'packages/arm-cortex-m3/compiler.wasm')
print(output)
