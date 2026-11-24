# 曦和编译器 · XH Compiler Collection

[English](README.md) · 简体中文

**项目定位**

> 可在 MCU 上运行的跨 OS 模块化编译器集合：多后端交叉编译生成可烧录固件，兼容 make/gcc 语法并接入 make/CC，开发态可由 RTOS harness 调用，通过复制目录添加后端。

曦和编译器（`xhcc`）当前以 WASI Preview1 command 运行，具备外部目标包、有界 make/GCC 兼容接口和 ARM 标准 ELF 链接器，已验证桌面执行与模拟 SD 部署。MCU 部署和 RTOS harness 集成仍是目标，NuttX/WAMR 完整内存占用尚未实测。复制包目录可扩展已有引擎，任意新 ISA 仍可能需要开发引擎。

## 四个应用

| 应用 | 内容与用途 |
| --- | --- |
| `xhcc.wasm` | 多目标 C 编译器，含有界标准 ARM ELF 链接及历史兼容接口 |
| `xhccm.wasm` | 编译器/链接器加进程内 make 子集，编译配方直接调用驱动，不启动 shell 或子进程 |
| `xhlinker.wasm` | 独立标准 ARM Thumb ELF32 链接器，不含 C 前端、IR 生成、代码生成后端或 make |
| `xhccn.wasm` | 编译期移除链接功能的多目标编译器，拒绝链接请求 |

编译器应用共享前端、IR 与目标引擎源码。四个命令并列部署在唯一的 `packages/` 树旁，不采用逐后端子 Wasm。默认每个应用的 **Wasm 线性内存上限为 4 MiB**，不代表完整 MCU RAM 已证明满足 4 MiB。

## 构建与快速使用

除特别说明外，命令均在仓库根目录运行。宿主构建需要 WASI SDK、Binaryen、WABT，桌面运行使用 Wasmtime。请按本机环境配置工具位置，不依赖脚本内的本地 Windows 回退路径。

保留 Python 宿主构建路径：

```sh
# 将 WASI_SDK_PATH 设置为本机 SDK 目录。
# 若工具不在 PATH 中，另设 WASM_OPT 和 WASM_VALIDATE。
python tools/build.py --app all-apps --target all
python tools/xhcc.py --list-tools
python tools/xhcc.py arm-none-eabi-gcc -c examples/add.c -o build/add.o
python tools/xhcc.py armcc --cpu Cortex-M4 -S examples/add.c -o build/add.s
python tools/xhcc.py riscv32-unknown-elf-gcc -c examples/add.c -o build/add-rv.o
python tools/xhcc.py x86_64-linux-gnu-gcc -c examples/add.c -o build/add-x64.o
```

[构建脚本](tools/build.py) 默认 `--app all-apps --target all`。可选单应用 `xhcc`、`xhccm`、`xhccn`、`xhlinker`；`--app both` 保留旧的 xhcc+xhccm 选择。生产输出为 `build/wasm/<app>.wasm`。Python 开发构建及 map 位于 `build/wasm/dev/all/`，非 xhcc 应用各有同名子目录。单目标开发构建不会替换生产多目标模块。

根 [Makefile](Makefile) 默认构建四应用：`make all`，或单选 `make xhcc`、`make xhccm`、`make xhccn`、`make xhlinker`。可覆盖 `PYTHON`、`WASI_SDK_PATH`。脚本支持 `--sdk`、`--wasm-opt`、`--wasm-validate` 及对应的 `WASI_SDK_PATH`、`WASM_OPT`、`WASM_VALIDATE` 环境设置。

另有已执行验证、不调用 Python 解释器的构建路径：

```sh
make -f Makefile.nopy all
```

[Makefile.nopy](Makefile.nopy) 由 GNU make 直接调用 WASI SDK、Binaryen、WABT 和 POSIX shell 工具。Windows 使用 MSYS2 GNU make/Bash 并配置工具路径；`DLL_DIR` 可选提供 SDK 运行库 DLL。模块及 map 位于 `build/nopy/<app>/`，验证后复制至 `build/wasm/`。详见 [有 Python 与无 Python 构建](docs/no-python-build.md)。这是宿主构建路径，不是 xhcc 自举。

Python [桌面适配器](tools/xhcc.py) 发现包并启动编译器，Wasm 命令本身不依赖 Python。需要时设置 `WASMTIME` 或传入 `--wasmtime`。也可直接运行：

```sh
wasmtime run --dir=. build/wasm/xhcc.wasm arm-none-eabi-gcc -c examples/add.c -o build/add.o --memory-report
```

## ARM ELF 链接与镜像输出

先编译，再链接显式对象。以下最小示例以 `add` 为 ELF 入口演示链接，不是板级启动程序：

```sh
python tools/xhcc.py arm-none-eabi-gcc -c examples/add.c -o build/add.o
python tools/xhcc.py arm-none-eabi-gcc build/add.o --entry add --text-address 0x08004000 --data-address 0x20002000 -o build/add.elf
wasmtime run --dir=. build/wasm/xhlinker.wasm --entry add --text-address 0x08004000 --data-address 0x20002000 --oformat=binary build/add.o -o build/add.bin
```

`xhcc`、`xhccm`、`xhlinker` 接受已支持的小端 ARM Thumb ELF32 可重定位对象，输出可执行 ELF 或 flat binary。`-T script.ld` 接受受检 MEMORY/SECTIONS 子集，覆盖 Flash/RAM 布局、data VMA/LMA、BSS/COMMON 及部分启动符号赋值。flat 输出按加载地址排列，不填充 Flash 到 SRAM 的巨大地址空洞。GNU ld 对照覆盖两种布局和真实执行、local/weak/strong/COMMON 符号、重定位及镜像字节。

自动 `.c` 编译后链接、混合 `.c` 与 `.o`、归档/`-l`、非 ARM 标准链接和完整 GNU 链接脚本仍不支持。启动代码、向量表、data 搬运/BSS 清零及运行库由工程提供，生成 binary 不代表可在任意开发板启动。`--ark-compat` 下的历史私有 ARKOBJ 链接、IR/LTO 和部分镜像输出仍为独立接口，不表示标准 ELF 或 GCC LTO 兼容。详见 [ARM 链接器合同](docs/elf-linker-first-stage.md) 与 [进度和规划](docs/progress-and-roadmap.md)。

文件输出先写同目录临时文件，成功后在支持的文件系统上替换目标；不承诺 fsync/崩溃持久性，stdout 不具原子性。

## SD 部署与 make

构建四应用后部署：

```sh
python tools/deploy_sd.py
python tests/test_sd.py
```

[部署脚本](tools/deploy_sd.py) 默认写入 `../sd/xhcc/`，可用 `--sd <directory>` 覆盖。安装四个并列应用、共享包、含 `memory.ld` 与 `Makefile.link` 的生成示例及 `installation.json` 清单。以下命令在**部署目录内**运行，并将此目录映射为 guest `/`：

```sh
wasmtime run --dir=.::/ --env XHCC_PACKAGE_ROOT=/packages xhccm.wasm make -C examples/make-demo all
wasmtime run --dir=.::/ --env XHCC_PACKAGE_ROOT=/packages xhccm.wasm make -C examples/make-demo -f Makefile.link all
wasmtime run --dir=.::/ --env XHCC_PACKAGE_ROOT=/packages xhlinker.wasm -T examples/make-demo/memory.ld --oformat=binary examples/make-demo/first.o examples/make-demo/second.o examples/make-demo/entry.o -o examples/make-demo/firmware.bin
```

使用 make 的 `-C` 时保持 `XHCC_PACKAGE_ROOT` 为 guest 绝对路径。`xhccm make [TOOL] ...` 注入默认 `CC`；命令行 `CC=...` 优先于普通赋值，显式 `override` 赋值优先。配方中写明的编译器名直接选择该工具。

make 子集支持 24 个文本/控制/诊断函数、变量与替换引用、`override`/`undefine`、include、有界条件、显式/模式/后缀规则、order-only 依赖、依赖文件及增量/强制构建。自动变量含 `$?`、`$|`、`$+`；`$+` 按 GNU 声明顺序保留普通依赖重复项，带配方声明优先。目标与模式变量向前置依赖继承，退出作用域恢复；模式由宽到窄应用，随后应用显式目标变量。多个显式目标可共享配方，但各自独立检查 mtime，使用各自 `$@` 和变量。

`-j1` 为串行，其他并行数拒绝。grouped `&:`、多目标变量声明、多个模式目标共享配方、环境导入/export、递归 make 和 shell 脚本不支持。支持的编译/链接配方在进程内运行，字面文件命令有界。`.DELETE_ON_ERROR:` 显式启用后，在未忽略的失败时删除新建/mtime 改变的普通目标；未改文件、目录及 `.PRECIOUS` 匹配保留。变化检测为 mtime 快照，不是内容哈希。准确范围见 [make 子集](docs/make-subset.md)。SD 测试属于桌面部署模拟，不是真机验证。

## 目标与语言范围

机器目标 `-c` 输出标准 ELF `ET_REL`，C 后端输出供另一编译器处理的源码。

| 目标 | 当前范围 | 主要边界 |
| --- | --- | --- |
| ARM | Cortex-M3/M4/M7/M33、Thumb-2、base AAPCS、受检 soft/softfp | 无 hard-float 参数 ABI；M0/M0+/M1/M23 需要 Thumb-1 lowering |
| RV32 | RV32IM、ILP32、软件浮点辅助调用 | 无 RV64 或硬浮点 ABI |
| i386 | ILP32、标量 SysV 子集和 xhcc 聚合 wire | 不保证完整 GNU 聚合 ABI |
| AMD64 | LP64、标量 SysV/SSE、已测变参及可选有限聚合 | 非完整原生 SysV 聚合互操作 |
| C | IR 生成 C 文本 | 不是 wasm32 输出后端 |

前端继承 arkcc 的 C99 实现，带 ARMCC 扩展及有限 C11 关键词子集：`_Alignas`、`_Alignof`、`_Static_assert`、`_Generic`、`_Thread_local`、`_Atomic`。原生 TLS 明确拒绝，原子操作有目标/运行库边界。基本/ARMCC asm 按目标支持；GNU 扩展 asm 支持一个 32 位 `=r` 输出、最多两个 `r/i` 输入及 `memory/cc` clobber。详见 [目标](docs/targets.md)、[功能限制](docs/feature-parity.md) 与 [对象文档](object/README.md)。

工具别名及 GCC/armcc 风格选项提供有界兼容，不代表完整 GNU GCC 或商用 ARM 编译器。现代驱动支持预处理、汇编/对象输出、诊断、依赖、有界 response 文件与多源顺序编译。未知或未实现选项报错，不提供全部 GCC 参数及 DWARF ELF 输出。

### AMD64 实验性 ABI 计划

`-frecord-sysv-abi` 记录参数/返回元数据；`-fuse-sysv-abi-plan` 记录并消费认证计划，两者仍默认关闭。具名参数/返回支持标量、自然对齐 1–8 字节整数/指针结构体、部分纯浮点结构体（float、两个 float、double）、严格 LP64 16 字节 LL/LD/DL/DD 双字段及自然对齐 8 字节 `{int,float}` / `{float,int}`。use 模式拒绝 packed、位域、union、数组、变参聚合和其他未认证形状；其余可记录计划仍仅供检查。

测试覆盖 Clang 双向互操作、寄存器 bank 耗尽时整对象回栈、精确尺寸边界/canary、结果缓冲及经 legacy LTO 的私有 IR 保存重载。校验版本化源类型资格、ABI 身份、规则指纹与保留的函数签名，等价计划可有不同索引。调用点实参类型尚未完整保留，不构成任意 IR 安全保证。IR v2 持久化计划，v1 读取为无计划。详见 [ABI 合同与执行证据](docs/abi-plan-ir-v2.md)。

## 外部后端、内存与自举

[包协议](docs/package-protocol.md) 注册工具别名、CPU、ABI 合同、头文件、选项与规则。五个引擎均实际消费外部规则，主模块哈希不变的规则变体可改变真实生成/执行行为。复杂 lowering、优化及布局政策仍有部分 C 实现。详见 [外置化状态](docs/backend-externalization.md) 与 [后端移植指南](docs/skills/port-backend/SKILL.md)。

每个默认模块声明一个非共享 wasm32 线性内存，initial = maximum = 4 MiB，包含静态数据、C 栈、arena 与 libc 堆。运行时结构、字节码、宿主栈、WASI 和文件系统缓冲增加完整任务 RAM。该上限和桌面 RSS 均不能证明 MCU 可行性，详见 [资源测量](docs/resource-budget.md)。

目前**没有 wasm32 输出后端，也未完成 Wasm 自举**，这些 Wasm 应用由宿主 Clang 构建。默认 32 KiB 源文件上限已无法容纳数个较大的编译器翻译单元，前端/sysroot/runtime 和内存工作仍待完成，见 [Wasm 后端与自举计划](docs/wasm-bootstrap-plan.md)。

## 验证与许可

```sh
python tools/test.py
python tests/test_four_apps.py
python tests/test_sd.py
python tools/resource_report.py --self-test --json build/reports/resources.json --markdown build/reports/resources.md
```

执行测试前构建，SD 测试前部署。相关检查需要 GNU ARM 工具、Unicorn、pyelftools 和合适的 Clang/LLVM；缺少参考工具时可能跳过，跳过不构成互操作证据。报告描述实测产物快照，不是固定体积规格。

初始前端、IR 与 ARM 发射器源自 MIT 授权的 arkcc，见 [来源](NOTICE.md) 和 [MIT 许可](LICENSE)。目标扩展依据公开 ISA/ABI/对象规范独立实现，通过外部编译器验证，不将 GPL 编译器实现代码复制混入本项目。
