"""Run all required integration gates and preserve failure exit statuses."""
from pathlib import Path
import subprocess, sys
ROOT=Path(__file__).resolve().parents[1]
for script in ['tests/test_driver.py','tests/test_platform.py','tests/test_host.py','tests/test_package_options.py','tests/test_package_capabilities.py','tests/test_cpu_rules.py','tests/test_target_rules.py','tests/test_predefine_order.py','tests/test_rv_templates.py','tests/test_rules.py','tests/test_template.py', 'tests/test_object_rules.py','tests/test_relocation_rules.py','tests/test_arm_relocation.py','tests/test_arm_rules.py','tests/test_x86_rules.py','tests/test_c_rules.py','object/tests/test_elf.py','object/tests/test_cpus.py','object/tests/test_sections.py','tests/test_targets.py','tests/test_language.py','tests/test_legacy_parity.py','tools/wasm_audit.py']:
    print(f'Running {script}',flush=True)
    result=subprocess.run([sys.executable,str(ROOT/script)],cwd=ROOT)
    if result.returncode:sys.exit(result.returncode)
print('All integration gates passed.')
