// transport/doca.hpp：真实 DOCA 设备、Comch、PCI 映射和 DMA 适配接口。
// SDK 对象封装在后端，算法层仅使用逻辑区域和读取请求。

#pragma once
#include "anns/transport/control_channel.hpp"
namespace anns {
class DocaDevice {
  void *device_ = nullptr;
  void *representor_ = nullptr;

 public:
  DocaDevice(const std::string &pci, const std::string &representor = "");
  ~DocaDevice();
  DocaDevice(const DocaDevice &) = delete;
  DocaDevice &operator=(const DocaDevice &) = delete;
  void *native() const { return device_; }
  void *representor() const { return representor_; }
};
class DocaControlChannel final : public ControlChannel {
  struct Impl;
  std::unique_ptr<Impl> impl_;

 public:
  DocaControlChannel(DocaDevice &device, bool server, const std::string &name,
                     uint64_t capacity);
  ~DocaControlChannel();
  bool send(const Bytes &) override;
  bool receive(Bytes &) override;
  void progress() override;
  uint64_t capacity() const override;
  std::function<bool(const Message &)> internal_handler;
};
class DocaDmaTransport final : public ReadTransport {
  struct Impl;
  std::unique_ptr<Impl> impl_;

 public:
  DocaDmaTransport(DocaDevice &device, uint64_t chunk_bytes, uint32_t tasks);
  ~DocaDmaTransport();
  // Backend export record: remote SDK address, region size, opaque mmap
  // descriptor.
  void import_region(uint32_t id, const Bytes &descriptor);
  Submission submit(const ReadRequest &) override;
  std::vector<ReadCompletion> progress() override;
  uint64_t max_bytes() const override;
  size_t pending() const override;
};
class DocaMemoryExporter {
  struct Impl;
  std::unique_ptr<Impl> impl_;

 public:
  DocaMemoryExporter(DocaDevice &device, ControlChannel &control,
                     uint64_t session);
  ~DocaMemoryExporter();
  void bind(uint32_t id, const Region &region);
  void unbind(uint32_t id);
};
// Bounded INIT_PART reassembly for backend memory export records.
class DocaRegionReceiver {
  DocaDmaTransport &dma_;
  uint64_t session_;
  uint32_t current_ = 0;
  uint64_t total_ = 0;
  Bytes assembled_;

 public:
  DocaRegionReceiver(DocaDmaTransport &dma, uint64_t session)
      : dma_(dma), session_(session) {}
  bool handle(const Message &message);
};
}  // namespace anns
