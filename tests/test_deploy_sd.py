"""Manifest-managed SD updates preserve unmanaged/modified user files."""
from pathlib import Path
import importlib.util,json,tempfile,unittest,hashlib,os
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('deploy_sd',ROOT/'tools/deploy_sd.py');deployment=importlib.util.module_from_spec(spec);spec.loader.exec_module(deployment)
class DeploySd(unittest.TestCase):
 def fixture(self,base):
  source=base/'source';(source/'build/wasm').mkdir(parents=True);(source/'packages/demo').mkdir(parents=True)
  for app in ['xhcc','xhccm']:(source/'build/wasm'/f'{app}.wasm').write_bytes(b'\0asm-'+app.encode())
  (source/'packages/demo/package.ini').write_text('id=demo\n');(source/'packages/demo/current.rules').write_text('current');(source/'packages/demo/old.rules').write_text('old');(source/'packages/demo/ignored.wasm').write_bytes(b'ignored')
  return source,base/'sd'
 def test_update_removes_only_unchanged_old_managed_files(self):
  with tempfile.TemporaryDirectory() as tmp:
   source,sd=self.fixture(Path(tmp));deployment.deploy(sd,source);(sd/'user.txt').write_text('user');(sd/'examples/make-demo/output.o').write_bytes(b'object');(sd/'packages/demo/modified.rules').write_text('old')
   manifest=json.loads((sd/'installation.json').read_text());manifest['files']['packages/demo/modified.rules']={'bytes':3,'sha256':hashlib.sha256(b'old').hexdigest()};(sd/'installation.json').write_text(json.dumps(manifest));(sd/'packages/demo/modified.rules').write_text('user edited')
   # Explicit verified source path; this fixture removes its old managed input.
   stale_source=source/'packages/demo/old.rules';self.assertEqual(stale_source.resolve(),(source/'packages/demo/old.rules').resolve());stale_source.unlink();(source/'build/wasm/xhcc.wasm').write_bytes(b'new compiler')
   result=deployment.deploy(sd,source);self.assertFalse((sd/'packages/demo/old.rules').exists());self.assertEqual((sd/'user.txt').read_text(),'user');self.assertEqual((sd/'examples/make-demo/output.o').read_bytes(),b'object');self.assertEqual((sd/'packages/demo/modified.rules').read_text(),'user edited');self.assertEqual((sd/'xhcc.wasm').read_bytes(),b'new compiler');self.assertIn('packages/demo/modified.rules',result['preserved_modified']);self.assertFalse(list((sd/'packages').rglob('*.wasm')))
   manifest=json.loads((sd/'installation.json').read_text());self.assertNotIn('user.txt',manifest['files']);self.assertNotIn('packages/demo/modified.rules',manifest['files']);self.assertFalse(list(sd.rglob('.xhcc-install-*')))
 def test_invalid_old_manifest_rejected_before_updates(self):
  with tempfile.TemporaryDirectory() as tmp:
   source,sd=self.fixture(Path(tmp));deployment.deploy(sd,source);original=(sd/'xhcc.wasm').read_bytes();(source/'build/wasm/xhcc.wasm').write_bytes(b'new')
   for name in ['../outside','/absolute','C:/absolute','packages/../outside','packages\\escape','packages//empty','installation.json']:
    with self.subTest(name=name):
     (sd/'installation.json').write_text(json.dumps({'layout':'xhcc.sd.v1','files':{name:{'bytes':0,'sha256':'0'*64}}}));self.assertRaises(ValueError,deployment.deploy,sd,source);self.assertEqual((sd/'xhcc.wasm').read_bytes(),original)
 def test_symlink_destination_rejected(self):
  with tempfile.TemporaryDirectory() as tmp:
   source,sd=self.fixture(Path(tmp));outside=Path(tmp)/'outside';outside.mkdir()
   try:sd.symlink_to(outside,target_is_directory=True)
   except OSError as error:self.skipTest(f'symlink creation unavailable: {error}')
   self.assertRaises(ValueError,deployment.deploy,sd,source);self.assertEqual(list(outside.iterdir()),[])
 def test_windows_junction_destination_rejected(self):
  if os.name!='nt':self.skipTest('Windows junction gate')
  import subprocess
  with tempfile.TemporaryDirectory() as tmp:
   source,sd=self.fixture(Path(tmp));outside=Path(tmp)/'outside';outside.mkdir();result=subprocess.run(['cmd','/c','mklink','/J',str(sd),str(outside)],capture_output=True,text=True)
   self.assertEqual(result.returncode,0,result.stderr);self.assertRaises(ValueError,deployment.deploy,sd,source);self.assertEqual(list(outside.iterdir()),[])
 def test_atomic_replacement_failure_preserves_existing_app(self):
  from unittest.mock import patch
  with tempfile.TemporaryDirectory() as tmp:
   source,sd=self.fixture(Path(tmp));deployment.deploy(sd,source);before=(sd/'xhcc.wasm').read_bytes();manifest=(sd/'installation.json').read_bytes();(source/'build/wasm/xhcc.wasm').write_bytes(b'new')
   with patch.object(deployment.os,'replace',side_effect=OSError('injected replace failure')):self.assertRaises(OSError,deployment.deploy,sd,source)
   self.assertEqual((sd/'xhcc.wasm').read_bytes(),before);self.assertEqual((sd/'installation.json').read_bytes(),manifest);self.assertFalse(list(sd.rglob('.xhcc-install-*')))
if __name__=='__main__':unittest.main()
