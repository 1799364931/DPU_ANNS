// runtime/soc_worker.hpp：SoC 单 worker 的初始化、读取屏障和续搜调度。
// 常驻 PQ 与映射；图记录、交接状态和重排向量按查询读取。

#pragma once
#include "anns/runtime/session.hpp"
#include "anns/search/search_core.hpp"
namespace anns {
class SocWorker {
  ControlChannel &control_;
  ReadRoutes routes_;
  RuntimeConfig config_;
  SocIndex index_;
  uint64_t session_ = 0, last_query_ = 0;
  bool ready_ = false;
  void initialize(const Message &message);
  void execute(const Message &message);

 public:
  Result diagnostics;
  DeviceStats init_stats;
  SocWorker(ControlChannel &channel, ReadRoutes routes, RuntimeConfig config)
      : control_(channel), routes_(routes), config_(std::move(config)) {}
  void tick();
  bool ready() const { return ready_; }
};
}  // namespace anns
