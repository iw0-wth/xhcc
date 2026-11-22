# xhcc 当前结构与后续接口

## 自有部件

- src/driver.c：工具名、参数、响应文件和顺序编译入口。
- src/packages.c：包清单、工具别名、目标能力和外部 include 路径。
- src/frontend/c：C 预处理、词法、语法、语义、类型与初始化。
- src/ir：栈式 IR、生成、轻量优化和阶段管线。
- src/backend/arm：Cortex-M 指令生成、汇编与重定位记录。
- object：ARM ELF32 输出及 GNU 汇编数据定义。
- src/diagnostics：诊断和可裁剪分析。
- src/core：编译调度、配置、CPU 和有界 arena。
- src/platform/wasi：stdio/WASI 文件、时间和诊断平台层。
- include：部件接口和 xh_small.h 受限构建配置。

当前内部仍复用 ArkOptions / ArkBackend / IrProgram 等名称。它们是 xhcc 自有源码中的实现接口，未承诺跨模块 ABI；后端不是通过 guest dlopen 加载。

## 模块装载协议 v1

宿主扫描 packages/<id>/package.ini，以tools唯一别名选择模块。相对module/include路径以包目录为根。宿主每次装载一个WASI Preview1 command模块，传入 [tool, --package-root, guestRoot, ...args]。目录必须预开放；退出码0成功，非零失败，诊断stderr，预处理文本stdout，编译输出到-o。退出后释放模块实例及其内存，再执行下一任务。

桌面参考宿主是tools/xhcc.py；它将包根映射到/xh-packages，cwd映射当前目录。NuttX宿主需以WAMR接口执行同等协议，传入args并配置文件访问。代码与资源包的存储可以独立于线性内存；总内存测量必须覆盖实际装载方式。

仅复制资源可扩展头文件/目标资源；新增ISA需要相应编译模块。包元数据不产生新指令编码能力。当前目标检查只允许arm/cortex-m3/aapcs-soft。

## 后续工作

1. 在实际NuttX/WAMR配置上运行，逐项核对WASI imports和3MiB线性内存之外的开销，测量约4MiB完整任务目标（允许适度超出，但尽量控制）；磁盘/Flash体积单独报告。
2. 动态include工作区和阶段复用，扩大单TU容量；栈递归限制与最坏情况计量。
3. 为Cortex-M0/M4/M7、FPU及其他目标建立独立ABI/执行测试。
4. 拆分TargetInfo、ObjectSink等稳定接口；当前内部string target判断与紧耦合尚未全部消除。
5. ELF独立.rodata、更多属性、debug/unwind、标准库与目标运行库包。
6. 在独立xhmake中实现Makefile解析，再复用工具别名与模块装载协议。

完整C99、完整gcc/armcc兼容、NuttX运行时宿主、4MiB真机总预算与xhmake都未被当前桌面测试证明。
