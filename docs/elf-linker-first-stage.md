# 标准 ELF 链接器首阶段与目标扩展

首版已将有界 ELF32 ARM ET_REL 读取、六类 ARM REL 适配、节布局、符号决议和输出接入 `xhcc`、`xhccm` 与独立 `xhlinker`；`xhccn` 在编译期排除对象链接器。支持显式 ARM `.o` 列表、受检 MEMORY/SECTIONS `.ld` 子集、ET_EXEC 及按 LMA 排列的 binary。独立 GNU 对照与执行测试已通过；它不是完整 GNU ld，不自动提供启动代码、运行库或板级配置。完整应用验收和范围见 [进度与规划](progress-and-roadmap.md)。

## 跨架构扩展

布局、符号决议、脚本、内存预算和输出由同一核心复用，目标描述与适配器区分 ARM/RV32。RV32 当前只接受 flags=0、ILP32 soft ABI、无 RVC/RVE 的 ELF32 RELA；PCREL LO12 要求同输入节中可验证的唯一 HI20 anchor，RELAX 只验证关联后作 no-op，不执行指令缩短。具体重定位、字节 oracle 与错误边界见 [RV ELF relocation](rv-elf-relocations.md)；真实生产对象分节见 [RV ELF sections](rv-elf-sections.md)。这不是所有架构标准链接已完成，i386/AMD64 等尚未接入。

## ARM 首版输入合同

- 显式列出的 ELF32、little-endian、ET_REL、EM_ARM、EABI5/base soft-float 对象，首版只面向 Thumb。核验 class、endianness、machine、ABI flags 和每个表的边界，不只检查 machine。
- 读取器借用输入缓冲区，视图、字符串和节数据都不能越过该缓冲区的生命周期。不允许保存视图后覆盖共用输入缓冲区。
- 不支持的 RELA、扩展节索引、group/COMDAT、TLS、未知 alloc 节语义和未实现 relocation 明确拒绝。非 alloc 调试内容不自动获得运行语义。
- 首批 REL 为 ABS32、THM_CALL、THM_JUMP24、THM_JUMP19、独立 THM_MOVW_ABS_NC 和 THM_MOVT_ABS。解码输入指令中的隐式 addend 后使用 ELF 专用规则，符号地址与 Thumb 状态显式区分，不能把奇数数据地址误当成 Thumb 函数；不能套用私有 `S-P-4` 分支或相邻 MOVW/MOVT pair delta。

## 已实现的 link core 结构

1. 扫描、计数并校验输入，复制后续必需的元数据与名称，建立 section 布局和 global 符号决议。
2. 逐个重新读取对象，通过借用视图获取 PROGBITS 与 REL；复制节内容、解析符号、应用重定位。所有输入及重定位成功后才原子写输出 ELF 或 binary。

有界实现按以下职责组织（具体结构以源码为准）：

| 记录 | 必需信息 |
| --- | --- |
| ObjPlan | 路径、section/symbol 区间 |
| SecPlan | 对象和原始节索引、type/flags、size/alignment、VMA/LMA、输出偏移 |
| SymPlan | 对象和原始 symbol 索引、名称池偏移、shndx/value/size/bind/type、决议引用 |
| GlobalPlan | 名称、胜出定义、COMMON 大小/对齐、状态 |
| LinkLimits | 对象、节、符号、名称、输入和镜像容量上限 |

以原始 `(object, symtab-section, symbol-index)` 区分 local 和 section symbol（或首版明确每对象只接受一个 SYMTAB）；不能复用 legacy 仅名字与合并代码 offset 的表。global strong 优先于 COMMON 和 weak，重复 strong 报错；COMMON 按最大大小和对齐合并；ABS 不重定位；未定义 strong 报错。未定义 weak 数据可归零，未定义 weak 分支首版明确拒绝，不由只接收解析地址的补丁 API 猜测。

section 按 flags/type 而非仅名称布局，分 RX/RO/RW/NOBITS，保留已支持的 custom PROGBITS 与 init/fini array 内容，但不自动调度其运行时。地址、大小、对齐和分配乘法均检查溢出；计数先过 limit 再分配。reader 的单表上限不能代替整个 link 的总符号、重定位与名称池预算；Thumb 策略检查也须由 link core 主动调用，不能把格式读取成功当作所有指令已获认证。

## 输出与边界

目标为 ET_EXEC，Flash RX 与 RAM RW 的 PT_LOAD 正确表达 `p_filesz/p_memsz`。data 的 Flash LMA 与 RAM VMA 分开，BSS 只增加内存大小。flat image 按 LMA 生成，不能填充 Flash 到 SRAM 的巨大地址空洞。entry 保留 Thumb 语义。

启动代码、vector table、data copy/BSS zero、constructor 调度、libgcc/libc、归档自动提取和完整 GNU linker script 均须单独提供或实现；首版不宣称任意 GCC 工程链接兼容。

## 脚本断言与统计

脚本支持最多 32 个 `ASSERT(expr, "message")`，在布局和脚本符号求值后按事件保存的 dot 求值；失败或未解析表达式拒绝输出并保留旧文件。比较与 `!`、`&&`、`||` 为受检表达式，逻辑操作在本子集中短路。GNU ld 对未选分支的符号查找可能仍 eager，因此这不是全部表达式行为的逐项兼容承诺。标准 Flash/RAM 容量断言已与 GNU ld 对照布局、执行和 binary。

编译器链接入口及独立链接器的 `--memory-report` 报告本次链接核心 requested heap 峰值、完成扫描的对象/节/符号数和实际输出字节。统计包括核心请求的输入、元数据、脚本和输出分配；不含分配器元数据、规则 VM 堆、栈、文件系统缓存或进程 RSS。八对象、64 定义及指针样例在两个入口实测为 58,506/524,288 字节；这是样例而非任意工程峰值。

## RAM 与验收

现有 Wasm 线性内存上限 4 MiB，编译 arena 约 1 MiB。不得再添加 4 MiB 静态链接镜像缓冲。按实际数量分配元数据、逐对象读取、避免同时保留所有原文件和多份最终镜像；这仍需实际峰值验证，不能用静态 map extent 当作运行时余量。

验收包括：xhcc/GNU 两方向对象混链；两个 Flash/SRAM 布局执行；local 同名、weak/strong/COMMON 与未定义符号；函数/数据指针及非零/负 addend；跨 64 KiB 的独立 MOVW/MOVT；逐 relocation 与 GNU ld 对照；ET_EXEC/PT_LOAD 和 flat bytes；坏输入、错误 target/ABI、未知 relocation 和溢出失败且旧输出保留。无 archive 提取前，soft-float 测试需显式提供所需 runtime `.o`，不能假装已经支持 `.a`。
