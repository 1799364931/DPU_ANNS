#include <iostream>
#include <thread>

#include "anns/runtime/soc_worker.hpp"
#include "anns/transport/doca.hpp"
// 装配真实 SoC Comch/DMA，接收区域导出并持续驱动单 worker。
int main(int argc, char **argv) {
  using namespace anns;
  try {
    require(argc == 7 && std::string(argv[1]) == "--config" &&
                std::string(argv[3]) == "--pci" &&
                std::string(argv[5]) == "--representor",
            "usage: anns_soc --config <JSON> --pci <SoC BDF> --representor "
            "<Host representor BDF>");
    auto cfg = load_config(argv[2]);
    require(cfg.runtime.handoff_prefetch == "dma" &&
                cfg.runtime.graph_miss == "dma" &&
                cfg.runtime.rerank_vectors == "dma",
            "v1 real routes must each select dma");
    require(cfg.runtime.in_flight <= UINT32_MAX, "task capacity overflow");
    DocaDevice device(argv[4], argv[6]);
    DocaControlChannel control(device, true, "anns-v1",
                               cfg.runtime.max_message);
    DocaDmaTransport dma(device, cfg.runtime.chunk_bytes,
                         uint32_t(cfg.runtime.in_flight));
    DocaRegionReceiver receiver(dma, 0);
    control.internal_handler = [&](const Message &message) {
      return receiver.handle(message);
    };
    SocWorker worker(control, {&dma, &dma, &dma}, cfg.runtime);
    for (;;) {
      worker.tick();
      std::this_thread::yield();
    }
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
