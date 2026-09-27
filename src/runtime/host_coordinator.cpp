#include "anns/runtime/host_coordinator.hpp"
namespace anns {
// 仅在启用 trace 且未达到上限时记录扩展前的图 ID 和 rank。
static void trace(Result &result, const QueryState &state,
                  const RuntimeConfig &runtime) {
  if (runtime.metrics == "trace" && result.trace.size() < runtime.trace_limit) {
    result.trace.push_back(state.pending_id);
    result.ranks.push_back(state.pending_index + 1);
  }
}
// 完成本地路径的全候选重排、结果映射和结束计时。
static void finish_host(const HostIndex &index, const QueryState &state,
                        Result &result) {
  result.termination = SearchCore::termination(state);
  result.pq_candidates = state.candidates.values;
  Reranker reranker;
  for (const auto &c : state.candidates.values)
    reranker.consume(c, {index.vector(c.id), index.meta.d},
                     {state.query.data(), state.query.size()});
  reranker.finish(state.config.topk, index.meta, result);
  result.predicted_clusters = state.predicted_clusters.size();
  result.predicted_nodes = state.predicted_nodes;
  result.h3 = now_ns();
}
// 全程在 Host 执行共享搜索语义，作为交接正确性的完整对照。
Result run_baseline(const HostIndex &index, ArrayView<float> query,
                    const SearchConfig &search, const RuntimeConfig &runtime) {
  Result result;
  result.h0 = now_ns();
  auto state = SearchCore::begin(index, query, search);
  result.host.enabled = runtime.metrics != "off";
  result.host.distances = result.host.enabled ? 1 : 0;
  QueryWorkspace workspace;
  while (SearchCore::prepare_next(state)) {
    SearchCore::maybe_trigger(state, index.meta);
    trace(result, state, runtime);
    SearchCore::expand(state, index.adjacency(state.pending_id), index.pq_codes,
                       index.meta, workspace, result.host);
  }
  finish_host(index, state, result);
  return result;
}
HostCoordinator::~HostCoordinator() {
  for (auto id : {4U, 3U, 2U, 1U}) {
    registry_.unbind(id);
  }
}
// 推进两端并等待当前 session/query 消息，忽略旧结果。
// 超时不等同于远端访问完成，交接存储由失败会话路径保留。
Message HostCoordinator::wait_for(uint64_t query) {
  uint64_t start = now_ns();
  for (;;) {
    control_.progress();
    pump_();
    Bytes bytes;
    if (control_.receive(bytes)) {
      auto message = decode_message(bytes, control_.capacity());
      if (message.session != session_ || message.query != query) continue;
      return message;
    }
    require(now_ns() - start < 60000000000ULL,
            "session timed out; handoff memory retained");
  }
}
// 导出长期图、向量与常驻资产，收到 READY 后才允许提交查询。
void HostCoordinator::initialize() {
  require(!ready_, "session already initialized");
  assets_ = encode_soc_assets(index_);
  registry_.bind(1, index_.graph.data(), uint64_t(index_.graph.size()) * 4);
  registry_.bind(2, index_.base.data(), uint64_t(index_.base.size()) * 4);
  registry_.bind(3, assets_.data(), assets_.size());
  send_message(control_, {MessageType::Init, session_, 0,
                          encode_init(index_, assets_.size())});
  auto response = wait_for(0);
  if (response.type == MessageType::InitError) {
    Reader r(response.payload);
    throw std::runtime_error("SoC init failed: " + r.string());
  }
  require(response.type == MessageType::Ready, "expected READY");
  ready_ = true;
}
// 执行逼近搜索，在触发节点扩展前冻结并发布交接区。
// 收到匹配的安全结果后撤销查询映射；未触发查询在 Host 完成。
Result HostCoordinator::run(ArrayView<float> query,
                            const SearchConfig &search) {
  require(ready_, "session not READY");
  Result result;
  result.session = session_;
  result.query_id = next_query_++;
  result.h0 = now_ns();
  auto state = SearchCore::begin(index_, query, search);
  result.host.enabled = config_.metrics != "off";
  result.host.distances = result.host.enabled ? 1 : 0;
  QueryWorkspace workspace;
  while (SearchCore::prepare_next(state)) {
    if (!state.triggered && state.pending_index + 1 >= state.config.w_rank()) {
      result.h1 = now_ns();
      SearchCore::maybe_trigger(state, index_.meta);
      result.predicted_clusters = state.predicted_clusters.size();
      result.predicted_nodes = state.predicted_nodes;
      Result size_check;
      size_check.ids.resize(search.topk);
      size_check.distances.resize(search.topk);
      require(encode_message({MessageType::Complete, session_, result.query_id,
                              encode_result(size_check)})
                      .size() <= control_.capacity(),
              "topk exceeds completion message capacity");
      handoff_ = encode_handoff(state, index_.meta, session_, result.query_id,
                                config_.prefetch_capacity);
      require(handoff_.size() <= config_.state_capacity,
              "handoff state capacity exceeded");
      registry_.bind(4, handoff_.data(), handoff_.size());
      send_message(control_, {MessageType::Submit, session_, result.query_id,
                              encode_submit({4, 0, handoff_.size()})});
      result.h2 = now_ns();
      auto message = wait_for(result.query_id);
      auto remote = decode_result(message, index_.meta.n, search.topk);
      remote.h0 = result.h0;
      remote.h1 = result.h1;
      remote.h2 = result.h2;
      remote.h3 = now_ns();
      remote.host = result.host;
      remote.trace = result.trace;
      remote.ranks = result.ranks;
      remote.predicted_clusters = result.predicted_clusters;
      remote.predicted_nodes = result.predicted_nodes;
      registry_.unbind(4);
      handoff_.clear();
      return remote;
    }
    trace(result, state, config_);
    SearchCore::expand(state, index_.adjacency(state.pending_id),
                       index_.pq_codes, index_.meta, workspace, result.host);
  }
  finish_host(index_, state, result);
  return result;
}
}  // namespace anns
