#include <functional>
#include <iostream>

#include "anns/runtime/host_coordinator.hpp"
#include "anns/runtime/soc_worker.hpp"
using namespace anns;
// 断言非法输入或故障路径明确失败，避免错误被当成有效空结果。
static void fails(const std::function<void()> &fn) {
  bool failure = false;
  try {
    fn();
  } catch (const std::exception &) {
    failure = true;
  }
  require(failure, "expected failure did not occur");
}
// 构造含空簇和非恒等 ID 映射的小图，提供独立的预期搜索行为。
static HostIndex fixture() {
  HostIndex index;
  auto &m = index.meta;
  m.n = 5;
  m.d = 32;
  m.c = 3;
  m.rmax = 4;
  m.stride = 20;
  m.entry = 0;
  m.identity = "manual-v1";
  m.graph_to_dataset = {4, 0, 2, 1, 3};
  m.graph_to_cluster = {0, 0, 2, 2, 2};
  m.cluster_offsets = {0, 2, 2, 5};
  m.validate();
  index.graph = {4, 1,          2,          3,          4,
                 1, 4,          UINT32_MAX, UINT32_MAX, UINT32_MAX,
                 2, 0,          4,          UINT32_MAX, UINT32_MAX,
                 0, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
                 0, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
  index.pq_codes.resize(5 * 32);
  index.centroids.resize(256 * 32);
  index.base.resize(5 * 32);
  for (uint32_t id = 0; id < 5; ++id) {
    index.pq_codes[id * 32] = uint8_t(id);
    index.base[m.graph_to_dataset[id] * 32] = float(id);
  }
  for (uint32_t code = 0; code < 256; ++code)
    index.centroids[code] = float(code);
  return index;
}
// 比较结果 ID、精确距离及终止原因，验证交接不改变查询语义。
static void same(const Result &a, const Result &b) {
  require(a.ids == b.ids && a.distances == b.distances &&
              a.termination == b.termination,
          "result mismatch");
}
// 验证手工图、交接边界、预取屏障、异步故障清理及统计模式。
int main() {
  try {
    auto index = fixture();
    auto invalid_meta = index.meta;
    invalid_meta.cluster_offsets[1] = 6;
    fails([&] { invalid_meta.validate(); });
    SearchConfig search;
    search.ef = 4;
    search.topk = 2;
    search.w = 0.5;
    search.x = 0.5;
    RuntimeConfig runtime;
    runtime.metrics = "trace";
    runtime.trace_limit = 100;
    runtime.chunk_bytes = 16;
    runtime.in_flight = 2;
    runtime.reverse_completion = true;
    runtime.delay_ticks = 3;
    std::vector<float> query(32);
    ArrayView<float> q{query.data(), query.size()};
    auto baseline = run_baseline(index, q, search, runtime);
    require(baseline.trace == std::vector<uint32_t>({0, 1, 2, 3}) &&
                baseline.ids == std::vector<uint32_t>({4, 0}),
            "manual graph expected behavior mismatch");
    require(baseline.predicted_nodes == 3 && baseline.host.visited_hits == 3,
            "prediction/visited semantics mismatch");
    auto state = SearchCore::begin(index, q, search);
    QueryWorkspace ws;
    WorkStats work;
    require(SearchCore::prepare_next(state) &&
                !SearchCore::maybe_trigger(state, index.meta),
            "entry should not trigger");
    SearchCore::expand(state, index.adjacency(0), index.pq_codes, index.meta,
                       ws, work);
    require(SearchCore::prepare_next(state) &&
                SearchCore::maybe_trigger(state, index.meta),
            "rank 2 must trigger");
    require(state.pending_id == 1 && state.expansions == 1 &&
                !state.candidates.values[1].expanded &&
                state.visited.bytes[4] == 1,
            "invalid pause boundary");
    auto snapshot = encode_handoff(state, index.meta, 7, 9, 1000);
    Bytes header(snapshot.begin(), snapshot.begin() + 24);
    auto h = decode_handoff_header(header, 100000);
    Bytes prefix(snapshot.begin() + 24,
                 snapshot.begin() + static_cast<std::ptrdiff_t>(h.prefix)),
        body(snapshot.begin() + static_cast<std::ptrdiff_t>(h.prefix),
             snapshot.end());
    auto pre = decode_preamble(prefix, index.meta, 7, 9, 1000);
    auto restored = restore_handoff(body, pre, index.meta);
    require(restored.pending_id == 1 &&
                restored.predicted_clusters == std::vector<uint32_t>({2}) &&
                restored.visited.bytes[4] == 1,
            "snapshot restore mismatch");
    fails([&] { decode_preamble(prefix, index.meta, 7, 10, 1000); });
    fails([&] { decode_preamble(prefix, index.meta, 7, 9, 1); });
    auto bad = body;
    bad.back() = 2;
    fails([&] { restore_handoff(bad, pre, index.meta); });
    bad = body;
    bad.pop_back();
    fails([&] { restore_handoff(bad, pre, index.meta); });
    MemoryRegistry registry;
    SimulatedReadTransport handoff(registry, runtime), miss(registry, runtime),
        vectors(registry, runtime);
    Mailbox host_box, soc_box;
    SimulatedControlChannel host(host_box, soc_box, 4096),
        soc(soc_box, host_box, 4096);
    SocWorker worker(soc, {&handoff, &miss, &vectors}, runtime);
    HostCoordinator coordinator(
        index, host, registry, runtime, [&] { worker.tick(); }, 7);
    coordinator.initialize();
    for (int i = 0; i < 2; ++i) {
      auto result = coordinator.run(q, search);
      same(result, baseline);
      auto trace = result.trace;
      trace.insert(trace.end(), worker.diagnostics.trace.begin(),
                   worker.diagnostics.trace.end());
      require(trace == baseline.trace, "resumed trace mismatch");
      require(result.soc.graph_misses == 1 && result.soc.graph_hits == 2,
              "prefetch not used or trigger not missed");
      require(result.soc.start >= result.soc.ds &&
                  result.soc.start >= result.soc.dp,
              "premature search start");
      require(
          result.soc.transfers[size_t(Purpose::Prefetch)].completed_bytes == 60,
          "predicted graph was not actually copied");
      require(result.soc.rerank_vectors == 4,
              "must rerank all final candidates");
    }
    search.max_expansions = 2;
    auto capped = coordinator.run(q, search);
    same(capped, run_baseline(index, q, search, runtime));
    require(capped.termination == Termination::Budget &&
                capped.host.expansions + capped.soc.work.expansions == 2,
            "budget reset on handoff");
    search.max_expansions = 100;
    search.w = 1;
    auto empty = coordinator.run(q, search);
    require(empty.offloaded && empty.predicted_clusters == 0 &&
                empty.soc.transfers[size_t(Purpose::Prefetch)].tasks == 0,
            "empty prediction handling failed");
    search.ef = 64;
    search.topk = 2;
    search.w = 0.75;
    auto local = coordinator.run(q, search);
    require(!local.offloaded && local.h1 == 0 && local.h2 == 0,
            "untriggered path handling failed");
    Bytes source(128, 42), target(128);
    registry.bind(9, source.data(), source.size());
    RuntimeConfig fault = runtime;
    fault.fail_task = 3;
    SimulatedReadTransport faulty(registry, fault);
    DeviceStats stats;
    fails([&] {
      read_one(faulty, {9, 0, 128, target.data(), Purpose::Prefetch}, stats);
    });
    require(faulty.pending() == 0 &&
                stats.transfers[size_t(Purpose::Prefetch)].failures == 1,
            "failed tasks were not drained");
    fails([&] {
      read_one(faulty, {9, 128, 1, target.data(), Purpose::GraphMiss}, stats);
    });
    auto encoded =
        encode_message({MessageType::Complete, 7, 9, encode_result(baseline)});
    fails([&] { decode_message(encoded, 10); });
    search.ef = 4;
    search.topk = 2;
    search.w = 0.5;
    search.max_expansions = 100;
    RuntimeConfig query_fault = runtime;
    query_fault.fail_task =
        (encode_soc_assets(index).size() + runtime.chunk_bytes - 1) /
            runtime.chunk_bytes +
        1;
    MemoryRegistry fault_registry;
    SimulatedReadTransport fault_handoff(fault_registry, query_fault),
        fault_miss(fault_registry, runtime),
        fault_vectors(fault_registry, runtime);
    Mailbox fh, fs;
    SimulatedControlChannel fhc(fh, fs, 4096), fsc(fs, fh, 4096);
    SocWorker fault_worker(fsc, {&fault_handoff, &fault_miss, &fault_vectors},
                           runtime);
    HostCoordinator fault_host(
        index, fhc, fault_registry, runtime, [&] { fault_worker.tick(); }, 19);
    fault_host.initialize();
    auto failure = fault_host.run(q, search);
    require(failure.termination == Termination::Error && failure.safe &&
                fault_handoff.pending() == 0,
            "query failure was not safely reported");
    same(fault_host.run(q, search), baseline);
    require(soc.send(encode_message(
                {MessageType::Complete, 6, 100, encode_result(baseline)})),
            "old result injection failed");
    same(coordinator.run(q, search), baseline);
    search.prediction = false;
    auto disabled = coordinator.run(q, search);
    same(disabled, run_baseline(index, q, search, runtime));
    require(disabled.predicted_clusters == 0,
            "disabled prediction should still permit a handoff");
    auto off = runtime;
    off.metrics = "off";
    same(run_baseline(index, q, search, off),
         run_baseline(index, q, search, runtime));
    MemoryRegistry quiet_registry;
    SimulatedReadTransport quiet_handoff(quiet_registry, off),
        quiet_miss(quiet_registry, off), quiet_vectors(quiet_registry, off);
    Mailbox qh, qs;
    SimulatedControlChannel qhc(qh, qs, 4096), qsc(qs, qh, 4096);
    SocWorker quiet_worker(qsc, {&quiet_handoff, &quiet_miss, &quiet_vectors},
                           off);
    HostCoordinator quiet_host(
        index, qhc, quiet_registry, off, [&] { quiet_worker.tick(); }, 23);
    quiet_host.initialize();
    auto quiet = quiet_host.run(q, search);
    same(quiet, run_baseline(index, q, search, off));
    require(quiet.soc.start == 0 && quiet.soc.work.expansions == 0 &&
                quiet.soc.transfers[size_t(Purpose::State)].tasks == 0,
            "off mode retained optional instrumentation");
    std::cout
        << "PASS: manual graph, rank window, visited, snapshot, simulated "
           "transfer, barrier, rerank, lifecycle, budget and failures\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
