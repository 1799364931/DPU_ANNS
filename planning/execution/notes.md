# ANNS v1 实施记录

2026-09-27 用户“执行计划”授权实施设计。P0–P5 无设备阶段已完成，交付与结果见 ../../docs/implementation_report_v1.md；复现见 ../../docs/build_and_run.md。

- 已有 json-c 和 sift-locality/NumPy 满足软件依赖，复用 CSR、PQ32、SIFT 矩阵及 c4096 聚类。只创建 ANNS 产物。
- 当前默认 sandbox 因 bubblewrap /mnt/wslg/distro 挂载失败；读写和构建使用 require_escalated，未发生自动审批拒绝。apply_patch 同样受此故障影响，最终修改改用范围明确的 Python 文本更新。
- 官方 DOCA 2.9.4005-1 x86_64/arm64-dpu 包和官方 Arm Ubuntu 用户态包校验后本地解压，不安装驱动。下载与校验清单保留在本目录。
- 软件共享搜索/协议/调度、模拟异步传输及真实 Comch/DMA 已实现。SDK 导出遵循 dma_copy 例程，显式传递远端 SDK 地址，不假定导入 mmap 可返回 Host 原地址。
- 全量转换等价检查通过。完整 ef32/64/128 回归共 30,000 条，29,996 条交接，全部与基线一致；最后统计修正后再次复核 300 条，并单独对照 baseline/off 各 100 条。
- 软件/Host SDK CTest、ASan/UBSan 通过；最终 Host 与 AArch64 SDK 编译日志无 warning/error。SDK 枚举 0 设备，未运行任何真机查询。
- 长回归使用阻塞子进程等待，不循环读取日志；按预计完成时间等待后收取结果。
- 源码、构建脚本、配置与二进制 SHA256/ELF 依赖见 build_manifest.json。已知真实环境待核对项写入交付报告，没有宣称硬件性能或 x86/Arm 精确一致。
- 工作目录不是 Git 仓库；未提交或推送。

## Google 格式与注释更新
2026-09-27：按用户要求，将 ANNS/.clang-format 改为 BasedOnStyle: Google，统一 34 个 C++ 文件；15 个 hpp 文件添加文件职责说明，在 109 处函数定义前补充注释。核对排除注释、空白和 include 排序后的 token，全部代码逻辑保持一致。四套软件/ASan/Host SDK/Arm SDK 构建通过且无编译警告，软件、ASan/UBSan 和 Host SDK 的契约测试通过。格式检查通过；更新源码与二进制构建清单。
