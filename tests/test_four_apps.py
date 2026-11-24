"""Execute all four production commands, including ld script and binary output.
Does not build the applications; usable after Python or no-Python builds.
"""
from pathlib import Path
import os, subprocess, tempfile, unittest, struct
ROOT=Path(__file__).resolve().parents[1]
RUNTIME=os.environ.get('WASMTIME',r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
class FourApps(unittest.TestCase):
 def invoke(self,app,*args,ok=True):
  p=subprocess.run([RUNTIME,'run','--dir=.',str(ROOT/'build/wasm'/f'{app}.wasm'),*args],cwd=ROOT,capture_output=True,text=True,timeout=60)
  self.assertEqual(p.returncode==0,ok,p.stdout+p.stderr)
  return p
 def test_compilers_linkers_script_and_binary(self):
  with tempfile.TemporaryDirectory(prefix='four-apps-',dir=ROOT/'build') as name:
   work=Path(name);rel=lambda p:p.relative_to(ROOT).as_posix()
   a=work/'a.c';b=work/'b.c';a.write_text('int answer(void){return 42;}\n');b.write_text('int answer(void); int entry(void){return answer();}\n')
   ao=work/'a.o';bo=work/'b.o'
   self.invoke('xhccn','arm-none-eabi-gcc','-c',rel(a),'-o',rel(ao))
   self.invoke('xhccm','arm-none-eabi-gcc','-c',rel(b),'-o',rel(bo))
   forbidden=work/'forbidden.elf'
   self.invoke('xhccn','arm-none-eabi-gcc',rel(ao),rel(bo),'--entry','entry','-o',rel(forbidden),ok=False)
   self.assertFalse(forbidden.exists())
   script=work/'memory.ld';script.write_text('ENTRY(entry)\nMEMORY { FLASH(rx) : ORIGIN = 0x08004000, LENGTH = 128K RAM(rw) : ORIGIN = 0x20002000, LENGTH = 32K }\nSECTIONS { .text : { *(.text*) *(.rodata*) } > FLASH .data : { _sdata = .; *(.data*) _edata = .; } > RAM AT> FLASH .bss (NOLOAD) : { _sbss = .; *(.bss*) *(COMMON) _ebss = .; } > RAM }\n')
   outputs=[]
   for app in ('xhcc','xhccm','xhlinker'):
    output=work/(app+'.elf');args=([ 'arm-none-eabi-gcc'] if app!='xhlinker' else [])+['-T',rel(script),rel(ao),rel(bo),'-o',rel(output)]
    self.invoke(app,*args);data=output.read_bytes();self.assertEqual(data[:6],b'\x7fELF\x01\x01');self.assertEqual(struct.unpack_from('<HH',data,16),(2,40));self.assertTrue(struct.unpack_from('<I',data,24)[0]&1);outputs.append(data)
    raw=work/(app+'.bin');self.invoke(app,*(([ 'arm-none-eabi-gcc'] if app!='xhlinker' else [])+['-T',rel(script),'--oformat=binary',rel(ao),rel(bo),'-o',rel(raw)]))
    expected=bytearray();segments=[];phoff=struct.unpack_from('<I',data,28)[0];phsize,phnum=struct.unpack_from('<HH',data,42)
    for i in range(phnum):
     typ,off,vma,lma,filesz,memsz,flags,align=struct.unpack_from('<8I',data,phoff+i*phsize)
     if typ==1 and filesz:segments.append((lma,data[off:off+filesz]))
    self.assertTrue(segments);base=min(x[0] for x in segments);end=max(x[0]+len(x[1]) for x in segments);expected=bytearray(end-base)
    for lma,payload in segments:expected[lma-base:lma-base+len(payload)]=payload
    self.assertEqual(raw.read_bytes(),bytes(expected))
   self.assertEqual(outputs[0],outputs[1]);self.assertEqual(outputs[0],outputs[2])
if __name__=='__main__':unittest.main()
