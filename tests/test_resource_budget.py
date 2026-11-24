"""Validate deployed resource facts, not inferred MCU RAM or host RSS.

Checks actual Wasm memory sections, input-object link-map boundaries, SD hashes,
package-only data, and measured arena peaks for bounded representative inputs.
Run: python tests/test_resource_budget.py [--sd ../sd/xhcc]
"""
from pathlib import Path
import argparse,hashlib,os,re,subprocess,sys,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from resource_report import wasm_memory
SD=Path(os.environ.get('XHCC_TEST_SD',ROOT.parent/'sd/xhcc')).resolve()
RUNTIME=os.environ.get('WASMTIME',r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
class ResourceBudget(unittest.TestCase):
 def test_all_four_apps_have_explicit_four_mib_memory_ceiling(self):
  for app in ['xhcc','xhccm','xhccn','xhlinker']:
   with self.subTest(app=app):
    report=wasm_memory(ROOT/f'build/wasm/{app}.wasm');self.assertEqual(len(report['memories']),1,report)
    memory=report['memories'][0];self.assertEqual(memory['maximum_bytes'],4*1024*1024);self.assertLessEqual(memory['initial_bytes'],memory['maximum_bytes']);self.assertFalse(memory['shared']);self.assertFalse(memory['memory64'])
 def test_link_maps_prove_compiler_make_separation(self):
  compiler=(ROOT/'build/wasm/dev/all/xhcc.map').read_text(encoding='utf-8')
  make=(ROOT/'build/wasm/dev/all/xhccm/xhcc.map').read_text(encoding='utf-8')
  # Maps enumerate linked input object sections; require positive evidence for
  # compiler/backend objects, and Make's parser/entry in the sibling app.
  self.assertIn('src_core_compiler.c.o:',compiler);self.assertNotRegex(compiler,r'src_make_[^\s:]*\.o:');self.assertNotIn('xh_make_main',compiler)
  self.assertRegex(make,r'src_make_make\.c\.o:');self.assertIn('xh_make_main',make);self.assertIn('src_core_compiler.c.o:',make)
 def test_link_maps_prove_compiler_only_and_linker_only_boundaries(self):
  compiler_only=(ROOT/'build/wasm/dev/all/xhccn/xhcc.map').read_text(encoding='utf-8')
  linker_only=(ROOT/'build/wasm/dev/all/xhlinker/xhcc.map').read_text(encoding='utf-8')
  # Positive input-object evidence keeps a missing or empty map from passing
  # the negative checks. These are the Wasm maps, not native build artifacts.
  self.assertIn('src_core_compiler.c.o:',compiler_only)
  self.assertRegex(compiler_only,r'src_frontend_[^\s:]*\.o:')
  self.assertNotRegex(compiler_only,r'src_link_[^\s:]*\.o:')
  for symbol in ('xh_elf_link_arm','ld_find_global','lto_read_payload'):
   with self.subTest(app='xhccn',symbol=symbol):self.assertNotIn(symbol,compiler_only)
  self.assertIn('src_link_elf_link.c.o:',linker_only);self.assertIn('xh_elf_link_arm',linker_only)
  self.assertNotRegex(linker_only,r'src_frontend_[^\s:]*\.o:')
  self.assertNotIn('src_core_compiler.c.o:',linker_only)
  self.assertNotIn('ark_frontend_unit',linker_only);self.assertNotIn('ark_compile',linker_only)
 def test_packages_contain_data_and_no_wasm(self):
  manifests=list((ROOT/'packages').glob('*/package.ini'));self.assertGreaterEqual(len(manifests),5)
  self.assertFalse(list((ROOT/'packages').rglob('*.wasm')))
  self.assertFalse(list((SD/'packages').rglob('*.wasm')))
  self.assertEqual({p.parent.name for p in manifests},{p.parent.name for p in (SD/'packages').glob('*/package.ini')})
 def test_sd_app_and_package_hashes_match_build(self):
  self.assertTrue(SD.is_dir(),f'SD root missing: {SD}; refresh deployment before this gate')
  for app in ['xhcc','xhccm','xhccn','xhlinker']:
   built=ROOT/f'build/wasm/{app}.wasm';deployed=SD/f'{app}.wasm';self.assertTrue(deployed.is_file(),str(deployed));self.assertEqual(hashlib.sha256(built.read_bytes()).digest(),hashlib.sha256(deployed.read_bytes()).digest(),f'stale SD application: {app}')
  source_files={p.relative_to(ROOT/'packages').as_posix():p for p in (ROOT/'packages').rglob('*') if p.is_file()}
  deployed_files={p.relative_to(SD/'packages').as_posix():p for p in (SD/'packages').rglob('*') if p.is_file()}
  self.assertEqual(source_files.keys(),deployed_files.keys(),'SD package file set differs')
  for name,path in source_files.items():self.assertEqual(hashlib.sha256(path.read_bytes()).digest(),hashlib.sha256(deployed_files[name].read_bytes()).digest(),f'stale SD package resource: {name}')
 def test_actual_multi_object_link_requested_heap_budget(self):
  with tempfile.TemporaryDirectory(prefix='link-resources-',dir=ROOT/'build') as temp:
   work=Path(temp);rel=lambda path:path.relative_to(ROOT).as_posix();objects=[]
   def invoke(app,args,ok=True):
    p=subprocess.run([RUNTIME,'run','--dir=.','--env','XHCC_PACKAGE_ROOT=packages',str(ROOT/'build/wasm'/f'{app}.wasm'),*args],cwd=ROOT,capture_output=True,text=True,timeout=60);self.assertEqual(p.returncode==0,ok,p.stdout+p.stderr);return p
   # Eight independent translation units retain 64 definitions plus addresses
   # in actual ELF objects. No fabricated table statistics stand in for linking.
   for index in range(8):
    source=work/f'unit{index}.c';output=work/f'unit{index}.o';text=''.join(f'int data_{index}_{j}={index+j+1}; int *ptr_{index}_{j}=&data_{index}_{j};\n' for j in range(8));text+=f'int function_{index}(void){{return *ptr_{index}_0;}}\n'
    if index==0:text+='extern int function_7(void); int entry(void){return function_7()+*ptr_0_0;}\n'
    source.write_text(text);invoke('xhccn',['arm-none-eabi-gcc','-O0','-c',rel(source),'-o',rel(output)]);objects.append(rel(output))
   script=work/'memory.ld';script.write_text('ENTRY(entry)\nMEMORY { FLASH(rx) : ORIGIN = 0x08004000, LENGTH = 256K RAM(rw) : ORIGIN = 0x20002000, LENGTH = 64K }\nSECTIONS { .text : { *(.text*) *(.rodata*) } > FLASH .data : { *(.data*) } > RAM AT> FLASH .bss (NOLOAD) : { *(.bss*) *(COMMON) } > RAM }\n')
   measures={}
   for app in ['xhcc','xhlinker']:
    output=work/(app+'.elf');args=(['arm-none-eabi-gcc'] if app=='xhcc' else [])+['-T',rel(script),*objects,'-o',rel(output),'--memory-report'];p=invoke(app,args);self.assertEqual(output.read_bytes()[:4],b'\x7fELF');reports=re.findall(r'link requested heap peak\s+(\d+)/(\d+)\s+B',p.stderr);self.assertEqual(len(reports),1,p.stderr);peak,limit=map(int,reports[0]);self.assertGreater(peak,0);self.assertLessEqual(peak,limit);self.assertEqual(limit,524288);counts=re.findall(r'objects=(\d+) sections=(\d+) symbols=(\d+) output=(\d+) B',p.stderr);self.assertEqual(len(counts),1,p.stderr);object_count,section_count,symbol_count,output_bytes=map(int,counts[0]);self.assertEqual(object_count,8);self.assertGreater(section_count,0);self.assertGreaterEqual(symbol_count,64);self.assertEqual(output_bytes,output.stat().st_size);measures[app]={'requested_peak_bytes':peak,'limit_bytes':limit}
   preserved=work/'preserved.elf';preserved.write_bytes(b'preserve existing output');p=invoke('xhlinker',['-T',rel(script),*objects,'-o',rel(preserved),'--max-memory','1024','--memory-report'],ok=False);self.assertEqual(preserved.read_bytes(),b'preserve existing output');self.assertRegex(p.stderr.lower(),r'memory|budget');reports=re.findall(r'link requested heap peak\s+(\d+)/(\d+)\s+B',p.stderr);self.assertEqual(len(reports),1,p.stderr);failed_peak,failed_limit=map(int,reports[0]);self.assertEqual(failed_limit,1024);self.assertLessEqual(failed_peak,failed_limit)
  print('MEASURED linker requested allocation bytes only (excludes allocator overhead, parsed rule heaps, runtime/JIT, WASI and total RSS): '+str(measures))
 def test_representative_runtime_arena_peak_below_one_mib(self):
  cases=[('arm-none-eabi-gcc','-c','probe.o'),('riscv32-unknown-elf-gcc','-c','probe.o'),('xh-c-gcc','-S','probe.c.out')]
  source='int add(int a,int b){return a+b;} int entry(void){int a[8];int i,total=0;for(i=0;i<8;i++){a[i]=add(i,3);total+=a[i];}return total;}\n'
  with tempfile.TemporaryDirectory(prefix='resources-',dir=ROOT) as temp:
   directory=Path(temp);input=directory/'probe.c';input.write_text(source);peaks={}
   for tool,mode,output in cases:
    result=subprocess.run([RUNTIME,'run','--dir=.','--env','XHCC_PACKAGE_ROOT=packages','build/wasm/xhcc.wasm',tool,mode,input.relative_to(ROOT).as_posix(),'-o',(directory/output).relative_to(ROOT).as_posix(),'--memory-report'],cwd=ROOT,capture_output=True,text=True,timeout=60)
    self.assertEqual(result.returncode,0,result.stdout+result.stderr);matches=re.findall(r'arena peak\s+(\d+)/(\d+)\s+B',result.stderr);self.assertEqual(len(matches),1,result.stderr);peak,capacity=map(int,matches[0]);self.assertGreater(peak,0);self.assertLessEqual(peak,capacity);self.assertLess(peak,1024*1024,(tool,peak,capacity));peaks[tool]={'peak_bytes':peak,'capacity_bytes':capacity}
  print('MEASURED arena only (excludes parsed rule heaps, Wasm runtime/JIT, WASI and total RSS): '+str(peaks))
if __name__=='__main__':
 parser=argparse.ArgumentParser(add_help=False);parser.add_argument('--sd',type=Path);options,remaining=parser.parse_known_args()
 if options.sd:SD=options.sd.resolve()
 unittest.main(argv=[sys.argv[0],*remaining])
