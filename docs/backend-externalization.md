# 后端外置化状态

目标是所有后端目标专用规则与数据由独立包提供，主程序保留共享机制。当前不是完整目标的完成声明；以下区分已消费的外部规则和仍由C实现的算法。

## 当前生产路径

一个xhcc Wasm包含唯一C前端与IR。包用v3 manifest声明templates/encoding/selection，参数映射和资源按需读取，不运行子Wasm。

共享XHTEMPLATE1引擎提供有界键表、纯文本转义与${N}替换，不解释原生代码或printf格式。一次只驻留一个目标模板表，最大文件512KiB、4096条、16参数。字段编码引擎一次只驻留一个目标编码表，最多256条/64KiB，支持16位/32位散布和范围对齐检查。

## 已迁出

- ARM：551条发射模板，覆盖固定发射片段、完整标量binary/compare/unary、常量、SP调整、frame/label/load/store/branch/call参数模板。166条编码规则覆盖多种Thumb16、MOVW/MOVT、ADDW/SUBW、division、memory、logic、system/位操作和VFP算术/转换/存取。
- RV32：R/I/S/B/U/J编码布局；23条scalar32序列规则覆盖算术、位运算、移位、比较、signed/unsigned DIV/MOD，最多8步，兼容v1。
- i386/AMD64：固定发射模板、scalar32/AMD64slot64整段运算、寄存器名称/编码/宽度、ABI寄存器表、ALU/SSE/shift/group3/condition选择、固定opcode/prefix、REP表、REX位描述、word-pair转换模板。ABI计数、frame/varargs/sret布局和聚合wire分类由必需数据描述并消费，与现有前端契约不匹配时拒绝。这仅覆盖既有xhcc聚合传参wire，并不证明完整SysV eightbyte聚合ABI与GCC互操作。
- C：357条模板（含40条参数化）、runtime前缀、operator/type拼写、6类libc映射、结构返回类型映射；12条ABI和帧/栈政策数据。ABI值严格对齐当前前端，政策值在合法范围可变。

- 对象输出：ARM/RV32/i386/AMD64的ELF machine/class/endian/flags/ARM attributes和历史内部重定位键到标准R_*类型的映射来自object.rules；布局与实际patch算法仍是共享C机制。

## 尚未完全迁出

ARM优化器、复杂IR/ABI/数据布局选择、branch/VFP多寄存器和部分asm-extra编码及重定位；RV32其余IR/ABI/汇编调度与重定位；x86聚合八字节class算法及部分frame/varargs/sret计算（描述已外置但通用计算仍C）、ModRM/SIB机制；C的IR控制调度、类型递归、栈深分析。标准对象布局和重定位补丁算法仍C；已写明下一阶段有限补丁VM方案。

因此目前所有后端均已有真实外部规则路径，但不能说所有后端算法已完全外置，也不能说任意新ISA仅靠现有规则即可加入。下一阶段需提取剩余目标描述和ABI，优先共享机制而非包专用主程序分支。

## 验证方式

运行tools/test.py。规则变体保持compiler SHA256不变，实际编译/链接/执行：ARM与x86操作加改减、ARM MOVW散布、x86 REX/REP/opcode、C生成程序原样交GNU ARM编译执行。坏版本、缺rule、占位/范围/对齐/容量错误须失败且不产有效对象。

约4MiB为任务RAM目标，当前linear memory cap4MiB；解释器字节码/元数据/宿主栈额外占用未在NuttX/WAMR证明，包文件大小不等于驻留RAM。
