// search/query_state.hpp：可交接的查询状态、候选集和精确 visited。
// visited 保留全部已发现节点，包括被候选集裁剪的节点。

#pragma once
#include "anns/index/index.hpp"
namespace anns {
class ExactVisited {
 public:
  Bytes bytes;
  // 按图 ID 初始化独立的精确 visited，不沿用上一条查询的内容。
  void begin_query(uint32_t n) { bytes.assign(n, 0); }
  // 返回节点此前是否已见，并保留发现标记；候选淘汰不撤销该标记。
  bool test_and_mark(uint32_t id) {
    require(id < bytes.size(), "visited ID overflow");
    bool old = bytes[id] != 0;
    bytes[id] = 1;
    return old;
  }
};
class CandidateSet {
 public:
  std::vector<Candidate> values;
  size_t nearest_unexpanded() const;
  void sort_and_trim(uint32_t ef);
};
struct QueryState {
  SearchConfig config;
  std::vector<float> query, lut;
  CandidateSet candidates;
  ExactVisited visited;
  uint32_t expansions = 0, pending_id = 0, pending_index = 0;
  bool triggered = false;
  std::vector<uint32_t> predicted_clusters;
  uint64_t predicted_nodes = 0;
};
struct QueryWorkspace {
  std::vector<uint32_t> fresh;
};
}  // namespace anns
