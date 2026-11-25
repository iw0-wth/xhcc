# NuttX/WAMR 文件访问与输出发布

本说明用于配置 xhcc 的 WASI 文件接口。它依据工作区审计文件 `NUTTX-WAMR-XHCC-API-COMPAT.md` 的源码调用链分析，并非真机验证报告。原审计文件不属于本仓库，因此这里保留版本和关键结论，不链接外部工作区路径。

| 来源 | 核对版本 / commit |
|---|---|
| NuttX releases/13.1 | `4268617f28ad0ed7e900a081ea1b33c9347458b3` |
| NuttX master 快照 | `d729a200002ba9b5551c8f319d284e09504cd980` |
| nuttx-apps releases/13.1 | `3c772ea35b9245972e18999828f7b533e6fc57cc` |
| nuttx-apps master 快照 | `f0dc662c0152028d2796beb6b8533be26cf079d2` |
| 本地 WAMR 调用链 | `fd3c104`，2026-11-19 快照 |

正式部署应固定实际测试的 NuttX、nuttx-apps 和 WAMR 版本。NuttX 下载的 WAMR 版本可能与本地调用链审计快照不同；不能仅使用浮动的 `main` 或默认版本就认为已验证兼容。

## 配置与目录

启用 WAMR 的 WASI libc，并按实际 NuttX 版本核对相关配置项：

```text
CONFIG_INTERPRETERS_WAMR=y
CONFIG_INTERPRETERS_IWASM_TASK=y
CONFIG_INTERPRETERS_WAMR_LIBC_WASI=y
```

所审计的 NuttX master 还需要 `CONFIG_FS_IOCTL_FILEPATH=y`。WAMR 使用预打开目录的 dirfd 调用路径接口，NuttX 再经 `F_GETPATH` 还原目录；关闭此选项会使相关 `openat/statat/renameat/unlinkat` 路径失败。`DEFAULT_SMALL=y` 时尤其要显式检查。releases/13.1 没有同名配置门，但文件系统仍须实现相应路径查询能力；xhcc 的 C 文件接口不能绕过该部署约束。

给 WAMR 预开放源码、包、输出与临时文件所在目录，并保证临时目录可写且有足够空间。示例使用 guest 可见路径：

```sh
iwasm --dir=/mnt/sd \
  --env=XHCC_PACKAGE_ROOT=/mnt/sd/packages \
  --env=XHCC_TMPDIR=/mnt/sd/tmp \
  --env=XHCC_OUTPUT_POLICY=no-replace \
  /mnt/sd/xhcc.wasm arm-none-eabi-gcc -c \
  /mnt/sd/src/add.c -o /mnt/sd/out/add-build-001.o
```

各版本 iwasm 的 `--dir`/`--map-dir` 和环境变量语法需按所固定版本确认。桌面参考宿主 [tools/xhcc.py](../tools/xhcc.py) 会把宿主环境中的 `XHCC_TMPDIR`、`XHCC_OUTPUT_POLICY` 显式传给 guest；路径仍必须处于允许访问的目录映射中。

宿主任务栈与 Wasm 线性内存中的 C 栈是不同预算。原调用链分析建议以 64 KiB 宿主 WAMR 任务栈作为待测起点，不能将它视为已经证明的最低值，也不能与应用构建的 Wasm C 栈大小等同。请在目标板、解释器/AOT 模式和代表性编译任务上测量栈与总内存。

## 选择输出发布策略

[flash_fs.h](../include/flash_fs.h) 和 [文件适配器](../src/platform/wasi/flash_fs_host.c) 为普通文件写入与 `fs_spool_publish` 统一使用以下策略：

| `XHCC_OUTPUT_POLICY` | 行为与前提 |
|---|---|
| 未设置或 `atomic-replace` | 完整写入、flush、close 临时文件后，调用平台替换接口。只有已经验证其覆盖和失败行为的文件系统才能支持所需原子可见性；名称不是能力探测或平台保证。 |
| `no-replace` | 发布前发现目标存在就返回错误，不调用 rename。仅目标明确不存在时才继续。要求本任务独占输出名称，检查至 rename 完成期间没有其他写入者。 |
| 空值或其他值 | 配置错误，拒绝发布，不静默回退。 |

所核对的 NuttX 13.1 VFS 在覆盖 rename 时先删除目标。master 快照先尝试文件系统 rename，失败且目标存在时删除再重试；FAT 的已有目标会触发此路径。因此这些 NuttX/FAT 组合不能依靠普通覆盖 rename 满足“失败保留旧文件”。部署时应显式使用 `no-replace`，不能通过一次成功覆盖试验自动推断失败时也安全。

`no-replace` 会拒绝已有输出，直接重复覆盖的增量构建因此不能照旧运行。可使用每次构建独立目录或带版本的对象/镜像名称，或由宿主采用经过验证的发布接口完成更新。不要用“备份旧文件、删除或改名、再发布”的流程冒充原子替换。

存在性检查加 rename **不是原子 rename-noreplace**。WASI Preview1 不提供该并发保证；其他任务在检查之后创建同名文件仍可能受底层 rename 行为影响。必须独占输出名称；需要并发更新时应由宿主提供并验证更强的发布原语。

flush/close 成功也不等于数据已经在断电后持久保存，本接口没有 fsync 或存储事务保证。正常错误会清理临时文件；trap、强制终止或掉电可能留下临时文件，恢复流程只能清理确认属于失败任务的临时数据。

## 部署验收

先验证包扫描、预打开目录及小文件编译，再验证实际输出策略：

1. 在 `no-replace` 下首次写入新的版本化输出应成功；重复写入应失败，旧文件内容保持不变。
2. 对临时文件写入、flush/close、rename 路径注入失败，分别检查普通输出和 spool 发布，确认旧内容及临时文件清理行为。
3. 核对实际 NuttX VFS 与文件系统源码及配置；若采用 `atomic-replace`，还需在该组合上验证覆盖失败语义，不能只验证成功路径。
4. 记录真实 WAMR 版本、运行模式、预开放目录、栈设置和内存峰值。

仓库的 [发布策略测试](../tests/test_output_policy.py) 使用 WASI 文件和模拟的破坏性 rename 检查两种发布 API，包含拒绝已有目标时 rename 调用为零的断言。这些测试验证适配器合同，不代表 NuttX/FAT 真机已经通过上述验收。
