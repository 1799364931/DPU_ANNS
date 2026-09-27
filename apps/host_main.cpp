#include <cstdlib>
#include <fstream>
#include <iostream>

#include "anns/common/report.hpp"
#include "anns/transport/doca.hpp"
// 装配真实 Host Comch 和内存导出，依次运行单在途查询并记录结果。
int main(int argc, char **argv) {
  using namespace anns;
  try {
    require(argc == 5 && std::string(argv[1]) == "--config" &&
                std::string(argv[3]) == "--pci",
            "usage: anns_host --config <JSON> --pci <Host BDF>");
    auto cfg = load_config(argv[2]);
    require(cfg.runtime.handoff_prefetch == "dma" &&
                cfg.runtime.graph_miss == "dma" &&
                cfg.runtime.rerank_vectors == "dma",
            "v1 real routes must each select dma");
    require(!std::filesystem::exists(cfg.output), "output already exists");
    std::filesystem::create_directories(cfg.output);
    auto index = HostIndex::load(cfg.manifest);
    DocaDevice device(argv[4]);
    DocaControlChannel control(device, false, "anns-v1",
                               cfg.runtime.max_message);
    uint64_t session = now_ns();
    DocaMemoryExporter exporter(device, control, session);
    MemoryRegistry registry;
    registry.on_bind = [&](uint32_t id, const Region &region) {
      exporter.bind(id, region);
    };
    registry.on_unbind = [&](uint32_t id) { exporter.unbind(id); };
    HostCoordinator coordinator(
        index, control, registry, cfg.runtime, [] {}, session);
    // The inner catch retains all exported storage on an uncertain session
    // failure. Process teardown, rather than an unconfirmed query completion,
    // ends the failed session.
    try {
      coordinator.initialize();
      std::ofstream out(cfg.output / "queries.jsonl");
      require(bool(out), "cannot create output");
      uint32_t count = cfg.query_limit
                           ? std::min(cfg.query_limit, index.query_count)
                           : index.query_count;
      for (uint32_t query = 0; query < count; ++query) {
        auto result = coordinator.run(
            {index.queries.data() + uint64_t(query) * index.meta.d,
             index.meta.d},
            cfg.search);
        write_result(out, index, query, result, cfg.search.topk, cfg.runtime);
        if (result.termination == Termination::Error)
          std::cerr << "query " << query << ": " << result.error << '\n';
      }
      std::filesystem::copy_file(argv[2], cfg.output / "config.json");
    } catch (const std::exception &error) {
      std::cerr << "session failed; no completion-based resource reuse: "
                << error.what() << std::endl;
      std::_Exit(2);
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
