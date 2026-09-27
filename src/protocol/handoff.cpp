#include "anns/protocol/handoff.hpp"
namespace anns {
// 校验快照版本、总长度和前导长度，使后续分段读取受容量约束。
HandoffHeader decode_handoff_header(const Bytes &header, uint64_t capacity) {
  Reader r(header);
  require(r.u32() == handoff_magic && r.u32() == 1,
          "unsupported handoff version");
  HandoffHeader h;
  h.total = r.u64();
  h.prefix = r.u64();
  r.finish();
  require(h.total <= capacity && h.total >= handoff_header_bytes &&
              h.prefix >= handoff_header_bytes && h.prefix <= h.total,
          "invalid handoff lengths");
  return h;
}
// 在触发节点扩展前编码前导与状态体，保存原候选距离和完整 visited。
Bytes encode_handoff(const QueryState &s, const IndexMetadata &m,
                     uint64_t session, uint64_t query, uint64_t capacity) {
  require(s.triggered && s.pending_index == s.candidates.nearest_unexpanded(),
          "snapshot must precede trigger expansion");
  auto plan = build_prefetch_plan(s, m, capacity);
  Writer p, b;
  p.u64(session);
  p.u64(query);
  p.string(m.identity);
  p.u32(m.n);
  p.u32(m.d);
  p.u32(s.config.ef);
  p.u32(s.config.topk);
  p.u32(s.config.max_expansions);
  p.f64(s.config.w);
  p.f64(s.config.x);
  p.u32(s.config.prediction ? 1 : 0);
  p.u32(uint32_t(s.candidates.values.size()));
  p.u32(s.expansions);
  p.u32(s.pending_id);
  p.u32(s.pending_index);
  p.u64(s.predicted_nodes);
  p.u32(1);
  p.u32(1);  // triggered and uint8 exact visited encoding
  p.u32(uint32_t(plan.ranges.size()));
  for (const auto &range : plan.ranges) {
    p.u32(range.cluster);
    p.u64(range.offset);
    p.u64(range.bytes);
  }
  for (float value : s.query) b.f32(value);
  for (float value : s.lut) b.f32(value);
  for (const auto &c : s.candidates.values) {
    b.u32(c.id);
    b.f32(c.distance);
    uint8_t flag = c.expanded ? 1 : 0;
    b.bytes(&flag, 1);
  }
  b.bytes(s.visited.bytes.data(), s.visited.bytes.size());
  Writer w;
  w.u32(handoff_magic);
  w.u32(1);
  w.u64(handoff_header_bytes + p.data.size() + b.data.size());
  w.u64(handoff_header_bytes + p.data.size());
  w.bytes(p.data.data(), p.data.size());
  w.bytes(b.data.data(), b.data.size());
  return w.data;
}
// 校验会话、查询、数据身份和预测簇范围，并生成 SoC 本地预取偏移。
HandoffPreamble decode_preamble(const Bytes &bytes, const IndexMetadata &m,
                                uint64_t session, uint64_t query,
                                uint64_t capacity) {
  Reader r(bytes);
  HandoffPreamble p;
  p.session = r.u64();
  p.query_id = r.u64();
  p.identity = r.string();
  require(
      p.session == session && p.query_id == query && p.identity == m.identity,
      "handoff identity mismatch");
  p.n = r.u32();
  p.d = r.u32();
  require(p.n == m.n && p.d == m.d, "handoff dataset shape mismatch");
  p.config.ef = r.u32();
  p.config.topk = r.u32();
  p.config.max_expansions = r.u32();
  p.config.w = r.f64();
  p.config.x = r.f64();
  auto prediction = r.u32();
  require(prediction <= 1, "invalid prediction flag");
  p.config.prediction = prediction != 0;
  p.config.validate();
  p.candidate_count = r.u32();
  p.expansions = r.u32();
  p.pending_id = r.u32();
  p.pending_index = r.u32();
  p.predicted_nodes = r.u64();
  require(r.u32() == 1 && r.u32() == 1, "unsupported triggered/visited state");
  require(p.candidate_count > 0 && p.candidate_count <= p.config.ef &&
              p.pending_index < p.candidate_count && p.pending_id < m.n &&
              p.expansions < p.config.max_expansions &&
              p.pending_index + 1 >= p.config.w_rank(),
          "invalid handoff progress");
  auto count = r.u32();
  require(count <= m.c && count <= p.config.x_count(),
          "predicted cluster count overflow");
  uint64_t nodes = 0;
  for (uint32_t i = 0; i < count; ++i) {
    ClusterRange range;
    range.cluster = r.u32();
    range.offset = r.u64();
    range.bytes = r.u64();
    range.local_offset = p.plan.total_bytes;
    require(range.cluster < m.c, "bad cluster ID");
    for (const auto &old : p.plan.ranges)
      require(old.cluster != range.cluster, "duplicate cluster");
    require(
        range.offset ==
                checked_mul(m.cluster_offsets[range.cluster], m.stride) &&
            range.bytes == checked_mul(m.cluster_offsets[range.cluster + 1] -
                                           m.cluster_offsets[range.cluster],
                                       m.stride),
        "prefetch range mismatch");
    bounds(p.plan.total_bytes, range.bytes, capacity);
    p.plan.total_bytes += range.bytes;
    nodes += range.bytes / m.stride;
    p.plan.ranges.push_back(range);
  }
  require(nodes == p.predicted_nodes, "predicted nodes mismatch");
  require(p.config.prediction || count == 0,
          "disabled prediction with clusters");
  r.finish();
  return p;
}
// 恢复查询向量、LUT、候选与 visited，核对排序和待扩展节点。
// 不重算候选距离，不重新触发预测，也不重置扩展预算。
QueryState restore_handoff(const Bytes &body, const HandoffPreamble &p,
                           const IndexMetadata &m) {
  const uint64_t expected =
      checked_mul(p.d, 4) + 8192 * 4 + checked_mul(p.candidate_count, 9) + p.n;
  require(body.size() == expected, "state body length mismatch");
  Reader r(body);
  QueryState s;
  s.config = p.config;
  s.expansions = p.expansions;
  s.pending_id = p.pending_id;
  s.pending_index = p.pending_index;
  s.triggered = true;
  s.predicted_nodes = p.predicted_nodes;
  for (const auto &range : p.plan.ranges)
    s.predicted_clusters.push_back(range.cluster);
  for (uint32_t i = 0; i < p.d; ++i) {
    auto value = r.f32();
    require(std::isfinite(value), "nonfinite query");
    s.query.push_back(value);
  }
  for (size_t i = 0; i < 8192; ++i) {
    auto value = r.f32();
    require(std::isfinite(value) && value >= 0, "invalid LUT");
    s.lut.push_back(value);
  }
  for (uint32_t i = 0; i < p.candidate_count; ++i) {
    Candidate c;
    c.id = r.u32();
    c.distance = r.f32();
    auto flag = r.bytes(1)[0];
    require(
        flag <= 1 && c.id < m.n && std::isfinite(c.distance) && c.distance >= 0,
        "invalid candidate");
    c.expanded = flag == 1;
    s.candidates.values.push_back(c);
  }
  s.visited.bytes = r.bytes(p.n);
  r.finish();
  for (auto x : s.visited.bytes) require(x <= 1, "invalid exact visited byte");
  require(std::is_sorted(s.candidates.values.begin(), s.candidates.values.end(),
                         candidate_less),
          "unordered candidates");
  std::vector<uint32_t> ids;
  for (const auto &c : s.candidates.values) {
    require(s.visited.bytes[c.id] == 1, "candidate absent from visited");
    ids.push_back(c.id);
  }
  std::sort(ids.begin(), ids.end());
  require(std::adjacent_find(ids.begin(), ids.end()) == ids.end(),
          "duplicate candidate");
  require(s.candidates.nearest_unexpanded() == s.pending_index &&
              s.candidates.values[s.pending_index].id == s.pending_id,
          "pending node does not match first unexpanded");
  return s;
}
}  // namespace anns
