# xhcc 当前结构

生产架构为一个WASI Preview1主程序，共享C前端/IR/通用优化/诊断，按工具名和外部配置包选择目标。后端子Wasm、分阶段前端/后端模块和宿主dispatch均已退出生产路径。

## 部件

- src/frontend/c：唯一C预处理、词法、语法、语义、类型、初始化实现。
- src/ir：通用IR、优化、分析与原有IR存档。
- src/driver.c、src/cli/package_options.c：统一请求解析与配置驱动参数映射。
- src/packages.c：配置发现、工具别名、目标默认值、外部资源和编码规则路径。
- src/backend/{arm,riscv32,x86_64,c}：当前机器目标和C发射器。
- src/backend/rules：共享字段编码规则VM；外部RV32 R/I/S/B/U/J布局。
- object：标准ELF32/64、符号、重定位和GNU数据定义。
- src/core：编译调度、CPU、配置、arena、私有对象和兼容链接。
- src/diagnostics、src/platform/wasi：诊断及WASI文件/标准流/时间适配。
- packages：v3配置、参数映射、编码规则、头文件和运行库源码，无包内Wasm。

## 扩展边界

配置包可以独立新增工具别名、参数名称、默认CPU/ABI、包含路径和运行库，并替换规则引擎支持的编码表。主程序仅按schema读取，不为每个包加兼容分支。

当前RV32通用编码VM已取代六类写死编码helper；它按字段散布/有符号范围/对齐等规则工作。有界selection v2 VM以每条规则最多8步的序列，将标量32位算术、位运算、移位、比较及有符号/无符号除法取模的IR选择和寄存器模板外置；兼容v1单指令规则。每类缓存只读取当前包的规则。其余指令选择、通用寄存器分配、ABI和重定位仍有C代码，新增任意全新ISA尚需扩规则表达能力及相应实现，不能仅改包target名称。

后续逐步把IR模式选择、合法化、寄存器约束、调用约定、重定位与对象属性转成有版本的描述。每步通过旧后端字节/执行对照，禁止用忽略约束和截断编码假装支持。

## RAM

默认4MiB线性内存含静态区、1MiB arena、C栈和libc堆。启用ARM活跃性分析时工作区按需申请，结束释放。外部规则按需加载一个缓存，外部运行库/头文件不打入主模块。

完整任务还含运行时装载的字节码、WAMR元数据、宿主栈和文件系统等，需要目标NuttX/WAMR配置实测。Wasm文件大小和线性内存上限分别报告；桌面WasmtimeRSS不能替代MCU测量。

## 后续重点

1. NuttX/WAMR导入与内存实测；扩大单TU容量的工作区复用。
2. 继续后端规则数据化，避免大量写死目标逻辑和常驻表。
3. Thumb-1/M0系列、完整VFP hard ABI、TLS/debug/unwind及更多标准对象能力。
4. 完整GNU约束分配和更广GCC参数语义。
5. 独立xhmake读取标准Makefile，以工具别名复用xhcc。
