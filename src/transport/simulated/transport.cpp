#include "anns/transport/read_transport.hpp"
namespace anns {
// 校验请求和在途容量后排队；提交时不提前复制或消费目标数据。
Submission SimulatedReadTransport::submit(const ReadRequest &request) {
  if (pending_.size() >= config_.in_flight)
    return {SubmitStatus::WouldBlock, 0};
  try {
    const auto &region = registry_.get(request.region);
    bounds(request.offset, request.bytes, region.bytes);
    require(request.target && request.bytes > 0 && request.bytes <= max_bytes(),
            "invalid read task");
  } catch (const std::exception &) {
    return {SubmitStatus::Failed, 0};
  }
  uint64_t token = next_++;
  ++submitted_;
  pending_.push_back({request, token, tick_ + config_.delay_ticks + 1,
                      request.measure ? now_ns() : 0});
  return {SubmitStatus::Accepted, token};
}
// 推进模拟时钟，按配置注入逆序完成或失败；成功完成时实际复制字节。
std::vector<ReadCompletion> SimulatedReadTransport::progress() {
  ++tick_;
  std::vector<ReadCompletion> completed;
  if (config_.reverse_completion)
    std::reverse(pending_.begin(), pending_.end());
  for (auto p = pending_.begin(); p != pending_.end();) {
    if (p->ready > tick_) {
      ++p;
      continue;
    }
    bool success = p->token != config_.fail_task;
    if (success) {
      const auto &region = registry_.get(p->request.region);
      std::memcpy(p->request.target, region.data + p->request.offset,
                  p->request.bytes);
    }
    completed.push_back({p->token, success ? p->request.bytes : 0,
                         p->request.measure ? now_ns() - p->started : 0,
                         success});
    p = pending_.erase(p);
  }
  return completed;
}
}  // namespace anns
