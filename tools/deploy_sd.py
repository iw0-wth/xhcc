"""Install sibling WASI commands and manifest-managed external data into SD.

Updates replace individual files atomically; installation.json commits last.
Only unchanged stale files named in the previous manifest are removed. User
files and modified stale files are preserved. This is not a multi-file filesystem
transaction: an interruption may require rerunning deployment.
"""
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import json
import os
import re
import tempfile

ROOT=Path(__file__).resolve().parents[1]
EXAMPLES={
 'first.c':'int first(void){return 17;}\n',
 'second.c':'int second(void){return 25;}\n',
 'Makefile':'CC ?= arm-none-eabi-gcc\nCFLAGS ?= -O1 -mcpu=cortex-m3\nOBJECTS = first.o second.o\n.PHONY: all clean\nall: $(OBJECTS)\n%.o: %.c\n\t$(CC) $(CFLAGS) -c $< -o $@\nclean:\n\trm -f $(OBJECTS)\n',
 'Makefile.extended':'MODE ?= arm\nSOURCES = first.c second.c\nOBJECTS := $(addsuffix .o,$(basename $(SOURCES)))\nifeq ($(MODE),rv32)\nCC = riscv32-unknown-elf-gcc\nCFLAGS = -O0\nelse\nCC = arm-none-eabi-gcc\nCFLAGS = -O1 -mcpu=cortex-m3\nendif\n.PHONY: all prepare\nall: $(OBJECTS)\nprepare:\n\tmkdir -p output\n%.o: %.c | prepare\n\t$(CC) $(CFLAGS) -c $< -o $@\n'}

def linked(path):
 return path.is_symlink() or (hasattr(path,'is_junction') and path.is_junction())

def reject_links(path):
 for component in [path,*path.parents]:
  if linked(component):raise ValueError(f'symlink/junction deployment path rejected: {component}')

def relative_name(name):
 if not isinstance(name,str) or not name or '\\' in name or ':' in name or '\x00' in name:
  raise ValueError('invalid managed relative path')
 parts=name.split('/')
 if PurePosixPath(name).is_absolute() or any(p in ('','.','..') or p.rstrip(' .')!=p for p in parts):raise ValueError(f'invalid managed relative path: {name}')
 if name=='installation.json':raise ValueError('manifest cannot manage itself')
 return name

def destination(root,name):
 relative_name(name);path=root.joinpath(*name.split('/'));reject_links(path)
 if not path.resolve().is_relative_to(root):raise ValueError(f'managed path escapes destination: {name}')
 if path.exists() and not path.is_file():raise ValueError(f'managed target is not a regular file: {name}')
 return path

def digest(data):return hashlib.sha256(data).hexdigest()

def read_previous(root):
 path=root/'installation.json';reject_links(path)
 if not path.exists():return {}
 def unique(pairs):
  result={}
  for key,value in pairs:
   if key in result:raise ValueError('duplicate installation manifest key')
   result[key]=value
  return result
 data=json.loads(path.read_text(encoding='utf-8'),object_pairs_hook=unique)
 if not isinstance(data,dict) or set(data)!={'layout','files'} or data['layout']!='xhcc.sd.v1' or not isinstance(data['files'],dict):raise ValueError('invalid installation manifest schema')
 previous=data['files'];folded=set()
 for name,entry in previous.items():
  destination(root,name)
  if name.casefold() in folded:raise ValueError('case-colliding managed paths')
  folded.add(name.casefold())
  if not isinstance(entry,dict) or set(entry)!={'bytes','sha256'} or type(entry['bytes']) is not int or entry['bytes']<0 or not isinstance(entry['sha256'],str) or not re.fullmatch('[0-9a-f]{64}',entry['sha256']):raise ValueError('invalid installation file metadata')
 return previous

def source_files(root):
 files={}
 for app in ['xhcc','xhccm']:
  path=root/'build/wasm'/f'{app}.wasm'
  if not path.is_file():raise ValueError(f'build {app} first: {path}')
  files[path.name]=path.read_bytes()
 for package in sorted((root/'packages').iterdir()):
  if not package.is_dir() or not (package/'package.ini').is_file():continue
  reject_links(package)
  for path in sorted(package.rglob('*')):
   reject_links(path)
   if not path.is_file() or path.suffix.lower() in ('.wasm','.pyc') or '__pycache__' in path.parts:continue
   files['packages/'+path.relative_to(root/'packages').as_posix()]=path.read_bytes()
 for name,text in EXAMPLES.items():files['examples/make-demo/'+name]=text.encode('utf-8')
 return files

def atomic_write(path,data):
 reject_links(path);path.parent.mkdir(parents=True,exist_ok=True);reject_links(path.parent)
 temp=None
 try:
  with tempfile.NamedTemporaryFile(prefix='.xhcc-install-',dir=path.parent,delete=False) as stream:
   temp=Path(stream.name);stream.write(data);stream.flush();os.fsync(stream.fileno())
  reject_links(path);os.replace(temp,path);temp=None
 finally:
  if temp is not None:
   # temp is the exact regular file created by this invocation, in the checked parent.
   reject_links(temp);temp.unlink(missing_ok=True)

def deploy(sd,source_root=ROOT):
 root=Path(os.path.abspath(sd));reject_links(root)
 if root.exists() and not root.is_dir():raise ValueError('SD root is not a directory')
 files=source_files(Path(source_root));previous=read_previous(root)
 folded=set()
 # Validate every new and stale target before the first mutation.
 for name in files:
  destination(root,name)
  if name.casefold() in folded:raise ValueError('case-colliding source paths')
  folded.add(name.casefold())
 stale=[];preserved=[]
 for name,entry in previous.items():
  if name in files:continue
  path=destination(root,name)
  if path.is_file():
   data=path.read_bytes()
   if len(data)==entry['bytes'] and digest(data)==entry['sha256']:stale.append(name)
   else:preserved.append(name)
 manifest={name:{'bytes':len(data),'sha256':digest(data)} for name,data in sorted(files.items())}
 root.mkdir(parents=True,exist_ok=True)
 for name,data in files.items():atomic_write(destination(root,name),data)
 for name in stale:
  path=destination(root,name) # resolved absolute target verified immediately before deletion
  data=path.read_bytes();entry=previous[name]
  if len(data)==entry['bytes'] and digest(data)==entry['sha256']:path.unlink()
  else:preserved.append(name)
 atomic_write(root/'installation.json',(json.dumps({'layout':'xhcc.sd.v1','files':manifest},indent=2)+'\n').encode('utf-8'))
 return {'installed':len(manifest),'removed':stale,'preserved_modified':preserved}

def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--sd',type=Path,default=ROOT.parent/'sd/xhcc');args=parser.parse_args()
 try:result=deploy(args.sd)
 except (ValueError,OSError,json.JSONDecodeError) as error:parser.exit(1,f'deploy_sd: {error}\n')
 print(f"Installed {result['installed']} managed files into {args.sd}; removed {len(result['removed'])} stale files; preserved {len(result['preserved_modified'])} modified stale files")
if __name__=='__main__':main()
