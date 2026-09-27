# BlueField-2 ANNS 初始实现规划

## 目标与授权范围
逐步与用户制定可修改、可编译的 BlueField-2 图搜索卸载系统方案。当前没有设备，仅讨论并记录方案；用户再次明确不要写代码。

## 已确认约束
- v1 目标：单查询正确交接并完成搜索。
- 当前目标设备 BlueField-2；原背景文档使用 BlueField-3。
- ANNS 是实现目录；predict 是搜索语义参考；correct 已归档，仅复用既有资产。
- 逐项讨论，不一次冻结全部设计，不安装依赖、不提交或推送 Git。

## 阶段
- [x] 阅读指定背景文档与 search_predict.cpp。
- [x] 核对官方能力边界，提出最小闭环。
- [x] 用户确认 v1 单查询正确交接目标。
- [x] 用户确认触发节点扩展前暂停的交接边界。
- [x] 用户明确策略可替换、底层存储简单、允许共享连续内存。
- [x] 用户明确控制面/数据面分离、SoC 单边读取及反向簇状态接口。
- [x] 已在正式实施计划中明确首版状态默认值、数据访问契约和查询生命周期。
- [x] 用户确认 Host 管理簇行为、SoC 执行和反馈（v1 只预留接口）。
- [x] 已将 benchmark 基础指标和全预取就绪后的计时边界纳入正式计划。
- [x] 用户确定实际预测簇预取、定长邻接表、SoC 常驻 PQ 编码、逐查询交接 LUT。
- [x] 用户确定 v1 所有数据读取先用 DMA，三类用途保留独立策略选择。
- [x] 用户选择消息通道、按簇连续图布局及 ID 映射、完整预取就绪后续搜、结果随完成消息。
- [x] 用户确定搜索统一用图 ID，读取原始数据集和返回结果时才映射。
- [x] 已整理最小协议语义、字段组、容量/错误规则和实现顺序。
- [x] 用户提供 CloudLab Host 镜像 UBUNTU24-64-STD，Host 按 Ubuntu 24.04 规划。
- [ ] 选定 SDK、DPU 操作系统、工具链和真实 SDK 编译验收；核对 CloudLab 实际内核。
- [x] 已形成 ANNS/implementation_plan_v1.md，包含 P0–P5 阶段、验证矩阵与交付清单。

## 记录文件
- ../implementation_plan_v1.md：正式实施入口，整合已确认要求及标注的工程默认值。
- protocol_v1.md：最新最小协议、聚类图布局及 ID 语义建议。
- v1_decisions.md：已确认决策汇总，优先于旧讨论稿。
- build_protocol_v1.md：编译候选、当前环境、最小协议与验收详细建议。
- initial_scope.md：第一轮范围建议及用户确认。
- handoff_v1.md：已确认的暂停边界及恢复语义建议。
- data_access_v1.md：第三轮逻辑抽象、简单存储与数据访问建议。
- control_data_v1.md：控制/数据面方向、Host 管理归属及任务/簇事件建议。
- benchmark_v1.md：第五轮 benchmark 插桩建议。
- notes.md：官方资料和研究记录。

## 环境记录
- 普通沙箱的 bubblewrap 因 /mnt/wslg/distro 挂载异常无法启动；使用 require_escalated 访问。
- 当前目录 git status 返回 not a git repository。

## 当前状态
2026-09-27：本次规划工作完成，正式计划见 ANNS/implementation_plan_v1.md。架构选择已足够开始后续实施；尚未执行 P0–P5。SDK、DPU 目标系统与 CloudLab 实际内核是 P0 环境核对事项，不能标成已经验证。未编写系统代码、未安装依赖、未运行编译、数据转换或实验。

补充环境记录：只读检查为 x86_64 Ubuntu 24.04.1；发现 GCC/G++/CMake，pkg-config 未列出 DOCA。部分官方 2.9.x 文档网页重定向失败，版本相关 API 以未来取得的同版本 SDK 头文件和示例核实。

## 后续实施更新
2026-09-27：用户“执行计划”授权后，无设备阶段 P0–P5 完成，见 [实施进度](execution/task_plan.md) 与 [实施报告](../docs/implementation_report_v1.md)。本文件上述记录是早期规划历史；真机 BFB、CloudLab 内核和 DMA/性能仍待验证。
