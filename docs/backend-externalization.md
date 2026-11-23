# 后端外置化状态

目标是所有后端目标专用规则与数据由独立包提供，主程序保留共享机制。当前不是完整目标的完成声明；以下区分已消费的外部规则和仍由C实现的算法。

## 当前生产路径

一个xhcc Wasm包含唯一C前端与IR。包用v3 manifest声明templates/encoding/selection，参数映射和资源按需读取，不运行子Wasm。

共享XHTEMPLATE1引擎提供有界键表、纯文本转义与${N}替换，不解释原生代码或printf格式。一次只驻留一个目标模板表，最大文件512KiB、4096条、16参数。字段编码引擎一次只驻留一个目标编码表，最多256条/64KiB，支持16位/32位散布和范围对齐检查。

## 已迁出

- ARM：551条发射模板，覆盖固定发射片段、完整标量binary/compare/unary、常量、SP调整、frame/label/load/store/branch/call参数模板。227条编码规则覆盖多种Thumb16、MOVW/MOVT、ADDW/SUBW、division、memory、logic、system/位操作、短宽分支、ADR/CBZ/CBNZ占位、MRS/MSR和VFP算术/转换/单多寄存器存取，以及LDM/STM、table branch、double memory、long multiply等asm-extra；同操作数形状的一部分指令到编码规则的映射也由模板键决定；访存形式由外部有序条件记录决定REG16/SP16/PC16/IMM16/REG32/WB32/POS32/NEG32，汇编两遍共用选择器。ARM caller/callee共享外部AAPCS参数字数、寄存器对齐和frame基准描述，并严格校验现有前端契约。标量浮点运算与转换的软/硬序列另由44条共享语义记录选择，64位整数运算、整数比较及软硬浮点比较另由56条记录选择，累计100条；硬件浮点比较修正NaN下LT/LE条件，另有13条复杂数四则、EQ/NE、共轭及实部/虚部/实数转换选择记录，累计113条；cabs/carg/cexp和部分complex ABI存取仍保留原路径。
- RV32：R/I/S/B/U/J编码布局；R型18条助记符到三寄存器编码规则的映射、寄存器别名及ABI参数寄存器名也由外部表消费，并以新助记符/寄存器别名同SHA执行验证；23条scalar32序列规则覆盖算术、位运算、移位、比较、signed/unsigned DIV/MOD，最多8步，兼容v1。另有347条基础共享模板规则，包括完整unary32、8/16位截断与符号扩展、访存宽度/符号选择和大frame地址序列，其余固定发射片段也已迁出。37条基础共享语义记录实际决定一元、转换、局部及索引访存和地址scale模板；另补宽整数及对齐复杂数索引访存完整选择，packed complex float8/double16路径已改为字节安全组装与分拆，偏移0/1/2/3有执行验证；float窄化辅助函数跨调用保留地址和IR值，C赋值表达式按目标精度转换另作验证。38条标量软浮点运算、比较和整数/浮点转换语义记录选择完整helper调用序列（辅助函数fixture验证仅覆盖选择与ABI，不是IEEE库正确性证明）。另有35条真实64位整数二元、比较和一元序列记录，包含纯RV实现的除法循环；16条复杂数运算/比较/转换/数学辅助调用选择记录提供完整序列，其辅助函数fixture仅验证选择与调用布局。调用寄存器数、字宽、栈对齐、参数字数分类和frame几何由必需ABI规则消费，允许经过边界校验的额外frame padding。
- i386/AMD64：固定发射模板、scalar32/AMD64slot64整段运算、寄存器名称/编码/宽度、ABI寄存器表、ALU/SSE/shift/group3/condition选择、固定opcode/prefix、REP表、ModRM/SIB/REX/寄存器opcode字段布局、word-pair转换模板。每个x86目标另有48条64位多字整数、宽转换及一元选择记录（含完整有界除法序列及带符号高字/无符号低字比较）和32条共享浮点语义记录选择SSE运算、比较和转换序列；18条有界first-match参数/返回值分类记录驱动寄存器bank、字数、宽度、间接传递等选择；ABI计数、frame/varargs/sret布局和聚合wire分类由必需数据描述并消费，与现有前端契约不匹配时拒绝。默认路径仍为既有xhcc聚合传参wire；显式-fuse-sysv-abi-plan已消费版本2认证标量计划并通过Clang双向标量互操作，并新增只对大小1–8字节、自然对齐、整数/指针成员结构的源类型认证与计划消费，并支持自然单SSE浮点结构及精确16字节long/long、long/double、double/long、double/double双字段结构。Clang双向参数/返回、独立bank编号、GPR/SSE耗尽整对象回退及canary已验证；其他未认证聚合仍拒绝，不能据此证明完整SysV eightbyte聚合ABI。
- C：249条语义选择记录及对应整条序列模板，包含102条运算、61条访存/控制和35条调用准备/参数绑定/路由/返回形状规则，以及44条类型/修饰符/声明符/属性生成政策及7条位域/SIMD序列选择；通用类型遍历有深度、循环和字段预算检查；由共享语义选择器查表，发射和栈效果共用所选记录；TRUNC栈效果修正为0。C栈上界分析改为有界CFG工作队列，精确传播深度并拒绝下溢、冲突汇合及增长循环；前端按IR字宽修正表达式丢弃的POP数量。原固定模板、runtime前缀、operator/type拼写、6类libc映射、结构返回类型映射；12条ABI和帧/栈政策数据。ABI值严格对齐当前前端，政策值在合法范围可变。

- 对象输出：ARM/RV32/i386/AMD64的ELF machine/class/endian/flags/ARM attributes和历史内部重定位键到标准R_*类型的映射来自object.rules；布局仍是共享C机制。XHRELOC1有限规则VM已接入legacy绝对/PC相对补丁、RV32 AUIPC成对补丁及ARM MOVW/MOVT与宽分支补丁；提供ABS64/PC64规则，最多16临时值、128操作、8字节scratch，按实际规则数分配，全部断言成功后原子提交。标准ELF外部符号仍交GNU/LLVM链接器处理。

- CPU/目标描述：ARM CPU名称、别名、Thumb2/FPU/DSP能力由cpus.rules提供；五引擎pointer/long、IR字宽、复杂数槽数、variadic模式和汇编能力由target.rules提供，前端实际消费并校验现有引擎契约。自定义同ISA CPU别名、目标身份和包宏无需重建主程序。

## 尚未完全迁出

ARM优化器、复杂IR/ABI/数据布局选择、少量位置/布局计算、剩余目标合法化与目标专用优化选择；RV32其余IR/ABI/汇编调度与重定位；x86聚合八字节class算法及部分frame/varargs/sret计算（描述已外置但通用计算仍C）、地址模式选择算法（ModRM/SIB/REX字段已外置）；C的IR规范化调度和通用有界类型图遍历（类型生成政策已由规则选择）；栈深分析现为共享机制性质的严格CFG传播，目标pop/push来自规则，不要求将通用工作队列翻译为目标脚本。标准对象布局、部分literal/address选择及addend准备仍C；RV成对、ARM分裂立即数与宽分支、x86本地绝对/PC相对引用公式已由有限VM执行。

因此目前所有后端均已有真实外部规则路径，但不能说所有后端算法已完全外置，也不能说任意新ISA仅靠现有规则即可加入。下一阶段需提取剩余目标描述和ABI，优先共享机制而非包专用主程序分支。

## 验证方式

运行tools/test.py。规则变体保持compiler SHA256不变，实际编译/链接/执行：ARM与x86操作加改减、ARM MOVW散布、x86 REX/REP/opcode、C生成程序原样交GNU ARM编译执行。坏版本、缺rule、占位/范围/对齐/容量错误须失败且不产有效对象。

约4MiB为任务RAM目标，当前linear memory cap4MiB；解释器字节码/元数据/宿主栈额外占用未在NuttX/WAMR证明，包文件大小不等于驻留RAM。
