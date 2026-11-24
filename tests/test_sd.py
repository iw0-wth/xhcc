"""Run installed commands in a single preopened simulated NuttX SD root."""
from pathlib import Path
import argparse,os,subprocess,sys,hashlib
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--sd',type=Path,default=ROOT.parent/'sd/xhcc');a=p.parse_args();sd=a.sd.resolve()
runtime=os.environ.get('WASMTIME',r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
def run(app,*args,success=True):
 result=subprocess.run([runtime,'run','--dir=.', '--env','XHCC_PACKAGE_ROOT=packages',str(sd/(app+'.wasm')),*args],cwd=sd,capture_output=True,text=True,timeout=60)
 if (result.returncode==0)!=success:raise AssertionError(result.stdout+result.stderr)
 return result
for app in ['xhcc','xhccm','xhccn','xhlinker']:assert (sd/(app+'.wasm')).is_file()
assert (sd/'packages/arm/arm.rules').is_file()
run('xhcc','arm-none-eabi-gcc','-c','examples/make-demo/first.c','-o','examples/make-demo/direct.o')
assert (sd/'examples/make-demo/direct.o').read_bytes()[:4]==b'\x7fELF'
# Root package path remains absolute in the guest when make enters a project.
base=[runtime,'run','--dir=.', '--env','XHCC_PACKAGE_ROOT=/packages',str(sd/'xhccm.wasm')]
def make(*args):
 r=subprocess.run([*base,'make','-C','examples/make-demo',*args],cwd=sd,capture_output=True,text=True,timeout=60)
 if r.returncode:raise AssertionError(r.stdout+r.stderr)
 return r
make('all')
paths=[sd/'examples/make-demo'/name for name in ['first.o','second.o']]
for path in paths:assert path.read_bytes()[:4]==b'\x7fELF'
before=[(path.stat().st_mtime_ns,hashlib.sha256(path.read_bytes()).digest()) for path in paths]
make('all');after=[(path.stat().st_mtime_ns,hashlib.sha256(path.read_bytes()).digest()) for path in paths];assert before==after,'incremental make rewrote objects'
make('-B','CC=riscv32-unknown-elf-gcc','CFLAGS=-O0','all')
for path in paths:assert int.from_bytes(path.read_bytes()[18:20],'little')==243
make('-f','Makefile.extended','-B','MODE=rv32','all')
for path in paths:assert int.from_bytes(path.read_bytes()[18:20],'little')==243
tracked=[path.stat().st_mtime_ns for path in paths]
make('-f','Makefile.extended','MODE=rv32','all');assert tracked==[path.stat().st_mtime_ns for path in paths],'order-only phony prerequisite triggered rebuild'
make('-f','Makefile.link','-B','all')
linked=sd/'examples/make-demo/firmware.elf';image=linked.read_bytes()
assert image[:4]==b'\x7fELF' and int.from_bytes(image[16:18],'little')==2,'standard executable ELF expected'
assert int.from_bytes(image[18:20],'little')==40 and int.from_bytes(image[24:28],'little')&1,'ARM Thumb entry expected'
linked_before=(linked.stat().st_mtime_ns,hashlib.sha256(image).digest())
make('-f','Makefile.link','all');assert linked_before==(linked.stat().st_mtime_ns,hashlib.sha256(linked.read_bytes()).digest()),'incremental make rewrote executable'
run('xhccn','arm-none-eabi-gcc','-c','examples/make-demo/entry.c','-o','examples/make-demo/no-link.o')
run('xhccn','arm-none-eabi-gcc','examples/make-demo/no-link.o','-o','examples/make-demo/forbidden.elf',success=False)
run('xhlinker','--entry','entry','--text-address','0x08004000','--data-address','0x20002000','examples/make-demo/first.o','examples/make-demo/second.o','examples/make-demo/entry.o','-o','examples/make-demo/standalone.elf')
assert (sd/'examples/make-demo/standalone.elf').read_bytes()[:4]==b'\x7fELF'
run('xhlinker','-T','examples/make-demo/memory.ld','--oformat=binary','examples/make-demo/first.o','examples/make-demo/second.o','examples/make-demo/entry.o','-o','examples/make-demo/firmware.bin')
assert (sd/'examples/make-demo/firmware.bin').stat().st_size>0
print('PASS SD four applications, shared packages, ARM direct/make/link/script/binary, incremental make, injected RV32 CC')
