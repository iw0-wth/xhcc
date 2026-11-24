# ARM ELF 显式 archive 下一阶段计划

本文是下一阶段合同，不代表当前链接器已经接受 `.a`。当前 linkcore 输入仍是显式 ELF `.o`，按实际请求堆字节统计并限制为 512 KiB。首阶段只新增显式 archive 路径，不增加 `-l`、库目录搜索、whole-archive 或 archive group。

1. **先补范围读取，不缓存整个 archive。** 当前 `fs_read_file` 与 core `load()` 都读取整个输入文件。下一步需要各目标文件系统提供受检的范围读取能力，例如 `fs_read_range(path, offset, buffer, count)`，精确返回已读字节；由调用方检查文件长度、加法溢出和短读。不得用“先把整个 archive 放入缓冲，再切片”实现。对象来源改为 `ObjRef {path, member_header_offset, payload_offset, payload_length, fingerprint}`，显式 `.o` 使用整个文件的 span。archive header、索引及长名表采用小块读取或受预算限制的临时缓冲，所有 malloc 均接入既有 peak/requested-heap 计量。范围读取接口及其各目标 backend 是实现成员提取的前置任务。

2. **首版仅接受普通 GNU ar，逐字段校验。** 支持 `!<arch>\n`、60 字节成员头、短名、GNU `//` 长名表及 `/decimal-offset` 长名引用，以及 GNU `/` 的 32 位大端符号索引。BSD `#1/length` 名称、BSD 索引、GNU 64 位索引、thin archive 均明确拒绝，不猜格式。检查十进制数字字段、成员头结尾、成员长度与偏移溢出、文件边界及偶数字节 padding；长名引用必须指向合法名称起点，并在长名表范围内遇到 `/\n` 终止；索引计数、offset 数组、NUL 名称串均检查边界，每个索引成员偏移必须指向已验证的实际成员头。建议 archive 成员数上限 4096、索引与长名各上限 64 KiB；这些上限不替代总堆预算，也不要求把所有索引名复制进常驻 metadata。

3. **明确从左到右及 archive 内 fixed point。** 显式 `.o` 按输入顺序加入；遇到 `.a` 时，只为当前实际所需、尚未满足的 strong 引用及 entry 提取成员，再根据新增成员产生的需求在同一 archive 内迭代，直到没有新成员，成员选择按 archive 文件顺序确定且每个成员仅提取一次。需求来自 alloc relocation 的实际引用，不能因为一个未使用的 undefined 表项而提取。undefined weak 不触发提取；既有 strong、COMMON、weak 定义及脚本定义如何满足名称，遵循已明确的符号决议，不为“也许能找到更强定义”额外抽取。后续 `.o` 或其它 `.a` 新增的需求不自动重扫此前 archive；跨 archive 循环依赖和 `--start-group` 不在首版范围。若 entry 由脚本 ENTRY 指定，必须在提取开始前确定；脚本赋值名称也必须先登记为可满足名称，值仍可留待最终布局求出。

4. **沿用成员级两遍和失败保旧合同。** 第一遍只对选中成员读取完整 payload，调用共享 ELF reader 与 Thumb 校验，复制必要 metadata/名称，记录成员 fingerprint，然后释放 payload；不能保留借用 view/name/data 指针。第二遍按 `ObjRef` 重新范围读取同一 payload，复核长度、成员 span 与 fingerprint，复制内容并重定位，之后立即释放。archive 文件长度和 header span 也须重新核验；未选中 payload 不影响输出，选中成员变化必须失败。32 个对象的上限包含显式对象与提取成员，总 sections≤512、symbols≤8192、名称≤128 KiB、输出≤512 KiB，以及默认总请求堆≤512 KiB 继续统一执行；成员索引、脚本、metadata、输入及输出缓冲均计入真实峰值。全部输入、布局、ASSERT 与重定位成功后才调用原子输出，不产生提前截断或部分替换。

5. **先验证提取，不扩大 relocation/runtime 宣称。** 首批测试用只有两个成员的自制 GNU archive，主对象仅引用其中一个：确认只提取所需成员、未提取成员不引发重复定义或未支持内容错误；再加反序依赖链验证 fixed point、undefined weak/unused undefined 不提取、输入次序、长名与坏索引、预算失败及旧输出保留。接着用本机真实 libgcc archive 验证少量整数除法或软浮点加减 helper 的按需提取与执行，报告实际 peak；不创建与 archive 总大小等大的缓冲。本机 GNU ARM 15.2.1 Thumb v7-M nofp libgcc.a 为 1,640,864 字节、1767 成员，最大成员 17,104 字节，GNU 索引 27,390 字节，长名表 11,318 字节，适合作为此项约束的实际验收样本。

现有六类 ARM REL 不因 `.a` 支持而自动扩展。审计发现常见 v7-M `_udivsi3.o`、`_divsi3.o`、`_arm_addsubsf3.o` 和 `_arm_addsubdf3.o` 使用现有 relocation 子集；整数除法还需按需提取提供 weak Thumb `__aeabi_idiv0` 的成员。全库另外出现 `R_ARM_NONE`（0）和 `R_ARM_PREL31`（42），v6-M 另见 `R_ARM_REL32`（3）；异常展开成员还使用 `SHT_ARM_EXIDX` 和 `SHF_LINK_ORDER`。这些必须分别设计、对照验证再开放，不能仅加 PREL31 补丁就宣称支持 EXIDX 或完整 libgcc。未被选中的 unwind 成员不应导致整个 archive 被拒；实际提取到不支持的成员则明确报 archive/member 诊断，并保留旧输出。
