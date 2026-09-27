#include "anns/search/search_core.hpp"
namespace anns {
// 查找距离最近的未扩展候选；全部已扩展时返回数组长度。
size_t CandidateSet::nearest_unexpanded() const {
  for (size_t i = 0; i < values.size(); ++i)
    if (!values[i].expanded) return i;
  return values.size();
}
// 按距离、图 ID 排序后保留前 ef 个候选，不撤销 visited 标记。
void CandidateSet::sort_and_trim(uint32_t ef) {
  std::sort(values.begin(), values.end(), candidate_less);
  if (values.size() > ef) values.resize(ef);
}
// 为当前查询计算 PQ32 的平方距离表，随后原样交接给 SoC。
std::vector<float> PQDistance::build_lut(const HostIndex &index,
                                         ArrayView<float> query) {
  require(query.size == index.meta.d, "query dimension mismatch");
  uint32_t sub = index.meta.d / 32;
  std::vector<float> lut(32 * 256);
  for (uint32_t part = 0; part < 32; ++part)
    for (uint32_t code = 0; code < 256; ++code) {
      float sum = 0;
      auto *center =
          index.centroids.data() + (uint64_t(part) * 256 + code) * sub;
      for (uint32_t j = 0; j < sub; ++j) {
        float delta = query[part * sub + j] - center[j];
        sum += delta * delta;
      }
      require(std::isfinite(sum), "LUT overflow");
      lut[part * 256 + code] = sum;
    }
  return lut;
}
// 按图 ID 读取 32 字节 PQ 编码并累加查询 LUT，拒绝非法或溢出结果。
float PQDistance::score(uint32_t id, const Bytes &codes,
                        const std::vector<float> &lut) {
  bounds(uint64_t(id) * 32, 32, codes.size());
  require(lut.size() == 8192, "bad LUT size");
  float result = 0;
  for (uint32_t part = 0; part < 32; ++part)
    result += lut[part * 256 + codes[uint64_t(id) * 32 + part]];
  require(std::isfinite(result), "PQ distance overflow");
  return result;
}
// 初始化查询状态、入口候选和 visited；本函数不执行节点扩展。
QueryState SearchCore::begin(const HostIndex &index, ArrayView<float> query,
                             const SearchConfig &config) {
  config.validate();
  QueryState s;
  s.config = config;
  s.query.assign(query.data, query.data + query.size);
  s.lut = PQDistance::build_lut(index, query);
  s.visited.begin_query(index.meta.n);
  s.candidates.values.reserve(uint64_t(config.ef) + index.meta.rmax);
  s.candidates.values.push_back(
      {index.meta.entry,
       PQDistance::score(index.meta.entry, index.pq_codes, s.lut), false});
  s.visited.test_and_mark(index.meta.entry);
  return s;
}
// 在候选可用且全局预算未耗尽时选定下一个节点，保留未扩展状态。
bool SearchCore::prepare_next(QueryState &s) {
  auto i = s.candidates.nearest_unexpanded();
  if (i == s.candidates.values.size() ||
      s.expansions >= s.config.max_expansions)
    return false;
  s.pending_index = static_cast<uint32_t>(i);
  s.pending_id = s.candidates.values[i].id;
  return true;
}
// 首次达到触发 rank 时冻结预测簇；窗口从触发节点之后开始。
// 触发本身不扩展节点，使 Host 可在这一边界交接。
bool SearchCore::maybe_trigger(QueryState &s, const IndexMetadata &m) {
  const uint32_t rank = s.pending_index + 1;
  if (s.triggered || rank < s.config.w_rank()) return false;
  s.triggered = true;
  if (s.config.prediction)
    for (size_t i = rank; i < std::min(uint64_t(rank) + s.config.x_count(),
                                       uint64_t(s.candidates.values.size()));
         ++i) {
      uint32_t cluster = m.graph_to_cluster[s.candidates.values[i].id];
      if (std::find(s.predicted_clusters.begin(), s.predicted_clusters.end(),
                    cluster) == s.predicted_clusters.end()) {
        s.predicted_clusters.push_back(cluster);
        s.predicted_nodes +=
            m.cluster_offsets[cluster + 1] - m.cluster_offsets[cluster];
      }
    }
  return true;
}
// 扩展已选节点，先标记全部新邻居再评分和裁剪候选。
// 扩展数跨交接连续累计，触发当次扩展计入收敛阶段。
void SearchCore::expand(QueryState &s, ArrayView<uint32_t> neighbors,
                        const Bytes &codes, const IndexMetadata &m,
                        QueryWorkspace &ws, WorkStats &stats) {
  require(neighbors.size <= m.rmax &&
              s.pending_index == s.candidates.nearest_unexpanded() &&
              s.pending_index < s.candidates.values.size() &&
              s.candidates.values[s.pending_index].id == s.pending_id &&
              s.expansions < s.config.max_expansions,
          "invalid expansion boundary");
  s.candidates.values[s.pending_index].expanded = true;
  ++s.expansions;
  if (stats.enabled) ++stats.expansions;
  if (s.triggered && stats.enabled) {
    ++stats.convergence;
    auto cluster = m.graph_to_cluster[s.pending_id];
    if (std::find(s.predicted_clusters.begin(), s.predicted_clusters.end(),
                  cluster) != s.predicted_clusters.end())
      ++stats.prediction_hits;
  }
  ws.fresh.clear();
  ws.fresh.reserve(m.rmax);
  for (size_t i = 0; i < neighbors.size; ++i) {
    auto id = neighbors[i];
    require(id < m.n, "neighbor ID overflow");
    if (s.visited.test_and_mark(id)) {
      if (stats.enabled) ++stats.visited_hits;
    } else {
      ws.fresh.push_back(id);
    }
  }
  for (auto id : ws.fresh) {
    s.candidates.values.push_back(
        {id, PQDistance::score(id, codes, s.lut), false});
    if (stats.enabled) ++stats.distances;
  }
  s.candidates.sort_and_trim(s.config.ef);
}
// 区分候选耗尽与预算停止；预算达到但无剩余候选仍视为耗尽。
Termination SearchCore::termination(const QueryState &s) {
  return s.candidates.nearest_unexpanded() != s.candidates.values.size() &&
                 s.expansions >= s.config.max_expansions
             ? Termination::Budget
             : Termination::Exhausted;
}
// 对一个最终候选计算 FP32 全精度距离，保存图 ID 以便统一排序。
void Reranker::consume(const Candidate &c, ArrayView<float> vector,
                       ArrayView<float> query) {
  require(vector.size == query.size, "rerank dimensions differ");
  float distance = 0;
  for (size_t i = 0; i < vector.size; ++i) {
    float delta = query[i] - vector[i];
    distance += delta * delta;
  }
  require(std::isfinite(distance), "rerank distance overflow");
  exact_.push_back({c.id, distance, c.expanded});
}
// 全部最终候选重排后取 top-k，并在输出边界转回数据集 ID。
void Reranker::finish(uint32_t topk, const IndexMetadata &meta, Result &r) {
  std::sort(exact_.begin(), exact_.end(), candidate_less);
  for (size_t i = 0; i < std::min(size_t(topk), exact_.size()); ++i) {
    r.ids.push_back(meta.graph_to_dataset[exact_[i].id]);
    r.distances.push_back(exact_[i].distance);
  }
}
}  // namespace anns
