"""Run all required integration gates and preserve failure exit statuses."""
from pathlib import Path
import subprocess, sys
ROOT=Path(__file__).resolve().parents[1]
for script in ['tests/test_driver.py','tests/test_platform.py','tests/test_host.py','object/tests/test_elf.py','tools/wasm_audit.py']:
    print(f'Running {script}',flush=True)
    result=subprocess.run([sys.executable,str(ROOT/script)],cwd=ROOT)
    if result.returncode:sys.exit(result.returncode)
print('All integration gates passed.')
