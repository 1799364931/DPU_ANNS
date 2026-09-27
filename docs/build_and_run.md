# ANNS v1 构建与运行

实现对应 program_design_v1.md；C++17，已有 json-c，数据工具使用 conda sift-locality 的 NumPy。软件和真实 SDK 构建分开，DOCA 包仅解压到项目，不安装系统驱动。

## C++ 格式与注释

`.clang-format` 采用 Google 风格（默认 80 列、2 空格缩进）。每个 `.hpp` 开头说明文件职责；主要函数在定义处解释行为、边界或资源生命周期。格式化与检查仅针对项目源码，避免处理 SDK 和生成文件：

```bash
rg --files ANNS/include ANNS/src ANNS/apps ANNS/tests -g '*.hpp' -g '*.cpp' -0 | xargs -0 clang-format -i
rg --files ANNS/include ANNS/src ANNS/apps ANNS/tests -g '*.hpp' -g '*.cpp' -0 | xargs -0 clang-format --dry-run --Werror
```

## 软件构建

以下命令在 DPU 项目根目录执行：

```bash
cmake -S ANNS -B ANNS/build/software -DCMAKE_BUILD_TYPE=Release
cmake --build ANNS/build/software -j 4
ctest --test-dir ANNS/build/software --output-on-failure
```

可选内存与未定义行为检查：

```bash
cmake -S ANNS -B ANNS/build/sanitized -DCMAKE_BUILD_TYPE=Debug -DANNS_SANITIZERS=ON
cmake --build ANNS/build/sanitized -j 4
ctest --test-dir ANNS/build/sanitized --output-on-failure
```

## 数据准备

prepare_sift1m.json 引用已有 CSR/PQ 和 c4096 聚类，转换只写 ANNS/artifacts。已有输出目录拒绝覆盖；本次已生成产物，因此可直接验证和运行，无需再次生成。

```bash
/root/miniconda3/envs/sift-locality/bin/python ANNS/tools/prepare_index.py --config ANNS/configs/prepare_sift1m.json
/root/miniconda3/envs/sift-locality/bin/python ANNS/tools/validate_index.py ANNS/artifacts/sift1m-c4096-v1/manifest.json
```

首次命令仅用于输出尚不存在时。重新生成时选择新的 output 路径，并同步运行配置的 manifest。来源清单与 SHA256 写入 manifest；所有有效边和次序、双向映射、簇范围、入口及 PQ 行全量验证后才发布 manifest。

加载时检查文件长度、矩阵头、元数据、度数/邻居边界和非有限数据。需要重新检查全部 SHA256 与源图等价性时使用 validate_index.py；运行加载器不重新计算所有资产的 SHA256。

## 软件查询与回归

```bash
ANNS/build/software/anns_sim --config ANNS/configs/sift1m.json
ANNS/build/software/anns_baseline --config ANNS/configs/sift1m.json
python3 ANNS/tools/run_regression.py --config ANNS/configs/sift1m.json --binary ANNS/build/software/anns_sim --queries 10000
```

单次配置的 output 必须不存在。运行 baseline 和 sim 时分别使用配置副本并设置不同 output；不能把两次结果写到同一目录。回归工具自动创建独立运行目录，并顺序执行 ef=64/32/128。

compare=1 时，每条成功查询与完整 Host 基线比较结果、精确距离、扩展序列/rank、最终 PQ 候选和终止原因；为此内部开启有界完整 trace。正式软件延迟测量应将 compare 设为 0，并注明 metrics 模式。验证运行的延迟包含查询内部的 trace 记录开销，基线对照本身在查询计时结束后执行。

runtime.routes 的三项必须各自显式配置。软件应用支持 simulated；真实应用 v1 支持 dma，分别绑定用途，SoC 可共用同一 DMA 实例。没有全局 backend 自动替换三项。

主要容量为 resident_capacity（默认 512 MiB，常驻 PQ/元数据传输包）、prefetch_capacity（完整预测图）、state_capacity、max_message、chunk_bytes 与 in_flight。resident_capacity 不包含 SoC 初始化解码峰值的全部额外复制；最终设备内存预算仍需考虑缓冲、SDK 对象与对齐。容量不足明确报错，不裁剪预测簇。

输出为 queries.jsonl、summary.json 和 config.json，ID 使用数据集编号。trace 使用图 ID。报告整理：

```bash
python3 ANNS/tools/summarize_results.py ANNS/results/<run_id>/ef64
```

应用级字节量和模拟延迟不能作为 PCIe 物理流量或硬件性能依据。状态/预取可重叠，各端本地时间求差，不能相加或直接相减跨端时间戳。

## 真实 Host SDK 构建

SDK 参考版本为官方 2.9.4 仓库中的 2.9.4005-1，下载 URL 和校验和见 planning/execution/sdk_packages.json。已在当前项目本地解压；复现下载需先准备同版本 Packages 索引（本次已记录）。

```bash
python3 ANNS/tools/fetch_doca.py
cmake -S ANNS -B ANNS/build/host-sdk -DCMAKE_BUILD_TYPE=Release -DANNS_DOCA=ON -DDOCA_ROOT="$PWD/ANNS/artifacts/sdk/x86_64/root/opt/mellanox/doca"
cmake --build ANNS/build/host-sdk -j 4
```

显式 ANNS_DOCA=ON 时缺 SDK 会配置失败。anns_sdk_check 是真实 API 链接/设备枚举检查；anns_host、anns_soc 均实际链接 anns_doca_backend 中的 Comch/DMA 源码，不以模拟库替代适配器。报告库包含软件应用支持，但真实 Host/SoC 入口只装配 DOCA 实例。

## AArch64 参考构建

使用现有 clang 18 和项目本地解压的 Ubuntu 22.04 Arm 用户态、GCC11 标准库及 AArch64 链接器。包来源见 arm_sysroot_packages.json；不代表未来设备 BFB 已匹配。

```bash
python3 ANNS/tools/fetch_arm_sysroot.py
cmake -S ANNS -B ANNS/build/arm-sdk -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/arm22-clang.cmake -DANNS_DOCA=ON -DDOCA_ROOT="$PWD/ANNS/artifacts/sdk/arm64-dpu/root/opt/mellanox/doca"
cmake --build ANNS/build/arm-sdk -j 4
readelf -h ANNS/build/arm-sdk/anns_soc
readelf -d ANNS/build/arm-sdk/anns_soc
```

链接器包需解压到 artifacts/toolchain/linker；当前已准备。cmake/aarch64-ld.sh 设置该工具自身的本地运行库目录。纯交叉编译不运行 AArch64 程序，不冒充硬件模拟。

## 未来真机入口

两端均使用匹配的 SDK/运行库与设备驱动，Host 和 SoC 的容量配置要兼容；BDF 必须按实机选择。Comch 服务名为 anns-v1，Host 为 client，SoC 为 server，需要 Host representor。两端命令分别执行：

```bash
ANNS/build/arm-sdk/anns_soc --config ANNS/configs/doca.json --pci <SoC-BDF> --representor <Host-representor-BDF>
ANNS/build/host-sdk/anns_host --config ANNS/configs/doca.json --pci <Host-BDF>
```

SoC 不加载本地全库图/向量/manifest，配置中的 manifest/output 仅被解析；PQ/映射在初始化时通过 DMA 从 Host 装载。当前 SoC 是单连接服务，Host 断连后结束会话，需要显式重新启动，不自动重连或重放。

Host 通过有界 INIT_PART 交换 PCI 映射导出记录，再发送 INIT/SUBMIT。记录包含已注册源区的 SDK 地址、长度及不透明 mmap 描述，遵循同版本官方 dma_copy 应用；后端只将远端地址交给 SDK 建立源 doca_buf，绝不在 SoC 本地解引用。搜索层仍只使用区域 ID 与偏移。Host 图/向量长期映射，交接区逐查询映射；首版 DMA 每任务注册目标区，这是正确性实现，尚未做注册成本优化。

错误任务先完成 drain 再返回 QUERY_ERROR。Host 若遇超时/断连等无法确认远端访问停止的会话异常，真实入口禁止继续复用内存，以不展开 C++ 查询资源析构的进程终止结束失败会话；这不被报告为成功/安全查询完成，映射撤销和设备错误行为必须通过真机故障测试核验。

当前不宣称真机 DMA 内容、完成顺序、性能或 x86/Arm 浮点逐位一致；这些是设备到位后的验收项。

## 官方版本依据

- [NVIDIA DOCA 2.9.4 安装说明](https://networking-docs.nvidia.com/doca/archive/2-9-4/doca-installation-guide-for-linux)
- [NVIDIA DOCA 2.9.4 支持范围](https://networking-docs.nvidia.com/doca/archive/2-9-4/general-support)
- 实际 API 依据：本地同版本 SDK 的 include/doca_comch.h、doca_dma.h、doca_mmap.h 及 samples/doca_dma、samples/doca_comch。
