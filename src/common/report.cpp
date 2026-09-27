#include "anns/common/report.hpp"

#include <fstream>
#include <iostream>
#include <numeric>
#include <set>

#include "anns/runtime/soc_worker.hpp"
namespace anns {
static void put(json_object *o, const char *name, uint64_t value) {
  json_object_object_add(o, name, json_object_new_uint64(value));
}
static void put(json_object *o, const char *name, const std::string &value) {
  json_object_object_add(o, name, json_object_new_string(value.c_str()));
}
static json_object *ids_json(const std::vector<uint32_t> &ids) {
  auto *a = json_object_new_array();
  for (auto id : ids) json_object_array_add(a, json_object_new_int64(id));
  return a;
}
// 按数据集 ID 与 ground truth 计算 Recall@k。
static double recall(const HostIndex &index, uint32_t query,
                     const Result &result, uint32_t topk) {
  std::set<uint32_t> truth;
  for (uint32_t i = 0; i < std::min(topk, index.truth_width); ++i)
    truth.insert(index.truth[uint64_t(query) * index.truth_width + i]);
  uint32_t matches = 0;
  for (auto id : result.ids) matches += truth.count(id) != 0;
  return double(matches) / topk;
}
// 与完整基线比较结果和终止状态，并对交接路径核对扩展和候选。
static void verify(const Result &expected, const Result &actual,
                   const Result &worker) {
  require(actual.termination == expected.termination &&
              actual.ids == expected.ids &&
              actual.distances == expected.distances,
          "baseline topk/termination mismatch");
  if (!actual.offloaded) {
    require(actual.trace == expected.trace && actual.ranks == expected.ranks,
            "Host trace mismatch");
    return;
  }
  auto trace = actual.trace;
  auto ranks = actual.ranks;
  trace.insert(trace.end(), worker.trace.begin(), worker.trace.end());
  ranks.insert(ranks.end(), worker.ranks.begin(), worker.ranks.end());
  require(trace == expected.trace && ranks == expected.ranks,
          "resumed expansion trace mismatch");
  require(worker.pq_candidates.size() == expected.pq_candidates.size(),
          "PQ candidate count mismatch");
  for (size_t i = 0; i < worker.pq_candidates.size(); ++i) {
    const auto &a = worker.pq_candidates[i];
    const auto &b = expected.pq_candidates[i];
    require(
        a.id == b.id && a.distance == b.distance && a.expanded == b.expanded,
        "resumed candidate mismatch");
  }
  require(actual.host.expansions + actual.soc.work.expansions ==
              expected.host.expansions,
          "expansion budget mismatch");
}
static json_object *work_json(const WorkStats &s) {
  auto *o = json_object_new_object();
  put(o, "expansions", s.expansions);
  put(o, "pq_distances", s.distances);
  put(o, "visited_hits", s.visited_hits);
  put(o, "convergence_expansions", s.convergence);
  put(o, "prediction_hits", s.prediction_hits);
  return o;
}
// 构建逐查询 JSON，按 off/basic/trace 模式选择统计字段。
static json_object *row_json(uint32_t query, const Result &r, double hit,
                             const RuntimeConfig &cfg) {
  auto *o = json_object_new_object();
  put(o, "query_index", query);
  put(o, "query_id", r.query_id);
  put(o, "backend", r.offloaded ? cfg.handoff_prefetch : "host_local");
  put(o, "termination",
      r.termination == Termination::Error    ? "error"
      : r.termination == Termination::Budget ? "max_expansions"
                                             : "candidate_exhausted");
  put(o, "error", r.error);
  put(o, "offloaded", r.offloaded ? 1 : 0);
  json_object_object_add(o, "ids_dataset", ids_json(r.ids));
  json_object_object_add(o, "recall", json_object_new_double(hit));
  put(o, "e2e_ns", r.h3 - r.h0);
  if (cfg.metrics != "off") {
    json_object_object_add(o, "host_work", work_json(r.host));
    json_object_object_add(o, "soc_work", work_json(r.soc.work));
    put(o, "predicted_clusters", r.predicted_clusters);
    put(o, "predicted_nodes", r.predicted_nodes);
    if (r.offloaded) {
      put(o, "host_prefix_ns", r.h1 - r.h0);
      put(o, "prepare_submit_ns", r.h2 - r.h1);
      put(o, "submitted_wait_ns", r.h3 - r.h2);
      auto *d = json_object_new_object();
      const auto &s = r.soc;
      auto duration = [&](const char *name, uint64_t end, uint64_t begin) {
        if (end && begin && end >= begin) put(d, name, end - begin);
      };
      duration("state_restore_ns", s.ds, s.d0);
      duration("prefetch_ns", s.dp, s.d0);
      duration("start_wait_ns", s.start, s.d0);
      duration("search_ns", s.end, s.start);
      duration("rerank_ns", s.rerank, s.end);
      duration("result_prepare_ns", s.done, s.rerank);
      put(d, "first_missing_wait_ns", s.first_wait_ns);
      put(d, "read_wait_ns", s.wait_ns);
      put(d, "prefetch_hits", s.graph_hits);
      put(d, "graph_misses", s.graph_misses);
      put(d, "rerank_vectors", s.rerank_vectors);
      json_object_object_add(o, "soc", d);
      auto *transfers = json_object_new_object();
      const char *names[] = {"initialization", "state", "prefetch",
                             "graph_miss", "vectors"};
      for (size_t i = 0; i < size_t(Purpose::Count); ++i) {
        const auto &t = s.transfers[i];
        auto *item = json_object_new_object();
        put(item, "logical_requests", t.logical_requests);
        put(item, "logical_bytes", t.logical_bytes);
        put(item, "tasks", t.tasks);
        put(item, "submitted_bytes", t.submitted_bytes);
        put(item, "completed_bytes", t.completed_bytes);
        put(item, "failures", t.failures);
        put(item, "sum_task_ns", t.task_ns);
        json_object_object_add(transfers, names[i], item);
      }
      json_object_object_add(o, "transfers", transfers);
    }
  }
  if (cfg.metrics == "trace") {
    json_object_object_add(o, "expansion_trace_graph", ids_json(r.trace));
    json_object_object_add(o, "rank_trace", ids_json(r.ranks));
    put(o, "trace_truncated", r.trace.size() >= cfg.trace_limit ? 1 : 0);
  }
  return o;
}
// 查询结束后输出一行结果与 Recall，避免在搜索热路径写文件。
void write_result(std::ostream &out, const HostIndex &index, uint32_t query,
                  const Result &result, uint32_t topk,
                  const RuntimeConfig &runtime) {
  auto *row =
      row_json(query, result, recall(index, query, result, topk), runtime);
  out << json_object_to_json_string_ext(row, JSON_C_TO_STRING_PLAIN) << '\n';
  json_object_put(row);
}
// 运行软件基线或模拟闭环，完成预热、可选基线对照及独立结果汇总。
int run_application(int argc, char **argv, bool simulated) {
  try {
    require(argc == 3 && std::string(argv[1]) == "--config",
            "usage: --config <JSON>");
    auto cfg = load_config(argv[2]);
    require(!std::filesystem::exists(cfg.output),
            "output directory already exists");
    std::filesystem::create_directories(cfg.output);
    uint64_t load_start = now_ns();
    auto index = HostIndex::load(cfg.manifest);
    uint64_t load_ns = now_ns() - load_start;
    RuntimeConfig runtime = cfg.runtime;
    if (cfg.compare) {
      runtime.metrics = "trace";
      runtime.trace_limit = cfg.search.max_expansions;
    }
    MemoryRegistry registry;
    SimulatedReadTransport handoff(registry, runtime), miss(registry, runtime),
        vectors(registry, runtime);
    ReadRoutes routes{&handoff, &miss, &vectors};
    Mailbox hbox, sbox;
    SimulatedControlChannel hc(hbox, sbox, runtime.max_message),
        sc(sbox, hbox, runtime.max_message);
    SocWorker worker(sc, routes, runtime);
    HostCoordinator coordinator(index, hc, registry, runtime,
                                [&] { worker.tick(); });
    uint64_t init_start = now_ns();
    if (simulated) {
      require(runtime.handoff_prefetch == "simulated" &&
                  runtime.graph_miss == "simulated" &&
                  runtime.rerank_vectors == "simulated",
              "sim application requires three simulated routes");
      coordinator.initialize();
    }
    uint64_t init_ns = now_ns() - init_start;
    uint32_t count = cfg.query_limit == 0
                         ? index.query_count
                         : std::min(cfg.query_limit, index.query_count);
    require(cfg.warmup <= index.query_count, "warmup exceeds query set");
    for (uint32_t q = 0; q < cfg.warmup; ++q) {
      ArrayView<float> query{index.queries.data() + uint64_t(q) * index.meta.d,
                             index.meta.d};
      if (simulated)
        (void)coordinator.run(query, cfg.search);
      else
        (void)run_baseline(index, query, cfg.search, runtime);
    }
    std::ofstream out(cfg.output / "queries.jsonl");
    require(bool(out), "cannot create output");
    std::vector<uint64_t> elapsed;
    double mean_recall = 0;
    uint64_t offloaded = 0, errors = 0, truncated = 0;
    for (uint32_t q = 0; q < count; ++q) {
      ArrayView<float> query{index.queries.data() + uint64_t(q) * index.meta.d,
                             index.meta.d};
      auto result = simulated ? coordinator.run(query, cfg.search)
                              : run_baseline(index, query, cfg.search, runtime);
      result.query_id = simulated ? result.query_id : q + 1;
      if (cfg.compare && result.termination != Termination::Error) {
        auto expected = run_baseline(index, query, cfg.search, runtime);
        verify(expected, result, worker.diagnostics);
      }
      if (result.offloaded && cfg.runtime.metrics == "trace") {
        result.trace.insert(result.trace.end(),
                            worker.diagnostics.trace.begin(),
                            worker.diagnostics.trace.end());
        result.ranks.insert(result.ranks.end(),
                            worker.diagnostics.ranks.begin(),
                            worker.diagnostics.ranks.end());
        if (result.trace.size() > cfg.runtime.trace_limit) {
          result.trace.resize(cfg.runtime.trace_limit);
          result.ranks.resize(cfg.runtime.trace_limit);
        }
      }
      double hit = recall(index, q, result, cfg.search.topk);
      mean_recall += hit;
      offloaded += result.offloaded;
      errors += result.termination == Termination::Error;
      truncated += result.termination == Termination::Budget;
      elapsed.push_back(result.h3 - result.h0);
      auto *row = row_json(q, result, hit, cfg.runtime);
      out << json_object_to_json_string_ext(row, JSON_C_TO_STRING_PLAIN)
          << '\n';
      json_object_put(row);
    }
    out.close();
    std::sort(elapsed.begin(), elapsed.end());
    auto *summary = json_object_new_object();
    put(summary, "backend", simulated ? "simulated" : "host_local");
    put(summary, "index_id", index.meta.identity);
    put(summary, "queries", count);
    put(summary, "offloaded", offloaded);
    put(summary, "errors", errors);
    put(summary, "truncated", truncated);
    put(summary, "never_offloaded", count - offloaded);
    put(summary, "warmup", cfg.warmup);
    put(summary, "baseline_comparison", cfg.compare ? 1 : 0);
    put(summary, "load_ns", load_ns);
    put(summary, "initialize_ns", init_ns);
    put(summary, "metrics", cfg.runtime.metrics);
    put(summary, "ef", cfg.search.ef);
    put(summary, "topk", cfg.search.topk);
    json_object_object_add(
        summary, "mean_recall",
        json_object_new_double(count ? mean_recall / count : 0));
    for (const auto &item : std::vector<std::pair<const char *, double>>{
             {"p50_ns", 0.5}, {"p95_ns", 0.95}, {"p99_ns", 0.99}})
      if (!elapsed.empty())
        put(summary, item.first,
            elapsed[std::min(
                elapsed.size() - 1,
                size_t(std::ceil(item.second *
                                 static_cast<double>(elapsed.size())) -
                       1))]);
    json_object_to_file_ext((cfg.output / "summary.json").c_str(), summary,
                            JSON_C_TO_STRING_PRETTY);
    std::cout << json_object_to_json_string_ext(summary, JSON_C_TO_STRING_PLAIN)
              << '\n';
    json_object_put(summary);
    std::filesystem::copy_file(argv[2], cfg.output / "config.json");
    return errors ? 2 : 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
}  // namespace anns
