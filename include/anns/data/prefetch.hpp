// data/prefetch.hpp：预测簇的连续读取计划及查询级预取图视图。
// 全部预测簇读取完成后才允许访问，本次查询结束后内容失效。

#pragma once
#include "anns/search/query_state.hpp"
namespace anns {
struct ClusterRange {
  uint32_t cluster = 0;
  uint64_t offset = 0, bytes = 0, local_offset = 0;
};
struct PrefetchPlan {
  std::vector<ClusterRange> ranges;
  uint64_t total_bytes = 0;
};
PrefetchPlan build_prefetch_plan(const QueryState &state,
                                 const IndexMetadata &meta, uint64_t capacity);
struct PrefetchedGraph {
  PrefetchPlan plan;
  std::vector<uint32_t> records;
  bool ready = false;
  const uint32_t *find(uint32_t id, const IndexMetadata &meta) const;
};
}  // namespace anns
