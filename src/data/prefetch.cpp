#include "anns/data/prefetch.hpp"
namespace anns {
// 将冻结的预测簇转换为完整连续图范围，容量不足时明确失败。
PrefetchPlan build_prefetch_plan(const QueryState &state,
                                 const IndexMetadata &meta, uint64_t capacity) {
  PrefetchPlan plan;
  for (auto c : state.predicted_clusters) {
    require(c < meta.c, "predicted cluster out of range");
    uint64_t bytes = checked_mul(
        meta.cluster_offsets[c + 1] - meta.cluster_offsets[c], meta.stride);
    bounds(plan.total_bytes, bytes, capacity);
    plan.ranges.push_back({c, checked_mul(meta.cluster_offsets[c], meta.stride),
                           bytes, plan.total_bytes});
    plan.total_bytes += bytes;
  }
  return plan;
}
// 仅在全预取就绪后定位查询本地记录；未命中时返回 nullptr 供补读。
const uint32_t *PrefetchedGraph::find(uint32_t id,
                                      const IndexMetadata &meta) const {
  require(ready, "prefetch accessed before full readiness");
  require(id < meta.n, "graph ID out of range");
  auto c = meta.graph_to_cluster[id];
  for (const auto &range : plan.ranges)
    if (range.cluster == c) {
      uint64_t offset = range.local_offset +
                        checked_mul(id - meta.cluster_offsets[c], meta.stride);
      bounds(offset, meta.stride, uint64_t(records.size()) * 4);
      return records.data() + offset / 4;
    }
  return nullptr;
}
}  // namespace anns
