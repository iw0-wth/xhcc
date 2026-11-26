"""GCC-compatible warning group membership; explicit xhcc lint stays available."""
from pathlib import Path
import os,subprocess,tempfile,unittest
from test_driver import ROOT,command
GNU=Path(os.environ.get('GNUARM',r'D:\arm-none-eabi'))/'bin/arm-none-eabi-gcc.exe'
class WarningGroups(unittest.TestCase):
 def test_gnu_wall_wextra_do_not_enable_unreachable(self):
  p=subprocess.run([str(GNU),'-Wall','-Wextra','-Q','--help=warnings','-c','-x','c','-o',os.devnull,'-'],input='int f(void){return 0;}\n',capture_output=True,text=True)
  self.assertEqual(p.returncode,0,p.stderr)
  rows=[x for x in p.stdout.splitlines() if x.strip().startswith('-Wunreachable-code ')]
  self.assertTrue(rows,p.stdout);self.assertTrue(all('[disabled]' in x or '[ignored]' in x for x in rows),rows)
 def test_xhcc_membership_and_explicit_override(self):
  with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='warning-groups-') as tmp:
   src=Path(tmp)/'one.c';src.write_text('int entry(void){return 0;}\n')
   for flags,want in [(['-Wall','-Wextra'],0),(['-Wall','-Wextra','-Wunreachable-code'],1),(['-Wunreachable-code','-Wall'],1),(['-Wunreachable-code','-Wno-unreachable-code'],0)]:
    p=subprocess.run([*command(),'arm-none-eabi-gcc','-v','-fsyntax-only',*flags,str(src.relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True)
    self.assertEqual(p.returncode,0,p.stdout+p.stderr);self.assertIn('warn_unreachable='+str(want),p.stdout+p.stderr)
if __name__=='__main__':unittest.main()
