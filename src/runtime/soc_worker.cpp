#include "anns/runtime/soc_worker.hpp"
namespace anns {
// 通过读取后端装载常驻 PQ 与映射，验证成功后发送 READY。
void SocWorker::initialize(const Message &message) {
  require(!ready_ && message.session > 0 && message.query == 0,
          "invalid INIT lifecycle");
  init_stats.enabled = config_.metrics != "off";
  auto init = decode_init(message.payload);
  require(init.asset_bytes <= config_.resident_capacity,
          "resident metadata/PQ capacity exceeded");
  Bytes assets(init.asset_bytes);
  read_one(routes_.select(Purpose::Init),
           {3, 0, assets.size(), assets.data(), Purpose::Init}, init_stats);
  index_ = decode_soc_assets(assets, std::move(init.meta));
  session_ = message.session;
  last_query_ = 0;
  ready_ = true;
  send_message(control_, {MessageType::Ready, session_, 0, {}});
}
// 读取前导后安排状态与全部预测簇，完整就绪才开始续搜。
// 缺失图记录和重排向量分别取数；确认在途读取结束后返回结果。
void SocWorker::execute(const Message &message) {
  require(ready_ && message.session == session_ && message.query > last_query_,
          "stale session/query submission");
  last_query_ = message.query;
  Result result;
  result.session = session_;
  result.query_id = message.query;
  result.offloaded = true;
  result.soc.enabled = config_.metrics != "off";
  result.soc.work.enabled = result.soc.enabled;
  auto stamp = [&] { return result.soc.enabled ? now_ns() : 0; };
  result.soc.d0 = stamp();
  try {
    auto desc = decode_submit(message.payload);
    require(desc.bytes <= config_.state_capacity, "state capacity exceeded");
    Bytes header(handoff_header_bytes);
    read_one(routes_.select(Purpose::State),
             {desc.region, desc.offset, header.size(), header.data(),
              Purpose::State},
             result.soc);
    auto h = decode_handoff_header(header, config_.state_capacity);
    require(h.total == desc.bytes, "descriptor size mismatch");
    Bytes prefix(h.prefix - handoff_header_bytes);
    read_one(routes_.select(Purpose::State),
             {desc.region, desc.offset + handoff_header_bytes, prefix.size(),
              prefix.data(), Purpose::State},
             result.soc);
    auto p = decode_preamble(prefix, index_.meta, session_, message.query,
                             config_.prefetch_capacity);
    Result size_check;
    size_check.ids.resize(p.config.topk);
    size_check.distances.resize(p.config.topk);
    require(encode_message({MessageType::Complete, session_, message.query,
                            encode_result(size_check)})
                    .size() <= control_.capacity(),
            "result exceeds channel capacity");
    Bytes body(h.total - h.prefix);
    PrefetchedGraph graph;
    graph.plan = p.plan;
    graph.records.resize(p.plan.total_bytes / 4);
    QueryState state;
    bool state_ready = false;
    size_t prefetch_groups = p.plan.ranges.size();
    uint64_t finished_prefetch = 0;
    std::vector<ReadRequest> requests{{desc.region, desc.offset + h.prefix,
                                       body.size(), body.data(),
                                       Purpose::State}};
    for (const auto &range : p.plan.ranges)
      requests.push_back(
          {1, range.offset, range.bytes,
           range.bytes ? reinterpret_cast<uint8_t *>(graph.records.data()) +
                             range.local_offset
                       : nullptr,
           Purpose::Prefetch});
    if (prefetch_groups == 0) result.soc.dp = stamp();
    read_batch(routes_.select(Purpose::State), requests, result.soc,
               [&](size_t group) {
                 if (group == 0) {
                   state = restore_handoff(body, p, index_.meta);
                   state_ready = true;
                   result.soc.ds = stamp();
                 } else if (++finished_prefetch == prefetch_groups)
                   result.soc.dp = stamp();
               });
    require(state_ready && finished_prefetch == prefetch_groups,
            "incomplete readiness barrier");
    graph.ready = true;
    result.predicted_clusters = p.plan.ranges.size();
    result.predicted_nodes = p.predicted_nodes;
    result.soc.start = stamp();
    QueryWorkspace workspace;
    std::vector<uint32_t> missing(index_.meta.rmax + 1);
    bool first = true;
    while (SearchCore::prepare_next(state)) {
      auto *record = graph.find(state.pending_id, index_.meta);
      if (record) {
        if (result.soc.enabled) ++result.soc.graph_hits;
      } else {
        if (result.soc.enabled) ++result.soc.graph_misses;
        uint64_t start = stamp();
        read_one(
            routes_.select(Purpose::GraphMiss),
            {1, uint64_t(state.pending_id) * index_.meta.stride,
             index_.meta.stride, reinterpret_cast<uint8_t *>(missing.data()),
             Purpose::GraphMiss},
            result.soc);
        uint64_t elapsed = stamp() - start;
        result.soc.wait_ns += elapsed;
        if (first) result.soc.first_wait_ns = elapsed;
        record = missing.data();
      }
      if (config_.metrics == "trace" &&
          result.trace.size() < config_.trace_limit) {
        result.trace.push_back(state.pending_id);
        result.ranks.push_back(state.pending_index + 1);
      }
      SearchCore::expand(
          state, decode_adjacency(record, index_.meta.rmax, index_.meta.n),
          index_.pq_codes, index_.meta, workspace, result.soc.work);
      first = false;
    }
    result.soc.end = stamp();
    result.termination = SearchCore::termination(state);
    result.pq_candidates = state.candidates.values;
    Reranker reranker;
    std::vector<float> vector(index_.meta.d);
    for (const auto &candidate : state.candidates.values) {
      uint64_t offset =
          checked_mul(checked_mul(index_.meta.graph_to_dataset[candidate.id],
                                  index_.meta.d),
                      4);
      uint64_t start = stamp();
      read_one(routes_.select(Purpose::Vectors),
               {2, offset, uint64_t(index_.meta.d) * 4,
                reinterpret_cast<uint8_t *>(vector.data()), Purpose::Vectors},
               result.soc);
      result.soc.wait_ns += stamp() - start;
      reranker.consume(candidate, {vector.data(), vector.size()},
                       {state.query.data(), state.query.size()});
      if (result.soc.enabled) ++result.soc.rerank_vectors;
    }
    reranker.finish(state.config.topk, index_.meta, result);
    result.soc.rerank = stamp();
  } catch (const std::exception &e) {
    result.termination = Termination::Error;
    result.error = std::string(e.what()).substr(0, 256);
  }
  require(routes_.select(Purpose::State).pending() == 0 &&
              routes_.select(Purpose::GraphMiss).pending() == 0 &&
              routes_.select(Purpose::Vectors).pending() == 0,
          "query resources still in flight");
  result.soc.done = stamp();
  result.safe = true;
  diagnostics = result;
  send_message(control_, {result.termination == Termination::Error
                              ? MessageType::QueryError
                              : MessageType::Complete,
                          session_, message.query, encode_result(result)});
}
// 推进控制通道并处理一条 INIT 或 SUBMIT；初始化失败返回明确错误。
void SocWorker::tick() {
  control_.progress();
  Bytes bytes;
  if (!control_.receive(bytes)) return;
  auto message = decode_message(bytes, control_.capacity());
  if (message.type == MessageType::Init) {
    try {
      initialize(message);
    } catch (const std::exception &e) {
      Writer w;
      w.string(std::string(e.what()).substr(0, 256));
      send_message(control_,
                   {MessageType::InitError, message.session, 0, w.data});
    }
  } else if (message.type == MessageType::Submit)
    execute(message);
  else
    throw std::runtime_error("unsupported worker message");
}
}  // namespace anns
