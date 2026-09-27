// transport/read_transport.hpp：内存区域注册、异步读取契约和按用途选择的读取路线。
// 目标缓冲在已接受任务全部完成前必须有效，失败批次也先 drain。

#pragma once
#include <deque>
#include <functional>
#include <map>
#include <memory>

#include "anns/common/config.hpp"
namespace anns {
struct Region {
  const uint8_t *data = nullptr;
  uint64_t bytes = 0;
};
class MemoryRegistry {
  std::map<uint32_t, Region> regions_;

 public:
  std::function<void(uint32_t, const Region &)> on_bind;
  std::function<void(uint32_t)> on_unbind;
  // 登记 Host 区域并通知真实后端；调用方保持存储有效直至安全撤销。
  void bind(uint32_t id, const void *data, uint64_t bytes) {
    require(id > 0 && data, "invalid memory region");
    regions_[id] = {static_cast<const uint8_t *>(data), bytes};
    if (on_bind) on_bind(id, regions_[id]);
  }
  // 通知后端撤销映射后移除逻辑区域，不隐式等待在途读取。
  void unbind(uint32_t id) {
    if (on_unbind) on_unbind(id);
    regions_.erase(id);
  }
  // 解析软件后端的逻辑区域，未知区域直接失败。
  const Region &get(uint32_t id) const {
    auto p = regions_.find(id);
    require(p != regions_.end(), "unknown memory region");
    return p->second;
  }
};
struct ReadRequest {
  uint32_t region = 0;
  uint64_t offset = 0, bytes = 0;
  uint8_t *target = nullptr;
  Purpose purpose = Purpose::State;
  bool measure = true;
};
struct ReadCompletion {
  uint64_t token = 0, bytes = 0, elapsed_ns = 0;
  bool success = false;
};
enum class SubmitStatus { Accepted, WouldBlock, Failed };
struct Submission {
  SubmitStatus status;
  uint64_t token = 0;
};
class ReadTransport {
 public:
  virtual ~ReadTransport() = default;
  virtual Submission submit(const ReadRequest &request) = 0;
  virtual std::vector<ReadCompletion> progress() = 0;
  virtual uint64_t max_bytes() const = 0;
  virtual size_t pending() const = 0;
};
class SimulatedReadTransport final : public ReadTransport {
  struct Pending {
    ReadRequest request;
    uint64_t token = 0, ready = 0, started = 0;
  };
  const MemoryRegistry &registry_;
  RuntimeConfig config_;
  uint64_t tick_ = 0, next_ = 1, submitted_ = 0;
  std::deque<Pending> pending_;

 public:
  SimulatedReadTransport(const MemoryRegistry &registry,
                         const RuntimeConfig &config)
      : registry_(registry), config_(config) {}
  Submission submit(const ReadRequest &) override;
  std::vector<ReadCompletion> progress() override;
  uint64_t max_bytes() const override { return config_.chunk_bytes; }
  size_t pending() const override { return pending_.size(); }
};
struct ReadRoutes {
  ReadTransport *handoff_prefetch = nullptr;
  ReadTransport *graph_miss = nullptr;
  ReadTransport *rerank_vectors = nullptr;
  ReadTransport &select(Purpose purpose) const;
};
// All destinations remain alive until every accepted task has completed, even
// on failure.
void read_batch(ReadTransport &transport,
                const std::vector<ReadRequest> &requests, DeviceStats &stats,
                std::function<void(size_t)> complete_group = {});
void read_one(ReadTransport &transport, const ReadRequest &request,
              DeviceStats &stats);
}  // namespace anns
