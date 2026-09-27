# ANNS v1 实施与验收报告

日期：2026-09-27。用户“执行计划”后完成 [正式计划](../implementation_plan_v1.md) 的无设备阶段 P0–P5。软件闭环可运行，真实 DOCA Host/SoC 后端已编译链接；没有执行真机查询或性能验收。

## 实现内容

- **数据准备**：稳定按 `(cluster_id, dataset_id)` 重编号，生成定长图、双向 ID 映射、簇目录与重排 PQ。源 `correct` / `predict` 资产只读使用，不重新构图或训练。
- **共享搜索**：C++17 的 `CandidateSet`、精确 uint8 `visited`、PQ32 距离、搜索/预测、全候选 FP32 重排。结果转换回数据集 ID，内部搜索保持图 ID。
- **Host/SoC 调度**：单在途查询；Host 在触发节点扩展前暂停，冻结簇和完整状态。SoC 读状态及全部预测簇，经过就绪屏障后续搜；缺失图记录和重排向量分别取数。未触发查询在 Host 完成。
- **协议**：显式小端字段，session/query/布局身份校验，INIT、READY、SUBMIT、COMPLETE 与失败消息；查询原向量、LUT、候选距离/expanded、全部 visited 和扩展预算原样交接。预留簇反馈消息类型。
- **传输**：交接预取、缺失补读、重排读取分别绑定策略。模拟后端真实复制字节，并产生延迟/逆序完成、背压、失败事件。DOCA 后端实现 Comch、PCI 映射导出/导入、DMA task 与 progress。
- **统计**：off/basic/trace 模式，逐查询 JSONL、汇总 JSON、配置快照与结果整理工具。off 关闭可选阶段计时与计数；保留查询结果和端到端时间。

主要目录：`include/anns/` 公共接口，`src/{common,index,search,protocol,data,transport,runtime}/` 实现，`apps/` 四个薄入口，`tools/` 转换/验证/回归，`configs/` 参数，`cmake/` SDK 和交叉构建，`tests/` 契约测试。具体操作见 [构建与运行](build_and_run.md)。

## 数据验收

产物：[SIFT-1M manifest](../artifacts/sift1m-c4096-v1/manifest.json)。N=1,000,000，D=128，4096 簇，PQ32，Rmax=32，邻接记录 132 字节。入口由数据集 ID 123742 转为图 ID 501796。

数据身份：`cd1d0446c7befaafbe5adda1eee2895f808468561f3fb8bf8f4905da6e53056c`。

转换发布前及生成最终身份后均通过全量校验：源文件 SHA256、映射互逆、簇范围覆盖、所有有效边/顺序、度数/邻居边界、PQ 行及入口对应。运行加载器检查结构和长度；重新验证全部 SHA256/源等价性使用 `tools/validate_index.py`。

## 软件验证

手工小图期望行为、交接/恢复、被候选集淘汰后仍保留 visited、触发节点与预测窗口边界、全预取屏障、实际复制数据、空预测、预算、未触发、旧消息、容量不足、部分失败后的 drain 与下一查询、全部最终候选重排等契约测试通过。普通软件、Host SDK 构建中的 CTest，以及 ASan/UBSan 检查均通过。

完整查询集结果：[30,000 条回归记录](../results/regression-20260927-024457/regression.json)。每种 ef 使用全部 10,000 条 SIFT 查询，warmup=2，topk=10，w=0.75，x=0.25，max_expansions=10000。

| ef | 查询数 | SoC 软件交接数 | Recall@10 | 失败 / 预算截断 |
| --- | ---: | ---: | ---: | ---: |
| 32 | 10,000 | 9,996 | 0.912240 | 0 / 0 |
| 64 | 10,000 | 10,000 | 0.971530 | 0 / 0 |
| 128 | 10,000 | 10,000 | 0.991900 | 0 / 0 |

全部成功查询与完整 Host 基线逐项比较 top-k、精确距离、终止原因；交接查询还比较扩展顺序/rank、最终 PQ 候选及总扩展数。没有发现不一致。ef32 的 4 条查询未触发，由 Host 正常完成。

完整回归后仅修正可选统计关闭行为，最终软件源码又完成 [每种 ef 100 条复核](../results/regression-20260927-025423/regression.json)。另执行独立 Host baseline 和 metrics=off 各 100 条，结果 ID/终止原因一致，off 输出没有可选统计字段。最后的 DOCA 导出记录修正只影响真实后端，Host/Arm 均已重新构建。

逐查询与汇总/Markdown 报告位于完整回归目录的 `ef32/ef64/ef128/`。软件验证启用 compare，因此内部使用 trace 进行比较；这些运行的延迟不能作为真机加速比。阶段时间可能重叠，传输量为应用请求字节。

## 真实 SDK 与构建验收

使用 NVIDIA 官方 DOCA 2.9.4 仓库中的 **2.9.4005-1** 包，校验 SHA256 后仅在项目内解压。API 依据同版本 SDK 头文件、DMA/Comch 示例与 dma_copy 应用；没有自建假头文件或用模拟库替代真实适配器。

| 程序 | ELF 架构 | 构建结果 |
| --- | --- | --- |
| `build/software/anns_baseline`、`anns_sim` | x86_64 | 通过，可在当前机器运行 |
| `build/host-sdk/anns_host` | x86_64 | 真实 Comch/DMA 编译链接通过 |
| `build/arm-sdk/anns_soc` | AArch64 | 真实 Comch/DMA 交叉编译链接通过 |

Host 与 SoC ELF 均依赖 `libdoca_common.so.2`、`libdoca_dma.so.2`、`libdoca_comch.so.2`。Host 动态依赖无缺失；实际 SDK 检查程序枚举到 **0 个设备**。请求 DOCA 构建但未提供 SDK 时按预期配置失败。

Arm 参考目标为 Ubuntu 22.04 用户态/GCC11 库，使用当前 clang 18 与项目内 AArch64 链接器，GLIBC 需求最高为 2.34。它是参考构建，未执行 Arm 二进制，也未核对未来 DPU BFB。

PCI 导出记录在后端传递注册源地址、区域长度与 SDK mmap 描述；SoC 只将该远端地址交给 SDK，不在本地解引用。算法和普通数据请求仅使用区域 ID/偏移。大传输拆分为有界任务，结果仍通过一条完成消息返回。

可追溯记录位于 [execution](../planning/execution/)，包括环境、SDK/Arm 包 URL 与校验和、构建/测试日志、最终源码和二进制 SHA256/ELF 依赖的 [构建清单](../planning/execution/build_manifest.json)。

## 硬件阶段待验收

CloudLab 实际内核、设备 BDF/representor、DPU BFB、驱动与 SDK/runtime 兼容性仍需在实机核对。当前机器是 WSL2 x86_64，不能替代目标设备。

设备到位后按计划验证：初始化和单块 DMA 内容、Comch 往返、完整状态恢复、预取/补读/重排、故障 drain 与断连/映射撤销、完整查询正确性及阶段性能；另核定 x86/Arm 浮点差异。

真实入口会拒绝不支持的路线/容量，单连接失败后不自动重放。Host 无法确认远端访问停止的异常路径会终止失败会话，避免正常查询内存回收；该行为仍须以真机故障测试检验，不能视为已证明的 DMA 安全结束。

RDMA、跨查询簇缓存、驱逐算法、多查询并发和注册成本优化仍按 v1 范围后置。本次没有 Git 提交或推送。
