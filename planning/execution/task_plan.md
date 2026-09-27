# ANNS v1 实施进度

授权：2026-09-27 用户“执行计划”，允许按 program_design_v1.md 编写代码和执行验证；不提交或推送。

- [x] P0：环境、输入与真实 SDK 获取路径核对。
- [x] P1：离线簇连续图、映射、PQ、manifest 与等价性验证。
- [x] P2：共享搜索核心、预测、FP32 重排和完整 Host 基线。
- [x] P3：交接状态/消息、模拟异步读取、全预取屏障、软件回归。
- [x] P4：真实 SDK 的 Comch/DMA 实现及 Host/Arm 编译链接。
- [x] P5：benchmark、完整查询集与 ef 回归、交付说明。

## 决策
- 复用 sift-locality 已有 Python/NumPy；C++17/CMake，已有 json-c 用于 JSON，无需下载 JSON 库。
- 基线 SIFT-1M，复用 c4096 聚类（已存在匹配资产），默认 ef64/w0.75/x0.25。
- SDK 2.9.4005-1 已从 NVIDIA 官方 2.9.4 仓库下载，校验 SHA256 并解压到 ANNS；没有安装驱动。

## 问题
- 文档迁移误用系统 Python 导入 NumPy；已切换 sift-locality 解释器，未安装依赖。
- 默认沙箱 bubblewrap 宿主挂载异常，使用 require_escalated。
- 当前不是 Git 仓库。
- Arm 链接器需本地 libbfd 路径；旧 CMake 缓存随后用 --fresh 刷新。显式 SDK 路径增加 NO_CMAKE_FIND_ROOT_PATH，避免被错误重定向进 sysroot。
- 首次 Arm sysroot 下载发生 TLS EOF；已缓存成功包，采用官方索引校验的下载重试/镜像。
- 首次 Arm 链接器下载工作目录尚不存在；先创建目录后下载成功。

## 状态
2026-09-27：P0–P5 无设备阶段完成。全量数据校验、30,000 条软件基线对照、最终统计修正后的 300 条复核、独立 baseline/off 各 100 条，以及 CTest/ASan/UBSan 通过。真实 SDK Host x86_64 与 SoC AArch64 均编译链接通过，最终日志无 warning/error。SDK 枚举 0 个设备，真机 DMA、故障清理、性能及目标 BFB/CloudLab 内核仍待核对。

交付报告见 ../../docs/implementation_report_v1.md，操作说明见 ../../docs/build_and_run.md，详细证据见 notes.md 和本目录 JSON/日志。
