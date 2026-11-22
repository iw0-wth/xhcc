"""A copied package containing a Wasm module can register a new command."""
from pathlib import Path
import shutil,subprocess,sys,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class HostTests(unittest.TestCase):
 def test_directory_code_extension(self):
  with tempfile.TemporaryDirectory(prefix='host-',dir=ROOT/'build') as td:
   root=Path(td);package=root/'second-arm';shutil.copytree(ROOT/'packages/arm-cortex-m3',package)
   p=package/'package.ini';p.write_text(p.read_text().replace('id=arm-cortex-m3','id=second-arm').replace('arm-none-eabi-gcc,armcc','second-gcc'))
   output=root/'copy.o'
   r=subprocess.run([sys.executable,str(ROOT/'tools/xhcc.py'),'--package-root',str(root),'second-gcc','-c','examples/add.c','-o',output.relative_to(ROOT).as_posix()],cwd=ROOT,capture_output=True,text=True)
   self.assertEqual(r.returncode,0,r.stdout+r.stderr);self.assertEqual(output.read_bytes()[:4],b'\x7fELF')
   p.write_text(p.read_text().replace('module=compiler.wasm','module=../escape.wasm'))
   r=subprocess.run([sys.executable,str(ROOT/'tools/xhcc.py'),'--package-root',str(root),'--list-tools'],cwd=ROOT,capture_output=True,text=True)
   self.assertNotEqual(r.returncode,0)
if __name__=='__main__':unittest.main()
