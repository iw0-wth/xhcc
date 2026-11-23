"""Content-addressed C object cache used by build.py; no compilation semantics.

Cache keys cover compiler bytes/version, complete TU flags, source path/content,
all project .h/.inc files, SDK header contents, and compiler-affecting environment.
Artifacts retain their existing build output paths (link maps stay readable).
"""
from pathlib import Path
import hashlib,json,os,shutil,subprocess,tempfile

ENVIRONMENT_KEYS=('CPATH','C_INCLUDE_PATH','CPLUS_INCLUDE_PATH','OBJC_INCLUDE_PATH',
 'INCLUDE','SDKROOT','MACOSX_DEPLOYMENT_TARGET','SOURCE_DATE_EPOCH','LANG','LC_ALL',
 'CL','_CL_','CCC_OVERRIDE_OPTIONS','GCC_EXEC_PREFIX','COMPILER_PATH')

def sha(data):return hashlib.sha256(data).hexdigest()

def compiler_identity(compiler):
 resolved=shutil.which(str(compiler)) or str(compiler)
 path=Path(resolved).resolve(strict=True)
 result=subprocess.run([str(path),'--version'],capture_output=True,text=True,check=True)
 return {'path':str(path),'sha256':sha(path.read_bytes()),'version':result.stdout+result.stderr}

def header_snapshot(project,sdk):
 project=Path(project).resolve();sdk=Path(sdk).resolve()
 paths=set(p for p in project.rglob('*') if p.is_file() and p.suffix.lower() in ('.h','.inc') and not any(x in ('build','.git','packages') for x in p.relative_to(project).parts))
 # Package .h files aren't compiler inputs; frontend includes come from include/.
 # SDK system headers are implicit clang dependencies and need an independent key.
 for root in [sdk/'share/wasi-sysroot/include',sdk/'lib/clang']:
  if root.is_dir():paths.update(p for p in root.rglob('*') if p.is_file() and p.suffix.lower() in ('.h','.inc','.def'))
 records=[(str(p),sha(p.read_bytes())) for p in sorted(paths)]
 return sha(json.dumps(records,separators=(',',':')).encode())

def object_key(source,flags,compiler,headers,environment=None):
 source=Path(source).resolve(strict=True)
 payload={'schema':'xhcc.object-cache.v1','compiler':compiler,'headers':headers,
          'source_path':str(source),'source_sha256':sha(source.read_bytes()),
          'flags':list(flags),'environment':{k:(environment or os.environ).get(k) for k in ENVIRONMENT_KEYS}}
 return sha(json.dumps(payload,sort_keys=True,separators=(',',':')).encode())

def atomic_copy(source,destination):
 destination=Path(destination);destination.parent.mkdir(parents=True,exist_ok=True);temporary=None
 try:
  with tempfile.NamedTemporaryFile(prefix='.xhcc-cache-',dir=destination.parent,delete=False) as stream:temporary=Path(stream.name)
  shutil.copyfile(source,temporary);os.replace(temporary,destination);temporary=None
 finally:
  if temporary is not None:temporary.unlink(missing_ok=True)

class ObjectCache:
 def __init__(self,directory,identity,headers,enabled=True):
  self.directory=Path(directory);self.identity=identity;self.headers=headers;self.enabled=enabled;self.hits=0;self.misses=0
 def compile(self,source,flags,output,run):
  source=Path(source);output=Path(output)
  if not self.enabled:
   run([self.identity['path'],*flags,'-c',str(source),'-o',str(output)]);self.misses+=1;return False
  key=object_key(source,flags,self.identity,self.headers);entry=self.directory/key[:2]/key;obj=entry.with_suffix('.o');metadata=entry.with_suffix('.json')
  try:
   record=json.loads(metadata.read_text(encoding='utf-8'))
   if record.get('key')==key and record.get('sha256')==sha(obj.read_bytes()):
    atomic_copy(obj,output);self.hits+=1;return True
  except (OSError,ValueError,TypeError):pass
  # Compile to the established output path so compiler path-sensitive behavior
  # is identical with and without caching. Failed compiles never publish cache.
  run([self.identity['path'],*flags,'-c',str(source),'-o',str(output)])
  atomic_copy(output,obj);entry.parent.mkdir(parents=True,exist_ok=True)
  with tempfile.NamedTemporaryFile(mode='w',encoding='utf-8',prefix='.xhcc-cache-',dir=entry.parent,delete=False) as stream:
   json.dump({'key':key,'sha256':sha(output.read_bytes())},stream,sort_keys=True);temp=Path(stream.name)
  try:os.replace(temp,metadata)
  finally:temp.unlink(missing_ok=True)
  self.misses+=1;return False
