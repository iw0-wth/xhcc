"""Object cache correctness using small actual WASI SDK translation units."""
from pathlib import Path
import importlib.util,os,subprocess,sys,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('build_cache',ROOT/'tools/build_cache.py');cachemodule=importlib.util.module_from_spec(spec);spec.loader.exec_module(cachemodule)
SDK=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'))
class BuildCache(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  if os.name=='nt':
   pd=Path(sys.executable).parent;os.environ['PATH']=str(pd)+os.pathsep+str(pd/'Library/bin')+os.pathsep+os.environ.get('PATH','')
  cls.identity=cachemodule.compiler_identity(SDK/'bin/clang.exe')
 def test_hits_invalidation_and_corrupt_cache_recompile(self):
  with tempfile.TemporaryDirectory(prefix='xh-cache-') as tmp:
   root=Path(tmp);(root/'include').mkdir();header=root/'include/value.h';header.write_text('#define VALUE 17\n');source=root/'probe.c';source.write_text('#include "value.h"\nint value(void){return VALUE;}\n');output=root/'probe.o';flags=['--target=wasm32-wasip1','-Oz','-g0','-I'+str(root/'include')];calls=[]
   def run(cmd):calls.append(cmd);p=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr)
   def current():return cachemodule.ObjectCache(root/'cache',self.identity,cachemodule.header_snapshot(root,root/'no-sdk'))
   cache=current();self.assertFalse(cache.compile(source,flags,output,run));original=output.read_bytes();self.assertTrue(cache.compile(source,flags,root/'second.o',run));self.assertEqual(len(calls),1);self.assertEqual((root/'second.o').read_bytes(),original)
   header.write_text('#define VALUE 19\n');cache=current();self.assertFalse(cache.compile(source,flags,output,run));self.assertNotEqual(output.read_bytes(),original)
   source.write_text('#include "value.h"\nint value(void){return VALUE+1;}\n');self.assertFalse(cache.compile(source,flags,output,run))
   self.assertFalse(cache.compile(source,[*flags,'-DCHANGED=1'],output,run))
   changed_identity=dict(self.identity,version=self.identity['version']+' fixturechange');other=cachemodule.ObjectCache(root/'cache',changed_identity,cache.headers);self.assertFalse(other.compile(source,flags,output,run))
   # Corrupt exactly the entry for the next query; digest validation must miss.
   key=cachemodule.object_key(source,flags,self.identity,cache.headers);entry=root/'cache'/key[:2]/(key+'.o');entry.write_bytes(b'corrupt');before=len(calls);self.assertFalse(cache.compile(source,flags,output,run));self.assertEqual(len(calls),before+1);self.assertNotEqual(output.read_bytes(),b'corrupt')
 def test_cache_and_no_cache_objects_are_identical(self):
  with tempfile.TemporaryDirectory(prefix='xh-cache-same-') as tmp:
   root=Path(tmp);source=root/'probe.c';source.write_text('int add(int a,int b){return a+b;}\n');flags=['--target=wasm32-wasip1','-Oz','-g0','-ffunction-sections'];headers=cachemodule.header_snapshot(root,root/'no-sdk')
   def run(cmd):subprocess.run(cmd,check=True,capture_output=True)
   cached=cachemodule.ObjectCache(root/'cache',self.identity,headers);uncached=cachemodule.ObjectCache(root/'cache',self.identity,headers,False);output=root/'probe.o';cached.compile(source,flags,output,run);first=output.read_bytes();cached.compile(source,flags,output,run);self.assertEqual(output.read_bytes(),first);uncached.compile(source,flags,output,run);self.assertEqual(output.read_bytes(),first);self.assertEqual(cached.hits,1);self.assertEqual(uncached.misses,1)
if __name__=='__main__':unittest.main()
