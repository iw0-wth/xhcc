# xhcc

面向 8 MB+ RAM MCU / NuttX 等 RTOS 的 C 编译器，编译任务 RAM 目标约 4 MiB，允许适度超出但尽量控制；磁盘/Flash 中的模块和资源体积不受 4 MiB 限制。当前是首个 ARM Cortex-M3 / Thumb-2 / AAPCS soft-float 实现，编译器自身为 WASI Preview 1 command 模块。xhmake 后续独立开发。

支持 C → 预处理文本、ARM GAS 汇编、标准 ARM ELF32 ET_REL 对象；`--private-object` 显式输出开发对照用 ARKOBJ。完整链接器、任意 GCC/armcc 参数和完整 C99 一致性尚未完成。

## 构建和运行

需要 Python 3、WASI SDK、Binaryen 和 WABT；运行需要 Wasmtime。Windows 当前环境：

```powershell
D:\miniconda3\python.exe tools/build.py
D:\miniconda3\python.exe tools/xhcc.py --list-tools
D:\miniconda3\python.exe tools/xhcc.py arm-none-eabi-gcc -c examples/add.c -o build/add.o --memory-report
D:\miniconda3\python.exe tools/xhcc.py armcc --c99 --cpu Cortex-M3 -S examples/add.c -o build/add.s
```

可通过 `--sdk` / `WASI_SDK_PATH`、`--wasm-opt` / `WASM_OPT`、`--wasm-validate` / `WASM_VALIDATE` 配置工具位置。脚本默认采用当前机器的 SDK/Binaryen 路径，其他机器必须配置；VC 运行库可从当前 Python 安装目录查找。构建不依赖 make。

模块也可完全脱离 Python 启动器直接调用：

```powershell
& 'C:\Program Files\Wasmtime\bin\wasmtime.exe' run --dir=. build/wasm/xhcc.wasm arm-none-eabi-gcc -c examples/add.c -o build/add.o
```

Python 仅为桌面构建/启动适配工具，不是 MCU 编译器的运行依赖。Wasm guest 中没有进程创建或动态库依赖。文件访问需要运行时预开放对应目录。

## 当前命令子集

`-E/-S/-c` 至少指定一项；支持 `-o`、`-I`、对象宏 `-D/-U`（保留次序）、`-include`、`-std=c99`、`-O0/-O1`、`-mcpu=cortex-m3`、`-mthumb`、`-mfloat-abi=soft`、`-MMD/-MF/-MT`、`@response`、`--package-root`、`--list-tools`、`--memory-report`。armcc 别名还支持 `--c99 --cpu Cortex-M3 --thumb --fpu SoftVFP`。

未知选项明确失败。暂不支持 `-g`、`-O2`、其他 CPU/ABI、汇编输入、链接及 GNU ld/scatter 脚本。`-MT` 按 Make 字面目标转义。多源顺序编译，不允许共享显式 `-o/-MF/-MT`；默认产物为 cwd 下 basename。

响应文件支持引号、有限反斜杠转义；嵌套路径相对 cwd，不展开环境变量。限制：512 参数、64 源、32 KiB 参数存储、64 KiB 累计响应输入、8 层嵌套。

## 内存与 NuttX 状态

模块线性内存 initial=max=3 MiB（48 个 Wasm pages），含 1 MiB arena、128 KiB Wasm C 栈、静态工作区和 libc 堆。源文件容量 32 KiB，预处理输出 64 KiB，汇编 128 KiB，机器码 32 KiB，include 每层16 KiB/最多8层。超限报错；这些容量限制会随后续工作区改造调整，不能据此认为任意工程可编译。

构建后使用 Binaryen `-Oz` 优化体积并降级 SDK libc 带入的 sign-extension、饱和浮点转换和 bulk memory 操作，使用 WABT 关闭相关扩展严格验证。`tools/wasm_audit.py` 检查单个非共享 wasm32 memory、3 MiB 上限和 WASI Preview 1 imports。SDK 的 target_features 元数据可能保留 reference-types/multivalue 等声明；需要以实际运行时禁用扩展后的验证为准。

**约 4 MiB 是完整编译任务 RAM 目标，允许适度超出但尽量避免；尚未在 NuttX/WAMR 上测量。** 3 MiB 线性内存之外仍有模块字节码、解释器装载数据、宿主栈和文件系统等开销；当前 Wasm 文件约 0.54 MiB。已去掉 MCU 自定义数据段造成的约 1.5 MiB 零填充文件负担。后续必须实测 XIP/文件映射/装载方式与运行时内存，继续约束剩余约 0.46 MiB 的开销。桌面 Wasmtime 的进程 RSS 不是 MCU 编译内存的证明。

NuttX 运行时暂按 WAMR+WASI Preview 1，未有目标 `.config` 或真机。所需具体导入可用 `tools/wasm_audit.py` 获取；当前含目录枚举、路径 stat、文件读写/seek、时钟及进程退出，需要目标配置支持。

## 外部包

`packages/<id>/package.ini` 版本1包含 `id/tools/target/cpu/abi/module/include`。大头文件、运行库和目标资源放包内，编译器通过文件访问；当前包仅提供小型 freestanding 头文件子集，不是完整 libc。

桌面启动器扫描工具别名，选择包内独立 `compiler.wasm`，每次运行一个模块；构建会将当前 ARM 模块复制到默认包。复制包含完整模块及清单的新包目录即可注册新工具；新的 ISA 仍需实现对应代码后端，不能仅修改 target 字段。guest 检查目标能力，不支持的包会失败。

WASI Preview 1 本身没有通用模块启动接口。NuttX 上目录扩展代码后端的能力须由宿主 WAMR 装载器实现，当前交付的是协议和桌面参考适配器。资源包无需新增宿主 ABI。

## 验证

```powershell
D:\miniconda3\python.exe tests/test_driver.py
D:\miniconda3\python.exe tests/test_platform.py
D:\miniconda3\python.exe tests/test_host.py
D:\miniconda3\python.exe object/tests/test_elf.py
D:\miniconda3\python.exe tools/wasm_audit.py
```

ELF 执行测试还需要 GNU ARM 工具链、Unicorn 和 pyelftools。测试在两组 text/data 地址链接 xhcc 与 GCC 对象并执行，覆盖跨文件调用、数据/字符串/函数指针、局部符号、弱符号、聚合初值和软浮点。

源码基于 MIT 授权的 arkcc。来源见 NOTICE.md；原始授权保留在根目录 LICENSE。前端、IR、诊断、ARM 后端和平台层按 xhcc 自有部件组织在 src 中，无 vendor 层。现阶段保留部分 Ark 内部类型/函数前缀及显式私有对象格式，后续接口演进逐步替换。
