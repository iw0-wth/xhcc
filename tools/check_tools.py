from pathlib import Path
import subprocess
for exe in [Path(r'D:\wasi-sdk-34.0-x86_64-windows\bin\clang.exe'), Path(r'C:\Program Files\Wasmtime\bin\wasmtime.exe'),Path(r'D:\arm-none-eabi\bin\arm-none-eabi-gcc.exe')]:
    p=subprocess.run([str(exe),'--version'])
    print(str(exe), 'exit', p.returncode, hex(p.returncode & 0xffffffff),flush=True)
