// common/types.hpp：公共基础类型、边界检查和查询统计。
// Host 与 SoC 共用候选和结果表示；本地对象不直接作为跨端协议。

#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace anns {
using Bytes = std::vector<uint8_t>;
inline void require(bool ok, const std::string &message) {
  if (!ok) throw std::runtime_error(message);
}
// 使用本机单调时钟计时；不同端的时间戳不能直接相减。
inline uint64_t now_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}
// 在计算资产长度与字节偏移前检查无符号乘法溢出。
inline uint64_t checked_mul(uint64_t a, uint64_t b) {
  require(b == 0 || a <= UINT64_MAX / b, "integer multiplication overflow");
  return a * b;
}
// 使用减法检查区域范围，避免 offset + bytes 溢出。
inline void bounds(uint64_t offset, uint64_t bytes, uint64_t size) {
  require(offset <= size && bytes <= size - offset, "region bounds violation");
}
template <class T>
struct ArrayView {
  const T *data = nullptr;
  size_t size = 0;
  const T &operator[](size_t i) const {
    require(i < size, "view bounds violation");
    return data[i];
  }
};
struct Candidate {
  uint32_t id = 0;
  float distance = 0;
  bool expanded = false;
};
// 以图 ID 作为同距排序的确定性次序，保证两端候选顺序一致。
inline bool candidate_less(const Candidate &a, const Candidate &b) {
  return a.distance < b.distance || (a.distance == b.distance && a.id < b.id);
}
enum class Termination : uint32_t { Exhausted, Budget, Error };
enum class Purpose : uint32_t {
  Init,
  State,
  Prefetch,
  GraphMiss,
  Vectors,
  Count
};
struct WorkStats {
  bool enabled = true;
  uint64_t expansions = 0, distances = 0, visited_hits = 0, convergence = 0,
           prediction_hits = 0;
};
struct TransferStats {
  uint64_t logical_requests = 0, logical_bytes = 0, tasks = 0,
           submitted_bytes = 0, completed_bytes = 0, failures = 0, task_ns = 0;
};
struct DeviceStats {
  bool enabled = true;
  uint64_t d0 = 0, ds = 0, dp = 0, start = 0, end = 0, rerank = 0, done = 0;
  uint64_t graph_hits = 0, graph_misses = 0, first_wait_ns = 0, wait_ns = 0,
           rerank_vectors = 0;
  WorkStats work;
  TransferStats transfers[static_cast<size_t>(Purpose::Count)];
};
struct Result {
  uint64_t session = 0, query_id = 0;
  bool offloaded = false, safe = true;
  Termination termination = Termination::Exhausted;
  std::string error;
  std::vector<uint32_t> ids;
  std::vector<float> distances;
  std::vector<Candidate> pq_candidates;
  std::vector<uint32_t> trace, ranks;
  uint64_t h0 = 0, h1 = 0, h2 = 0, h3 = 0;
  WorkStats host;
  DeviceStats soc;
  uint64_t predicted_clusters = 0, predicted_nodes = 0;
};
}  // namespace anns
