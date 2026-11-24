# ARKOBJ 与 legacy 脚本路径的定位

这是对本地原始 arkcc 和当前 xhcc 实现的只读核查结论，不是两个格式的 RAM 基准测试。

## ARKOBJ 是否更省 RAM

不能仅由格式简单推断节省 RAM。私有 ARKOBJ 的字段较少，但已有 legacy 实现使用固定大小名称表和镜像缓冲；标准 ELF 链接器则按实际对象分配，并有受检的 requested-heap 预算。对象文件大小、链接峰值内存和最终程序运行 RAM 是三个不同指标。

目前保留 ARKOBJ 的理由是兼容已有对象、私有布局元数据和可选 IR/LTO。它不是 MCU 编译或链接不可替代的格式；未来可将这些用途迁到标准对象自定义节或独立版本化 IR 容器，再评估降为可选兼容功能。在替代路径完整前不直接删除已有能力。

公平比较应使用相同工程、目标、优化、布局和容量配置，普通链接与含 IR/LTO 链接分开。记录输入/输出字节、对象/符号/重定位数量、静态工作区、requested heap 峰值、栈水位及运行时/设备总内存，并先验证输出行为一致。当前新 ELF 链接样例测量见资源测试，不等于已经和 ARKOBJ 做过等功能比较。

## 原 arkcc 的 .ld/.sct 能力

原始 arkcc 确实读取脚本文件，存在 `-T`、`--linker-script`、`--script` 和 `--scatter` 等分词选项。它不是只保存文件名，但也不是 GNU ld 兼容解释器。

其 `.ld` 路径解析数值型 MEMORY ORIGIN/LENGTH、SECTIONS 的名称、可选显式数值地址、selector 原文及区域名称，再转换成有限 ScatterDesc。MEMORY 属性被跳过；AT> 名称未完整驱动加载地址布局；缺省节地址不按区域游标自动分配，size 不通过输入节布局计算。部分 selector 匹配回退到原文子串，不能将文本中出现 KEEP/ALIGN 等字样视为已执行语义。表达式、点计数器、符号赋值、ADDR/LOADADDR/SIZEOF、ASSERT 等不能按 GNU ld 语义期待。原解析对部分未知关键字跳行，也不能作为可靠的语法支持证明。

`.sct` 路径可表达 LR_/ER_/RW_/ZI_ 数值区间和有限属性/selector 摘要，主要为原固定布局与重定位站点服务；不能凭脚本为原对象补出其未记录的重定位。原私有链接器有 weak/strong 决议，但不是标准 ar 自动提取和 ELF COMMON 合并实现。

## 当前 xhcc 的标准路径

新标准 ARM ELF 链接器独立实现有界输入读取、符号/节布局、六类 ELF REL、受检 MEMORY/SECTIONS/表达式及 ASSERT 子集，并实际消费 VMA/LMA 和启动相关符号。它与 legacy scatter 路径分别维护，不能用一条路径的能力替另一条作保证。仍不宣称完整 GNU ld、任意板级脚本或自动提供启动代码/运行库。

接口和范围见 [标准链接器首阶段](elf-linker-first-stage.md)、[进度与规划](progress-and-roadmap.md)；原始版权与许可见 [NOTICE](../NOTICE.md) 和 [arkcc MIT](../LICENSES/arkcc/LICENSE)。
