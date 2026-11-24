# 源码来源

C 前端、IR 与 ARM 发射器的初始源码来自本机参考工程 `../arkcc/`，复制于 2026-11-22。
代码现按 xhcc 自有部件组织在 `src/frontend/c`、`src/ir`、`src/backend/arm`、`src/core` 和 `src/diagnostics`，没有 vendor 层。
arkcc 原作者的版权声明 `Copyright (c) 2026 春雫` 与 MIT 授权全文完整保留于 [LICENSES/arkcc/LICENSE](LICENSES/arkcc/LICENSE)，该文件逐字节保留原许可证。arkcc 继承内容及其衍生修改仍须保留该版权声明与许可全文，随源码或其重要部分的分发一并提供。
参考仓库基线提交：`e9699cc4caed5dcb5ea949291e943384be47b854`；复制时工作区干净。xhcc 的初始导入提交记录复制基线及后续修改。

xhcc 在此基础上开发受限内存配置、WASI 平台、命令兼容层、外部包协议与标准 ARM ELF 输出。

根目录 [LICENSE](LICENSE) 中的 `Copyright (c) 2026 Taohan Wang <tianqi2021_001@163.com>` 及 MIT 授权适用于 xhcc 自有新增内容，不取代 arkcc 继承内容的原始版权声明和授权。许可原文集中收录于 [LICENSES](LICENSES/README.md)；分发继承内容时应同时提供相应的原始许可证。
