// common/report.hpp：查询结果输出与软件应用运行入口。
// 提供 JSONL 统计、完整 Host 基线及模拟交接的对照运行。

#pragma once
#include <ostream>

#include "anns/runtime/host_coordinator.hpp"
namespace anns {
void write_result(std::ostream &out, const HostIndex &index, uint32_t query,
                  const Result &result, uint32_t topk,
                  const RuntimeConfig &runtime);
int run_application(int argc, char **argv, bool simulated);
}  // namespace anns
