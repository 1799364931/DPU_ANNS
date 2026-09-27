#include "anns/transport/read_transport.hpp"

#include "anns/transport/control_channel.hpp"
namespace anns {
// 按读取用途选择独立配置的路线，禁止使用未绑定后端。
ReadTransport &ReadRoutes::select(Purpose purpose) const {
  ReadTransport *p = purpose == Purpose::GraphMiss ? graph_miss
                     : purpose == Purpose::Vectors ? rerank_vectors
                                                   : handoff_prefetch;
  require(p, "unbound read route");
  return *p;
}
// 拆分逻辑读取并在背压时推进任务；只对完整成功的请求调用回调。
// 失败或回调异常时停止新提交，并 drain 全部已接受任务后再抛出。
void read_batch(ReadTransport &transport,
                const std::vector<ReadRequest> &requests, DeviceStats &stats,
                std::function<void(size_t)> complete_group) {
  struct Chunk {
    ReadRequest request;
    size_t group;
  };
  std::vector<Chunk> chunks;
  std::vector<size_t> remaining(requests.size(), 0);
  require(transport.pending() == 0 && transport.max_bytes() > 0,
          "read batch ownership violation");
  for (size_t group = 0; group < requests.size(); ++group) {
    auto request = requests[group];
    request.measure = stats.enabled;
    auto &s = stats.transfers[static_cast<size_t>(request.purpose)];
    if (stats.enabled) {
      ++s.logical_requests;
      s.logical_bytes += request.bytes;
    }
    for (uint64_t offset = 0; offset < request.bytes;) {
      uint64_t size = std::min(transport.max_bytes(), request.bytes - offset);
      chunks.push_back(
          {{request.region, request.offset + offset, size,
            request.target + offset, request.purpose, request.measure},
           group});
      ++remaining[group];
      offset += size;
    }
  }
  for (size_t group = 0; group < remaining.size(); ++group)
    if (remaining[group] == 0 && complete_group) complete_group(group);
  std::map<uint64_t, Chunk> active;
  size_t next = 0;
  bool failed = false;
  std::exception_ptr callback_error;
  while (next < chunks.size() || !active.empty()) {
    while (!failed && next < chunks.size()) {
      auto submission = transport.submit(chunks[next].request);
      if (submission.status == SubmitStatus::WouldBlock) break;
      if (submission.status == SubmitStatus::Failed) {
        failed = true;
        if (stats.enabled)
          ++stats.transfers[static_cast<size_t>(chunks[next].request.purpose)]
                .failures;
        break;
      }
      require(active.emplace(submission.token, chunks[next]).second,
              "duplicate transfer token");
      auto &s =
          stats.transfers[static_cast<size_t>(chunks[next].request.purpose)];
      if (stats.enabled) {
        ++s.tasks;
        s.submitted_bytes += chunks[next].request.bytes;
      }
      ++next;
    }
    if (failed) next = chunks.size();
    for (const auto &completion : transport.progress()) {
      auto p = active.find(completion.token);
      require(p != active.end(), "unexpected completion token");
      auto chunk = p->second;
      active.erase(p);
      auto &s = stats.transfers[static_cast<size_t>(chunk.request.purpose)];
      if (stats.enabled) s.task_ns += completion.elapsed_ns;
      if (!completion.success || completion.bytes != chunk.request.bytes) {
        failed = true;
        if (stats.enabled) ++s.failures;
      } else {
        if (stats.enabled) s.completed_bytes += completion.bytes;
        if (--remaining[chunk.group] == 0 && !failed && complete_group) {
          try {
            complete_group(chunk.group);
          } catch (...) {
            callback_error = std::current_exception();
            failed = true;
          }
        }
      }
    }
  }
  require(transport.pending() == 0, "transport did not drain");
  if (callback_error) std::rethrow_exception(callback_error);
  require(!failed, "read batch failed after safe drain");
}
// 以同一批读取契约执行单个请求，包括分块、统计和失败 drain。
void read_one(ReadTransport &transport, const ReadRequest &request,
              DeviceStats &stats) {
  read_batch(transport, {request}, stats);
}
// 编码消息并在发送背压时推进控制通道，超时视为会话异常。
void send_message(ControlChannel &channel, const Message &message) {
  auto bytes = encode_message(message);
  uint64_t start = now_ns();
  while (!channel.send(bytes)) {
    channel.progress();
    require(now_ns() - start < 30000000000ULL, "message send timed out");
  }
}
}  // namespace anns
