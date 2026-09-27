# 程序框架设计阅读记录

日期：2026-09-27。原有 13 份 Markdown 均完整阅读；本目录文件为本轮新产出。

## 逐文档记录

| 原始文档（相对 ANNS） | 设计输入 |
| --- | --- |
| readme.md | 系统动机；背景 BF3 与当前 v1 BF2 区分 |
| prompt.md | 原始搜索与卸载设想；环队列、RDMA 为早期描述，以后续决策为准 |
| implementation_plan_v1.md | 正式范围、工程默认值、数据契约、P0–P5 与验收；设计主入口 |
| planning/v1_decisions.md | 已确认要求；冲突时优先采用 |
| planning/task_plan.md | 上轮进度；规划完成、实现未开始，保留原样 |
| planning/initial_scope.md | 无设备验收分软件、真实 SDK 编译、未来真机三层 |
| planning/handoff_v1.md | 触发节点扩展前快照；visited 包括已淘汰候选 |
| planning/data_access_v1.md | 策略边界不要求复杂继承；数据返回有效期与失败契约 |
| planning/control_data_v1.md | Host 管簇，SoC 执行；完成/安全失败后回收，超时不代表停止访问 |
| planning/protocol_v1.md | 图 ID、簇连续区、全就绪屏障、结果随消息 |
| planning/build_protocol_v1.md | Host/Arm 构建隔离；SDK 与 BFB 尚待核对，软件不能代替真实 SDK |
| planning/benchmark_v1.md | 两端本地时钟；状态与预取并行不能累加；初始化与查询计时分开 |
| planning/notes.md | 历史推导，部分旧未定项已被后续决策替代，不能直接当当前规格 |

## 额外只读核对
- 根目录 AGENTS.md：不改归档资产、不提交；实验环境 sift-locality；长任务最多前三次估时查询。
- predict/src/search_predict.cpp：核对 Candidate、visited epoch、PQ LUT、runQuery、参数转换和重排；作为语义参考，不修改、不照搬 OpenMP/单体程序结构。
- 搜索 rank = selected + 1；预测使用 candidates[rank:min(rank+xCount,size)]，故触发节点本身不在窗口。
- PQ 重排覆盖最终保留的全部候选，再选 top-k；不能只读取 PQ top-k 做 FP32 重排。
- 原 predict 用 epoch visited，正式计划默认 uint8 精确 visited：表示可变化，所有已发现节点的去重语义必须一致。
- 现有 ANNS 只有 Markdown，无源码；不存在需保留的现有 ANNS 类接口。

## 设计选择（待用户审阅）
- C++17/CMake，普通 struct 表达数据；只有控制通道与异步传输边界使用有限运行时接口。
- 一个共用 SearchCore；Host 与 SoC 驱动同一扩展逻辑，核心不等待或持有设备句柄。
- 搜索核心与 FP32 Reranker 分开，后者重排全部最终候选。
- QueryState / HandoffBuffer / PrefetchedGraph 三种所有权分开；状态可共用连续存储，不强制每块独立分配。
- 固定三类 ReadRoutes，不设全局 backend；状态与预取分开统计及就绪。
- v1 Host 冻结具体簇读取计划，SoC 检查偏移/容量、执行搬运，避免将簇策略转移到 worker。
- 每查询目录与缓冲不做跨查询缓存；只预留 Host 簇命令与 SoC 状态事件入口。
- 增加 bounded vector batch 与可确认的 drain 边界，确保单查询失败不会提前释放 DMA 内存。

## 状态与限制
- 未安装依赖、未核验 SDK API/版本、未核验二进制资产、未转换数据、未编译或运行实验。
- 框架设计建立在已读项目文档之上，不新增 SDK 支持结论。
- 默认沙箱挂载异常，已使用 require_escalated 完成只读和 Markdown 写入；git status 显示当前非 Git 仓库。
