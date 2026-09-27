// runtime/host_coordinator.hpp：Host 完整搜索基线及单在途查询协调器。
// 负责逼近搜索、冻结交接区和收割结果，完成确认前保持导出数据有效。

#pragma once
#include "anns/runtime/session.hpp"
#include "anns/search/search_core.hpp"
namespace anns {
Result run_baseline(const HostIndex &index, ArrayView<float> query,
                    const SearchConfig &search, const RuntimeConfig &runtime);
class HostCoordinator {
  const HostIndex &index_;
  ControlChannel &control_;
  MemoryRegistry &registry_;
  RuntimeConfig config_;
  Bytes assets_, handoff_;
  uint64_t session_, next_query_ = 1;
  bool ready_ = false;
  std::function<void()> pump_;
  Message wait_for(uint64_t query);

 public:
  HostCoordinator(const HostIndex &index, ControlChannel &channel,
                  MemoryRegistry &registry, RuntimeConfig config,
                  std::function<void()> pump, uint64_t session = 1)
      : index_(index),
        control_(channel),
        registry_(registry),
        config_(std::move(config)),
        session_(session),
        pump_(std::move(pump)) {}
  ~HostCoordinator();
  void initialize();
  Result run(ArrayView<float> query, const SearchConfig &search);
};
}  // namespace anns
