"""Install two sibling WASI commands and one shared backend directory into SD."""
from pathlib import Path
import argparse,hashlib,json,shutil
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--sd',type=Path,default=ROOT.parent/'sd/xhcc');a=p.parse_args();dest=a.sd.resolve();dest.mkdir(parents=True,exist_ok=True)
for app in ['xhcc','xhccm']:
 source=ROOT/'build/wasm'/(app+'.wasm')
 if not source.is_file():raise SystemExit(f'Build {app} first: {source}')
 shutil.copy2(source,dest/source.name)
# Copy external data only. Never install ignored prototype modules or host files.
for package in sorted((ROOT/'packages').iterdir()):
 if not package.is_dir() or not (package/'package.ini').is_file():continue
 for source in package.rglob('*'):
  if not source.is_file() or source.suffix.lower() in ('.wasm','.pyc') or '__pycache__' in source.parts:continue
  target=dest/'packages'/source.relative_to(ROOT/'packages');target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)
example=dest/'examples/make-demo';example.mkdir(parents=True,exist_ok=True)
(example/'first.c').write_text('int first(void){return 17;}\n',encoding='utf-8')
(example/'second.c').write_text('int second(void){return 25;}\n',encoding='utf-8')
(example/'Makefile').write_text('CC ?= arm-none-eabi-gcc\nCFLAGS ?= -O1 -mcpu=cortex-m3\nOBJECTS = first.o second.o\n.PHONY: all clean\nall: $(OBJECTS)\n%.o: %.c\n\t$(CC) $(CFLAGS) -c $< -o $@\nclean:\n\trm -f $(OBJECTS)\n',encoding='utf-8')
(example/'Makefile.extended').write_text('MODE ?= arm\nSOURCES = first.c second.c\nOBJECTS := $(addsuffix .o,$(basename $(SOURCES)))\nifeq ($(MODE),rv32)\nCC = riscv32-unknown-elf-gcc\nCFLAGS = -O0\nelse\nCC = arm-none-eabi-gcc\nCFLAGS = -O1 -mcpu=cortex-m3\nendif\n.PHONY: all prepare\nall: $(OBJECTS)\nprepare:\n\tmkdir -p output\n%.o: %.c | prepare\n\t$(CC) $(CFLAGS) -c $< -o $@\n',encoding='utf-8')
manifest={}
for file in sorted(dest.rglob('*')):
 if file.is_file() and file.name!='installation.json' and not file.suffix.lower() in ('.o','.d'):
  manifest[file.relative_to(dest).as_posix()]={'bytes':file.stat().st_size,'sha256':hashlib.sha256(file.read_bytes()).hexdigest()}
(dest/'installation.json').write_text(json.dumps({'layout':'xhcc.sd.v1','files':manifest},indent=2),encoding='utf-8')
print(f'Installed {len(manifest)} files into {dest}')
