# MicroPython 原 Makefile 的 MCU 构建验收

目标是从真实仓库的原 Makefile 编译 MCU 固件，而不是仅扫描大文件或用手写源列表替代。当前尚未达到该里程碑。本文基于本地 MicroPython 的只读审计；不同版本必须重新核对开关与生成链。

## 工作副本约束

原始 `D:/micropython/micropython` 仅作只读参考，不在其中生成文件或修改端口配置。实际构建使用 SD 下保持完整目录结构的 `sd/micropython` 副本，复制现有子模块源码与生成工具资源，排除 `.git` 元数据。复制来源与校验结果记录在 SD 外层清单；本次副本已核验 62,060 个文件、2,078,336,738 字节，size/SHA256 无差异，源 HEAD 与干净状态保持不变。本计划不把机器本地清单提交为通用源码。所有禁用冻结的可审阅配置变更、预生成文件、对象、固件和日志均留在副本，不能破坏原仓库。

## 首个候选与可跳过的宿主工作

首候选为 `ports/minimal/Makefile CROSS=1`，它实际面向 STM32F405、Cortex-M4、soft-float。这与现有 ARM M4 目标族匹配，但不是编译兼容性证明。

用户允许用变量/参数跳过宿主工具，只编 MCU 端；也允许宿主 Python 预生成。必须使用原工程真实开关或清楚的受检 artifact 策略，不能假设所有生成步骤都能跳过：

- `MICROPY_USE_COMPILER_PLUGIN=none` 可避开插件探测。
- `MICROPY_ROM_TEXT_COMPRESSION=0` 可关闭文本压缩生成分支。
- `FROZEN_MANIFEST=` 只影响 manifest，不会删除 minimal 固定的 `_frozen_mpy.c`。
- `QSTR_AUTOGEN_DISABLE=1` 不是禁用所有 qstr 规则和对象依赖的总开关。
- `mpversion.h` 仍有 FORCE 依赖，预先存在文件不等于原 make 不再执行规则。
- minimal 使用已有 `tests/assets/frozentest.mpy` 经 mpy-tool 生成，不必先构建 mpy-cross；其他端口的 manifest 可能需要它，已有工具可用 `MICROPY_MPYCROSS` 指定。

冻结 Python 代码本身是可选产品功能，不是 MCU C 工具链必须执行的步骤。首工程可采用无冻结代码配置；若所选端口没有现成开关，允许提供可审阅、明确记录的最小端口/Makefile 配置补丁关闭冻结，不能宣称该配置是上游 Makefile 无修改兼容。仍须从实际工程构建入口推进，不用手列源文件替代。`mpy-cross` 可由宿主 GCC/Clang 构建或使用匹配版本现成工具；只编 MCU 部分不要求在 MCU 上构建宿主工具。qstr、配置和版本等被实际 C 源码引用的生成结果仍需提供。

预生成产物须记录 MicroPython 版本、端口配置、生成命令、输入与输出摘要。缺失或过期必须报错；宿主步骤与 MCU 编译执行分别记录。当前执行器不运行 shell，不能把原生成器中的 shell/pipe/sed/touch/重定向自动视为受支持。

## 两条合作路线与主验收

允许宿主 GNU make 调用主机 Python 生成器，并通过 CC/LD 变量使用 xhcc/xhlinker。该路线保留原构建图和生成链，用来验证真实 MCU C、ABI、对象和链接能力；GCC 风格选项仍是受检子集，不能宣称直接替换所有 GCC 参数即可成功。

当前先以主机 GNU make＋宿主 Python＋xhcc/xhlinker 完成整体 MCU 固件构建，验证编译、ABI、链接及运行。固件链跑通后，集中测量并优化 RAM，先争取 5～6 MiB 总任务 RAM 的阶段目标，同时区分线性内存与运行时、宿主栈和文件系统开销；再完善 xhccm，让内置 make 接管已验证流程。宿主生成结果可预先提供，必须记录来源和配置。当前 xhccm 不运行任意 shell/Python 命令，未知命令继续明确拒绝；主机 make 成功不等于 xhccm 兼容完成。

## 按依赖排序的缺口

1. **Make 入口语义**：`lastword`、`MAKEFILE_LIST`、路径函数、quoted conditional、else-if 和 `info` 等。
2. **大图与长列表**：核心约 132 个对象，当前 make 128 词/2047 字节/64 KiB 状态不足。用顺序解析 journal、分页索引、流式 word iterator 和活动 DFS，不通过提高常量冒充有界。
3. **规则和作用域**：`define/call/vpath`、模式查找及生成头依赖图，正确保持 include、`:=` 与递归变量求值时机。
4. **生成阶段**：明确宿主执行/委派或可信预生成策略，不能由缺失文件直接略过；保持原 Makefile 为入口。
5. **编译器命令**：原 `-g`、`-ffunction-sections` 等当前未实现项须逐个支持或在明确工程配置中移除，不能静默忽略。
6. **真实 TU 语言/ABI**：启动 naked 函数、NLR、扩展 asm、alloca 等需要实际原源文件编译验收。
7. **链接规模**：当前标准 linker 上限 32 个选中对象、512 sections、8192 symbols、512 KiB requested heap/output，尚不足覆盖原核心 132 个对象。需要逐对象/成员读取和受测的规模设计，而非一次读入全库。
8. **脚本与打包**：已有受检 `-T`，应逐项验证原 F405 脚本；map/cref/GC、运行库、size/objcopy/DFU 等命令仍需明确接入范围。

## 当前真实工程进展

SD 副本新增可选 `XHCC_MCU_ONLY=1` profile 关闭冻结，默认 profile 不变；补丁、原文件摘要和生成命令保存在 SD 证据目录，不改原仓。经原 Makefile 的宿主 Python/GNU 工具生成了六个顶层头，219 个输入和 49 个生成 artifact 摘要可校验；版本信息固定到复制源提交。

内置 make 已补 `MAKEFILE_LIST`、`lastword/abspath/info`、quoted conditionals/else chains、变量开头的目标规则；变量成功替换会释放旧 owned 值，40 项回归通过。原 minimal 入口仍在完整 OBJ 列表展开处触发 2047 字节限制；`OBJ=` 仅用于定位后续 `vpath` 缺口，不是有效工程验收。

实际非空 `py/obj.c` 在同一显式 `-O0` MCU profile 下由 GNU ARM 编译成功（该批 text=2864 字节）。xhcc 首次缺标准头和端序宏；补齐 assert/端序及 alloca 包含接口后，实际 `-E` 已成功输出 24961 行，随后 parser 在位域宽度 `8 * sizeof(size_t) - 1` 等表达式失败。下一批已修复受支持的位域常量表达式并通过六项专项；attempt-3 的同一 `obj.c` 已越过该诊断，但编译与 `-fsyntax-only` 都在 IR 生成前触发 `memory pool exhausted`，仍未产生 xhcc 对象。`alloca` 的类型和参数识别已有测试，但执行保持明确拒绝，直到动态栈活跃值与 ABI 对齐验证完成。不能把头文件可包含等同 builtin 已可执行。

独立 expanded 开发配置的 attempt-4（2 MiB arena／8 MiB linear）已越过 OOM，但函数内 `MP_STATIC_ASSERT_NONCONSTEXPR` 的 `sizeof(char[...])` 变长 type-name 被拒绝，尚无对象。该宏有意接受非 ICE 的 extern 对象地址比较，不能用定义别的编译器宏跳过。函数内 VLA `sizeof` 已实现受支持的运行时路径并通过六项专项；后续真实取证显示 parser、sema、IR 均完成，RAM 优化回收不可达分析临时 CFG 后，attempt-8 同预算首次完成 opt。attempt-9 复用既有汇编输出缓冲后，emit 也完成且 arena 净增为 0；当前 assemble 申请 1536 字节失败，仍无对象。attempt-10 使用独立 4 MiB arena／16 MiB linear 后，真实 `obj.c` 首次编译 exit 0，并核验为 ELF32 little-endian ARM EABI5 ET_REL：`.text` 9946 字节、六个核心函数有实际定义、192 条已识别标准 ARM 重定位。arena peak=2154808 字节，比 2 MiB 多 57656；对象尚未链接或执行，不代表固件完成或默认 RAM 达标。默认预算不变；详细阶段数据见[资源预算](resource-budget.md)。

先以主机 GNU make＋Python＋xhcc/xhlinker 推进真实 TU 和固件链，再逐步完善 xhccm 的流式长列表/大图。用户允许独立 expanded 开发 profile 临时增加 arena 与线性内存以先跑通，再按阶段测量优化回目标；默认受限配置及 SD 产物必须保留，不能用扩大预算后的成功冒充原 RAM 目标已达成。每次新 TU 尝试保存独立 artifact 和编译器摘要，不能覆盖旧失败证据。

## 主机 make 批次与隔离执行

第一批实际通过原 GNU make 对象规则调用 Python CC 启动器和 Wasmtime：八个目标中 `obj/nlr/map/lexer/argcheck` 五个产出非空 ARM ET_REL，`objint/runtime/parse` 三个因宏参数上限或缺 `unistd.h` 失败；GNU 同配置八个均通过。相同目标增量重跑仅重试三个失败目标，五个成功对象 SHA 不变，生成的 `.P` 依赖可被原 make 再次读取。宿主先生成且校验头文件，再在对象批次中固定终端生成头；不是 xhcc 独立完成生成链或固件。

真实 `obj.c` 三条合法 tagged-small-int 路径另做隔离执行：xhcc 和 GNU 对象各 66 个案例与独立数值预期一致，检查返回值、输出指针、callee-saved 寄存器及 SP。此测试的完整对象严格链接仍因未解析外部目标失败，诊断映像使用 `--noinhibit-exec` 保留，仅允许执行已定义白名单函数及已验证内部重定位，未解析路径不执行。它不是完整链接、整个 TU 运行或固件正确性证明。

第二批在新 BUILD 重新生成头并编译八个对象：64 参数／每层 8 KiB 总 scratch 与受限 `unistd.h` 已消除旧预处理阻碍，但仍为 5/8。新失败分别为 `objint` 的类型初始化器 designator、`runtime` 的 GNU 零长度字段、`parse` 的 ELF 对象写出泛化错误（peak=3205744 小于 4 MiB，不能称 arena OOM）。GNU 同配置仍 8/8，原五个成功对象逐字节不变。符号名字借用的同预处理器反事实测试独立减少 `obj.c` arena peak 29056 字节（2154808→2125752）；不是整个工程 RAM 目标已达成。首两批证据均保留，后续按新的语言和对象写出缺口推进。

第三、四批已把同一门槛推进到 6/8，`parse` 首次产出 `.text` 15090／`.data` 1188 字节的标准 ARM 对象。GNU 尾部 `[0]` 与聚合整数 AST 求值已通过专项；`runtime` 的下一阻碍是明确的动态栈支持，接入认证 CFG 栈深后又暴露独立的 128 KiB 汇编缓冲上限，不是 arena OOM。`objint` 经私有诊断确认实际类型布局完整，真正剩余阻碍是 `##` 实参过早展开损坏了初始化器；不能继续归因于柔性尾 extent。

当前新增受限 ARM Thumb-2 `alloca`、原始 token-paste 后重扫描，以及独立 `--asm-kib` 开发容量配置。第五批在 4 MiB arena／16 MiB linear／512 KiB 汇编文本预算下推进到 7/8：`objint` 首次成功，`.text` 8818、`.data` 152 字节；`runtime` 已通过 emit，下一阻碍是 assemble 的输出/重定位缓冲范围错误，并非 arena OOM。原 139 对象的 keep-going 全编已完成：xhcc 120/139，GNU 同配置 139/139。19 个失败为 include 深度 8 项、IR arena 容量 2 项、alloca CFG 2 项、汇编/重定位 2 项、泛 ELF 写出 2 项，以及 extended asm、parser、重复声明各 1 项。61 个 GNU 空对象在 xhcc 仅多出弱 memcpy helper，未发现被关闭模块函数误发射。完整批次模块、配置和 235 项包资源哈希前后一致；未混用 GNU 对象，也未用缺失对象尝试固件链接。链接端 32 对象上限与未引用 extern 符号冗余另有明确扩展需求。未完成对象集合、链接和执行前，不称完整固件成功。

第六批全新构建已到 128/139；第七批修正真实 include guard 观察、宏注释、void/comma 栈平衡、暂定定义、聚合地址、reserved unreachable 和 GNU 警告/asm 输入后，**原 GNU make 的 139 个对象全部实际编译成功，make exit 0**。开发配置为 arena 8 MiB、linear 32 MiB、ASM 2 MiB、code buffer 1 MiB；最大单 TU arena 为 `compile.o` 的 6042144 字节，不代表总任务 RAM 已到 5～6 MiB。所有对象来自同一固定模块，未复用旧 xhcc 对象或混入 GNU 对象，源与生成头参考校验、包资源前后哈希均通过。

独立 project linker 已读入该 139 对象集合：1948 sections、22284 symbols，请求 heap peak 1227839/4194304 字节。原 GNU 参数因 `-nostdlib` 被明确拒绝；声明 no-GC/no-map/no-cref 的 `-T` 适配后，首个真实链接缺口是原 STM32F405 脚本 `.data : AT(_sidata)`（line 38），尚无 firmware 输出。原脚本和对象顺序未改，未添加伪运行库或覆盖原仓。显式加载地址脚本语义已实现并通过 GNU oracle 与旧 ARM/RV 链接回归。第二次真实链接完整通过原脚本，进入符号解析，首错为 `__builtin_clz`，仍无固件。只读检查全部 alloc 重定位后的真实未解析集合为八个名字：`__builtin_clz`、`__builtin_expect` 和六个 ARM EABI 64 位辅助函数，共 41 个引用；不把未引用 extern 声明误报为缺口。`clz/expect` 已实现通用 IR 下沉并通过五目标及 ARM/C 执行专项；第八批全新 139/139 对象成功，实际 alloc 重定位中两个 builtin 外部引用为零，仅剩六个 EABI 名字。另提取准确 Cortex-M4 soft-float multilib 的八个真实 libgcc 成员形成闭包，保留许可证和来源限制，未冒充 xhcc 生成代码。PREL31 与受检 CANTUNWIND EXIDX 支持后，第三次链接使用 147 输入和独立、有 diff 的 SD 脚本配置，未删除异常元数据；实际首先拒绝 `pyexec.o` 中超出代码节 10 字节的本地标签。汇编器已统一字符串解码并完成全新 139 对象及 8956 个分配段定义符号范围检查；随后 147 输入的真实 make LD 与 size 均 exit 0，首次产出 ELF（1360836 字节）和 xhlinker BIN（534168 字节）。BIN 与 ELF Flash PT_LOAD 展平逐字节一致；GNU objcopy 少最后 4 字节对齐填充，严格文件相等仍为 false，未裁剪掩盖。

ELF 向量表、Thumb 入口、数据 LMA、Flash/RAM 范围和保留的异常索引结构检查通过，但独立 Unicorn Reset 启动失败：第 6 条指令在 naked Reset_Handler 内通过未建立的 r6 栈帧写地址零，尚未进行数据复制、BSS 清零、MMIO 或到达 main。没有预置 r6、预复制内存、跳过启动或修改 ROM。下一步修编译器启动代码生成；文件产出与结构成功不等于可运行固件。

## 下一批实施顺序

- 完成默认流式前端的旧功能兼容回归与输出故障清理。
- 从原 minimal Makefile 做依赖发现与 GNU dry-run 对照；此步骤不是完整构建。
- 经原生成器在宿主预生成必要文件，冻结版本/配置/摘要，制定 MCU 端拒绝陈旧 artifact 的合同。
- 从一个原 TU 扩展到全部 MCU 对象，逐项修复语言、ABI、flags 和容量问题。
- 使用原链接脚本完成对象链接与打包，验证符号、重定位、初始化及固件启动。
- 真机验证 STM32F405 UART REPL、异常路径及源/头/配置变化后的增量构建。

资源分项测量：文本窗口/scratch、宏、稳定 token、AST/IR、make journal/cache/活动图、链接 metadata、临时磁盘及运行时。大注释小 AST 的流式测试只是前置；不能由它推导整个仓库在 4 MiB 任务 RAM 内可构建。

参考：[流式前端](streaming-frontend-plan.md)、[make 子集](make-subset.md)、[链接器](elf-linker-first-stage.md)、[归档计划](elf-archive-plan.md)、[总体规划](progress-and-roadmap.md)。
