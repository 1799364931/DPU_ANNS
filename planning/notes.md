# BlueField-2 初始规划研究记录

## 已阅读项目背景
- ANNS/readme.md：系统动机和贡献。
- ANNS/prompt.md：Host/DPU 交接、DMA/RDMA、簇工作集、收敛判定。
- predict/read.md：预测冻结语义、实验输入与参数。
- predict/src/search_predict.cpp：636 行已完整阅读；只作搜索语义参考。

## 官方资料（2026-09-26 查询）
1. DOCA DMA 3.3 文档：https://docs.nvidia.com/doca/sdk/doca-dma/
   - Host/DPU 远程 DMA 要求 DPU mode。
   - 基于异步任务和 Progress Engine；需要设备、内存映射和能力检查。
   - DMA 的 PCIe 导出内存和 RDMA 导出内存不能不加区分地使用；该版本 DMA 不支持 RDMA export buffer 作为源或目的。
   - 此为能力参考，不代表项目已选定 3.3 SDK。
2. DOCA RDMA 2.9.0：https://networking-docs.nvidia.com/doca/archive/2-9-0/doca-rdma
   - 明确支持 BlueField-2 及更高设备。
   - Arm 侧此版本要求使用 SF；支持 RDMA Read 等操作，连接基于 IB 或 RoCE。
   - 不能从“库支持 BF2”推导未来设备的 Host/Arm RDMA 路径已经配置可用。
3. DOCA Developer Guide 2.5.1：https://docs.nvidia.com/doca/archive/doca-v2-5-1/NVIDIA%2BDOCA%2BDeveloper%2BGuide/index.html
   - 官方描述无 DPU 开发、主机交叉编译和 QEMU 开发容器路径。
   - 这里只确认开发方式可行；最终安装命令、镜像和工具链必须按选定 SDK 重新核对。
4. DOCA 2.9.4 General Support：https://networking-docs.nvidia.com/doca/archive/2-9-4/general-support
   - 列出 Host 与 Arm SDK 包，BFB 默认 Ubuntu 22.04。
5. DOCA 2.9.4 Release Notes：https://networking-docs.nvidia.com/doca/archive/2-9-4/doca-release-notes
   - 2.9.4 是 2.9.0 的 LTS 更新；尚未选定为项目基线。

## 设计推论（建议，未经用户确认）
- 搜索状态和搜索核心不暴露 DOCA 对象；设备对象及任务生命周期收敛在后端。
- 第一条验收主线：固定输入下，对比不中断搜索与暂停/序列化/恢复后的搜索。
- 同平台的确定性测试可以严格比对路径与结果；跨 x86/Arm 的浮点差异需要独立定义容差与近等距排序规则。
- 软件后端须模拟数据请求和完成，避免搜索核心偷读 Host 地址；其测试不能证明硬件同步正确性或性能。
- 先单查询和单工作线程完成闭环，再讨论共享簇缓存与并行搬运。
- 环状队列是待评估的请求机制，不能默认等同于一个已验证的 DOCA API。
- DMA 先完成闭环、RDMA 再接入是分阶段建议，不是删除用户的最终双通道目标。

## 未确定
SDK/BFB/系统版本、PQ 编码布局、访问标记交接形式、控制通道、请求队列、DMA/RDMA 接入顺序、缓存容量和驱逐策略均未冻结。

## 第二轮更新
- 用户确认 v1 单查询正确交接，重申只计划、不写代码。
- 重新阅读根目录 AGENTS.md；明确 CSR 与 PQ32 现有资产位置，未发现额外 agent.md。
- 基于已读搜索实现，形成 handoff_v1.md；重点是触发节点扩展前交接，以及 visited 必须保留所有已发现节点。
- 数据格式说明不等同于已核验二进制；本轮未读取数据文件。

## 第三轮更新
- 用户确认暂停边界，要求 visited 等策略可替换，允许数组及候选集/visited 的连续共享存储。
- 修正先前过度限制：不再排除直接搬运 epoch 表；只要求相关元数据完整且双方解释一致。
- 数据访问接口仅按逻辑需求规划，尚未选定具体类、签名、内存布局和 DOCA API。

## 第四轮更新
- 用户提出控制面与数据面分离，环队列提交/收割作为可能方案；数据读取应由 SoC 单边发起。
- 预留 SoC → Host 簇状态接口；尚未确认由哪一端决定驱逐。
- 建议区分引用释放、可驱逐与驱逐完成；本轮仅定义语义，未核定 SDK API 或跨端环实现。

## 第五轮更新
- 用户确认 Host 管理簇、SoC 执行，提出 benchmark 插桩需求。
- benchmark_v1.md 提出正确性/工作量、分端阶段计时、数据访问计数及未来缓存事件指标。
- 时间戳只在各自时钟域内求差；软件模拟与真机性能结果分开；详细 trace 不作为正式延迟测量默认模式。
- 本轮仍仅修改规划文档。

## 第六轮更新
用户明确实际预取、v1 各类 DMA 读取但策略独立、定长图、常驻 PQ 编码和按查询交接 LUT；已集中记录 v1_decisions.md。编译候选为 2.9.4 LTS 更新/Ubuntu 22.04，尚未获用户确认，也未安装或构建。官方 general-support/release-notes 与 2.9.0 DMA Copy 应用已核对，资料和边界详见 build_protocol_v1.md。当前发现预测窗口不保证含触发节点，已记录首轮缺失必须补读以及预取就绪策略待讨论。

## CloudLab 镜像修正
用户提供 UBUNTU24-64-STD。修正上一轮将 Host 和 DPU 同时建议为 Ubuntu 22.04 的不准确表述：Host 按 Ubuntu 24.04，DPU 按实际 BFB 单独确定。官方 DOCA 2.9.4 general-support 的 Host 表包含 Ubuntu 24.04 x86 与 6.8.0-31-generic；SDK 2.9.4 仍可保留为候选。未获取 CloudLab 机器实际内核、驱动或 DPU 系统信息，也未安装依赖。

## 最小协议四项决策
用户确定：消息通道；图按簇离线连续存储，用 CSR 风格簇偏移和 dataset_to_graph 映射；全部状态及预测簇就绪后启动；结果随完成消息。Host 打包建议被用户的离线重排方案替代。protocol_v1.md 建议仅重排记录位置，保持邻接边/候选/visited/PQ/向量使用原数据集 ID，ID 语义尚未得到明确确认。

## 图 ID 决策更新
用户否定仅重排存储位置的建议，确定搜索内部统一用重排图 ID。已更新 protocol_v1.md 等活动规划：双向映射、邻接边转换、Host/SoC PQ 行重排、簇归属元数据同步、原始向量及结果边界转换。旧记录保留为讨论历史，以 v1_decisions.md 和 protocol_v1.md 为准。交接验收改用重排图 Host 基线，避免原始 ID 同距排序干扰。仍未编写代码或转换数据。

## 2026-09-27 正式计划完成
用户要求在无重大遗漏时开始制定正式计划。本轮整合为 ANNS/implementation_plan_v1.md，未获实施授权，也未编写系统代码。补齐容量拒绝、消息载荷、在途访问清理、三类独立策略、图 ID 基线、初始化 PQ、无交接路径及阶段验收。修正 benchmark 原 D1 起算续搜的旧口径，现从状态和全部预取就绪后的 Dstart 起算。工程默认值与用户决策分开；实际 SDK/DPU 系统核对归 P0。
