#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_comch.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_dma.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include <iostream>
#include <thread>

#include "anns/transport/doca.hpp"
namespace anns {
static void check(doca_error_t status, const char *action) {
  require(status == DOCA_SUCCESS,
          std::string(action) + ": " + doca_error_get_descr(status));
}
static doca_dev *native(DocaDevice &device) {
  return static_cast<doca_dev *>(device.native());
}
// Never destroy buffers while a context can still access them.
static void stop_context(doca_ctx *ctx, doca_pe *pe) noexcept {
  if (!ctx) return;
  doca_ctx_states initial;
  if (doca_ctx_get_state(ctx, &initial) != DOCA_SUCCESS) std::terminate();
  if (initial == DOCA_CTX_STATE_IDLE) return;
  auto status = doca_ctx_stop(ctx);
  if (status != DOCA_SUCCESS && status != DOCA_ERROR_IN_PROGRESS)
    std::terminate();
  uint64_t start = now_ns();
  doca_ctx_states state = DOCA_CTX_STATE_STOPPING;
  do {
    doca_pe_progress(pe);
    if (doca_ctx_get_state(ctx, &state) != DOCA_SUCCESS) std::terminate();
    if (now_ns() - start > 30000000000ULL) std::terminate();
  } while (state != DOCA_CTX_STATE_IDLE);
}
// 按配置的 PCI 地址打开设备，并在 SoC 端选择 Host representor。
DocaDevice::DocaDevice(const std::string &pci, const std::string &rep) {
  doca_devinfo **list = nullptr;
  uint32_t count = 0;
  check(doca_devinfo_create_list(&list, &count), "device enumeration");
  doca_dev *dev = nullptr;
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t equal = 0;
    if (doca_devinfo_is_equal_pci_addr(list[i], pci.c_str(), &equal) ==
            DOCA_SUCCESS &&
        equal) {
      auto status = doca_dev_open(list[i], &dev);
      doca_devinfo_destroy_list(list);
      check(status, "device open");
      list = nullptr;
      break;
    }
  }
  if (list) doca_devinfo_destroy_list(list);
  require(dev, "configured PCI device not found");
  device_ = dev;
  if (!rep.empty()) {
    doca_devinfo_rep **reps = nullptr;
    uint32_t size = 0;
    auto status = doca_devinfo_rep_create_list(dev, DOCA_DEVINFO_REP_FILTER_NET,
                                               &reps, &size);
    if (status != DOCA_SUCCESS) {
      doca_dev_close(dev);
      device_ = nullptr;
      check(status, "representor enumeration");
    }
    doca_dev_rep *selected = nullptr;
    for (uint32_t i = 0; i < size; ++i) {
      uint8_t equal = 0;
      if (doca_devinfo_rep_is_equal_pci_addr(reps[i], rep.c_str(), &equal) ==
              DOCA_SUCCESS &&
          equal) {
        status = doca_dev_rep_open(reps[i], &selected);
        break;
      }
    }
    doca_devinfo_rep_destroy_list(reps);
    if (!selected) {
      doca_dev_close(dev);
      device_ = nullptr;
      throw std::runtime_error("configured representor not found");
    }
    representor_ = selected;
  }
}
DocaDevice::~DocaDevice() {
  if (representor_)
    doca_dev_rep_close(static_cast<doca_dev_rep *>(representor_));
  if (device_) doca_dev_close(static_cast<doca_dev *>(device_));
}
struct DocaControlChannel::Impl {
  doca_comch_client *client = nullptr;
  doca_comch_server *server = nullptr;
  doca_comch_connection *connection = nullptr;
  doca_pe *pe = nullptr;
  doca_ctx *ctx = nullptr;
  uint64_t capacity = 0;
  bool failed = false;
  std::deque<Bytes> received;
  std::map<doca_task *, Bytes> sends;
  // 发送完成后释放由通道持有的载荷和 SDK 任务。
  static void sent(doca_comch_task_send *task, doca_data, doca_data context) {
    auto *self = static_cast<Impl *>(context.ptr);
    auto *t = doca_comch_task_send_as_task(task);
    self->sends.erase(t);
    doca_task_free(t);
  }
  // 标记发送失败并走相同的任务释放路径，阻止会话继续使用。
  static void send_error(doca_comch_task_send *task, doca_data data,
                         doca_data context) {
    static_cast<Impl *>(context.ptr)->failed = true;
    sent(task, data, context);
  }
  // 在 SDK 接收缓冲有效期内复制载荷，容量或排队超限时终止会话。
  static void recv(doca_comch_event_msg_recv *, uint8_t *bytes, uint32_t size,
                   doca_comch_connection *connection) {
    auto *self = static_cast<Impl *>(
        doca_comch_connection_get_user_data(connection).ptr);
    if (!self) return;
    try {
      if (size > self->capacity || self->received.size() >= 64) {
        self->failed = true;
        return;
      }
      self->received.emplace_back(bytes, bytes + size);
    } catch (...) {
      self->failed = true;
    }
  }
  // 接受单个服务端连接，并绑定回调所需的通道上下文。
  static void connected(doca_comch_event_connection_status_changed *,
                        doca_comch_connection *connection, uint8_t success) {
    doca_data context{};
    auto *server = doca_comch_server_get_server_ctx(connection);
    if (doca_ctx_get_user_data(doca_comch_server_as_ctx(server), &context) !=
        DOCA_SUCCESS)
      return;
    auto *self = static_cast<Impl *>(context.ptr);
    if (!success || self->connection) {
      self->failed = true;
      return;
    }
    self->connection = connection;
    doca_data data{};
    data.ptr = self;
    if (doca_comch_connection_set_user_data(connection, data) != DOCA_SUCCESS)
      self->failed = true;
  }
  // 记录断连并清除连接句柄，不自动重连或重放查询。
  static void disconnected(doca_comch_event_connection_status_changed *,
                           doca_comch_connection *connection, uint8_t) {
    auto *self = static_cast<Impl *>(
        doca_comch_connection_get_user_data(connection).ptr);
    if (self) {
      self->failed = true;
      self->connection = nullptr;
    }
  }
  ~Impl() {
    if (ctx) stop_context(ctx, pe);
    if (server) doca_comch_server_destroy(server);
    if (client) doca_comch_client_destroy(client);
    if (pe) doca_pe_destroy(pe);
  }
};
// 检查设备消息容量，配置 Comch 与回调，等待单连接建立。
DocaControlChannel::DocaControlChannel(DocaDevice &device, bool server,
                                       const std::string &name,
                                       uint64_t requested)
    : impl_(std::make_unique<Impl>()) {
  auto &s = *impl_;
  uint32_t max_message = 0, max_queue = 0;
  check(doca_comch_cap_get_max_msg_size(doca_dev_as_devinfo(native(device)),
                                        &max_message),
        "Comch message capability");
  check(doca_comch_cap_get_max_recv_queue_size(
            doca_dev_as_devinfo(native(device)), &max_queue),
        "Comch queue capability");
  require(requested <= max_message && requested <= UINT32_MAX && max_queue >= 8,
          "unsupported Comch capacities");
  s.capacity = requested;
  check(doca_pe_create(&s.pe), "Comch progress engine");
  if (server) {
    require(device.representor(), "SoC Comch requires configured representor");
    check(doca_comch_server_create(
              native(device), static_cast<doca_dev_rep *>(device.representor()),
              name.c_str(), &s.server),
          "Comch server create");
    s.ctx = doca_comch_server_as_ctx(s.server);
    check(doca_comch_server_set_max_msg_size(s.server, uint32_t(requested)),
          "Comch message limit");
    check(doca_comch_server_set_recv_queue_size(s.server, 8), "Comch queue");
    check(doca_comch_server_task_send_set_conf(s.server, Impl::sent,
                                               Impl::send_error, 8),
          "Comch send configuration");
    check(doca_comch_server_event_msg_recv_register(s.server, Impl::recv),
          "Comch receive callback");
    check(doca_comch_server_event_connection_status_changed_register(
              s.server, Impl::connected, Impl::disconnected),
          "Comch connection callbacks");
  } else {
    check(doca_comch_client_create(native(device), name.c_str(), &s.client),
          "Comch client create");
    s.ctx = doca_comch_client_as_ctx(s.client);
    check(doca_comch_client_set_max_msg_size(s.client, uint32_t(requested)),
          "Comch message limit");
    check(doca_comch_client_set_recv_queue_size(s.client, 8), "Comch queue");
    check(doca_comch_client_task_send_set_conf(s.client, Impl::sent,
                                               Impl::send_error, 8),
          "Comch send configuration");
    check(doca_comch_client_event_msg_recv_register(s.client, Impl::recv),
          "Comch receive callback");
  }
  doca_data context{};
  context.ptr = &s;
  check(doca_ctx_set_user_data(s.ctx, context), "Comch user data");
  check(doca_pe_connect_ctx(s.pe, s.ctx), "Comch progress connect");
  auto status = doca_ctx_start(s.ctx);
  require(status == DOCA_SUCCESS || status == DOCA_ERROR_IN_PROGRESS,
          "Comch start failed");
  uint64_t start = now_ns();
  while (!s.connection) {
    doca_pe_progress(s.pe);
    require(!s.failed, "Comch connection failed");
    if (!server) {
      doca_ctx_states state;
      check(doca_ctx_get_state(s.ctx, &state), "Comch context state");
      if (state == DOCA_CTX_STATE_RUNNING) {
        check(doca_comch_client_get_connection(s.client, &s.connection),
              "Comch connection");
        check(doca_comch_connection_set_user_data(s.connection, context),
              "Comch connection user data");
      }
    }
    require(now_ns() - start < 60000000000ULL, "Comch connection timed out");
    std::this_thread::yield();
  }
}
DocaControlChannel::~DocaControlChannel() = default;
uint64_t DocaControlChannel::capacity() const { return impl_->capacity; }
// 推进 Comch 完成事件，并将断连或发送错误上报给会话层。
void DocaControlChannel::progress() {
  doca_pe_progress(impl_->pe);
  require(!impl_->failed, "Comch disconnected or send failed");
}
// 保持载荷副本直到发送完成；任务资源不足时返回背压而非成功。
bool DocaControlChannel::send(const Bytes &bytes) {
  auto &s = *impl_;
  require(bytes.size() <= s.capacity, "Comch message too large");
  require(!s.failed && s.connection, "Comch not connected");
  if (s.sends.size() >= 8) return false;
  Bytes owned = bytes;
  doca_comch_task_send *task = nullptr;
  auto status = s.server ? doca_comch_server_task_send_alloc_init(
                               s.server, s.connection, owned.data(),
                               uint32_t(owned.size()), &task)
                         : doca_comch_client_task_send_alloc_init(
                               s.client, s.connection, owned.data(),
                               uint32_t(owned.size()), &task);
  if (status == DOCA_ERROR_NO_MEMORY) return false;
  check(status, "Comch send allocation");
  auto *t = doca_comch_task_send_as_task(task);
  s.sends.emplace(t, std::move(owned));
  status = doca_task_submit(t);
  if (status != DOCA_SUCCESS) {
    s.sends.erase(t);
    doca_task_free(t);
    if (status == DOCA_ERROR_AGAIN) return false;
    check(status, "Comch send submission");
  }
  return true;
}
// 先消费内存导出分段等后端消息，再向运行层返回普通控制消息。
bool DocaControlChannel::receive(Bytes &bytes) {
  auto &s = *impl_;
  while (!s.received.empty()) {
    auto current = std::move(s.received.front());
    s.received.pop_front();
    if (internal_handler &&
        internal_handler(decode_message(current, s.capacity)))
      continue;
    bytes = std::move(current);
    return true;
  }
  return false;
}
struct MmapOwner {
  doca_mmap *mmap = nullptr;
  ~MmapOwner() {
    if (mmap && doca_mmap_destroy(mmap) != DOCA_SUCCESS) std::terminate();
  }
};
struct DocaDmaTransport::Impl {
  struct Remote {
    std::unique_ptr<MmapOwner> owner;
    uintptr_t address = 0;
    uint64_t bytes = 0;
  };
  struct Pending {
    Impl *owner = nullptr;
    ReadRequest request;
    uint64_t token = 0, started = 0;
    std::unique_ptr<MmapOwner> local;
    doca_buf *source = nullptr;
    doca_buf *target = nullptr;
    doca_dma_task_memcpy *task = nullptr;
    ~Pending() {
      if (task) doca_task_free(doca_dma_task_memcpy_as_task(task));
      if (target) doca_buf_dec_refcount(target, nullptr);
      if (source) doca_buf_dec_refcount(source, nullptr);
    }
  };
  doca_dev *device = nullptr;
  doca_dma *dma = nullptr;
  doca_ctx *ctx = nullptr;
  doca_pe *pe = nullptr;
  doca_buf_inventory *inventory = nullptr;
  uint64_t max_bytes = 0, next = 1;
  uint32_t capacity = 0;
  std::map<uint32_t, Remote> regions;
  std::map<uint64_t, std::unique_ptr<Pending>> tasks;
  std::vector<ReadCompletion> completed;
  // 核对 DMA 状态和实际写入长度，记录完成事件后释放任务资源。
  static void done(doca_dma_task_memcpy *task, doca_data data, doca_data) {
    auto *p = static_cast<Pending *>(data.ptr);
    auto *self = p->owner;
    auto status = doca_task_get_status(doca_dma_task_memcpy_as_task(task));
    size_t bytes = 0;
    auto valid = doca_buf_get_data_len(p->target, &bytes);
    bool success = status == DOCA_SUCCESS && valid == DOCA_SUCCESS &&
                   bytes == p->request.bytes;
    self->completed.push_back({p->token, success ? uint64_t(bytes) : 0,
                               p->request.measure ? now_ns() - p->started : 0,
                               success});
    self->tasks.erase(p->token);
  }
  ~Impl() {
    if (ctx) stop_context(ctx, pe);
    if (!tasks.empty()) std::terminate();
    regions.clear();
    if (inventory) doca_buf_inventory_destroy(inventory);
    if (dma) doca_dma_destroy(dma);
    if (pe) doca_pe_destroy(pe);
  }
};
// 核对 DMA 能力，创建 progress engine、任务池和 buffer inventory。
DocaDmaTransport::DocaDmaTransport(DocaDevice &device, uint64_t chunk,
                                   uint32_t tasks)
    : impl_(std::make_unique<Impl>()) {
  auto &s = *impl_;
  s.device = native(device);
  s.capacity = tasks;
  check(doca_dma_cap_task_memcpy_is_supported(doca_dev_as_devinfo(s.device)),
        "DMA device support");
  check(doca_dma_cap_task_memcpy_get_max_buf_size(doca_dev_as_devinfo(s.device),
                                                  &s.max_bytes),
        "DMA maximum size");
  s.max_bytes = std::min(s.max_bytes, chunk);
  require(s.max_bytes > 0 && tasks > 0, "invalid DMA configuration");
  s.completed.reserve(tasks);
  check(doca_pe_create(&s.pe), "DMA progress engine");
  check(doca_dma_create(s.device, &s.dma), "DMA create");
  s.ctx = doca_dma_as_ctx(s.dma);
  check(doca_dma_task_memcpy_set_conf(s.dma, Impl::done, Impl::done, tasks),
        "DMA task configuration");
  check(doca_pe_connect_ctx(s.pe, s.ctx), "DMA progress connect");
  check(doca_buf_inventory_create(uint64_t(tasks) * 2, &s.inventory),
        "DMA buffer inventory");
  check(doca_buf_inventory_start(s.inventory), "DMA inventory start");
  check(doca_ctx_start(s.ctx), "DMA start");
}
DocaDmaTransport::~DocaDmaTransport() = default;
uint64_t DocaDmaTransport::max_bytes() const { return impl_->max_bytes; }
size_t DocaDmaTransport::pending() const { return impl_->tasks.size(); }
// 解析远端注册地址及 SDK 导出描述；只在没有 DMA 在途时替换映射。
// 远端地址仅供 SDK 构造源 buffer，不在 SoC 本地解引用。
void DocaDmaTransport::import_region(uint32_t id, const Bytes &descriptor) {
  auto &s = *impl_;
  require(s.tasks.empty(), "cannot replace mapping while DMA is active");
  Reader r(descriptor);
  uint64_t address = r.u64(), bytes = r.u64();
  require(address > 0 && address <= UINTPTR_MAX && bytes > 0 &&
              bytes <= UINTPTR_MAX - address,
          "invalid imported memory range");
  auto sdk_descriptor = r.bytes(r.remaining());
  require(!sdk_descriptor.empty(), "empty SDK memory descriptor");
  auto owner = std::make_unique<MmapOwner>();
  check(doca_mmap_create_from_export(nullptr, sdk_descriptor.data(),
                                     sdk_descriptor.size(), s.device,
                                     &owner->mmap),
        "import Host memory");
  // The SDK consumes this remote address only; it is never dereferenced on the
  // SoC.
  s.regions[id] = {std::move(owner), static_cast<uintptr_t>(address), bytes};
}
// 检查区域边界并注册本地目标，提交异步 DMA 读取。
// 已接受任务持有映射和 buffer，直到完成回调释放。
Submission DocaDmaTransport::submit(const ReadRequest &request) {
  auto &s = *impl_;
  if (s.tasks.size() >= s.capacity) return {SubmitStatus::WouldBlock, 0};
  auto region = s.regions.find(request.region);
  if (region == s.regions.end()) return {SubmitStatus::Failed, 0};
  try {
    bounds(request.offset, request.bytes, region->second.bytes);
    require(request.bytes > 0 && request.bytes <= s.max_bytes && request.target,
            "invalid DMA read");
    require(request.offset <= UINTPTR_MAX - region->second.address,
            "remote address overflow");
    auto p = std::make_unique<Impl::Pending>();
    p->owner = &s;
    p->request = request;
    p->token = s.next++;
    p->started = request.measure ? now_ns() : 0;
    p->local = std::make_unique<MmapOwner>();
    check(doca_mmap_create(&p->local->mmap), "DMA destination mapping");
    check(doca_mmap_add_dev(p->local->mmap, s.device),
          "DMA destination device");
    check(doca_mmap_set_permissions(p->local->mmap,
                                    DOCA_ACCESS_FLAG_LOCAL_READ_WRITE),
          "DMA destination access");
    check(doca_mmap_set_memrange(p->local->mmap, request.target, request.bytes),
          "DMA destination range");
    check(doca_mmap_start(p->local->mmap), "DMA destination start");
    auto *address =
        reinterpret_cast<void *>(region->second.address + request.offset);
    check(doca_buf_inventory_buf_get_by_data(
              s.inventory, region->second.owner->mmap, address, request.bytes,
              &p->source),
          "DMA source buffer");
    check(doca_buf_inventory_buf_get_by_addr(s.inventory, p->local->mmap,
                                             request.target, request.bytes,
                                             &p->target),
          "DMA destination buffer");
    doca_data data{};
    data.ptr = p.get();
    auto status = doca_dma_task_memcpy_alloc_init(s.dma, p->source, p->target,
                                                  data, &p->task);
    if (status == DOCA_ERROR_NO_MEMORY) return {SubmitStatus::WouldBlock, 0};
    check(status, "DMA allocation");
    uint64_t token = p->token;
    status = doca_task_submit(doca_dma_task_memcpy_as_task(p->task));
    if (status == DOCA_ERROR_AGAIN) return {SubmitStatus::WouldBlock, 0};
    check(status, "DMA submit");
    s.tasks.emplace(token, std::move(p));
    return {SubmitStatus::Accepted, token};
  } catch (const std::exception &) {
    return {SubmitStatus::Failed, 0};
  }
}
// 推进 DMA 并取走完成事件，允许上层统一核对 token 和读取长度。
std::vector<ReadCompletion> DocaDmaTransport::progress() {
  doca_pe_progress(impl_->pe);
  auto result = std::move(impl_->completed);
  impl_->completed.clear();
  return result;
}
struct DocaMemoryExporter::Impl {
  doca_dev *device = nullptr;
  ControlChannel *control = nullptr;
  uint64_t session = 0;
  std::map<uint32_t, std::unique_ptr<MmapOwner>> mappings;
};
// 绑定控制通道及 session，提前确认设备支持 PCI 内存导出。
DocaMemoryExporter::DocaMemoryExporter(DocaDevice &device,
                                       ControlChannel &control,
                                       uint64_t session)
    : impl_(std::make_unique<Impl>()) {
  impl_->device = native(device);
  impl_->control = &control;
  impl_->session = session;
  uint8_t supported = 0;
  check(doca_mmap_cap_is_export_pci_supported(
            doca_dev_as_devinfo(impl_->device), &supported),
        "PCI export support");
  require(supported, "PCI memory export is unsupported");
}
DocaMemoryExporter::~DocaMemoryExporter() = default;
// 注册只读 Host 区域，持有映射并通过有界 INIT_PART 发送导出记录。
void DocaMemoryExporter::bind(uint32_t id, const Region &region) {
  auto &s = *impl_;
  require(!s.mappings.count(id), "memory region already exported");
  auto owner = std::make_unique<MmapOwner>();
  check(doca_mmap_create(&owner->mmap), "Host mapping create");
  check(doca_mmap_add_dev(owner->mmap, s.device), "Host mapping device");
  check(doca_mmap_set_permissions(owner->mmap, DOCA_ACCESS_FLAG_PCI_READ_ONLY),
        "Host mapping permission");
  check(doca_mmap_set_memrange(owner->mmap, const_cast<uint8_t *>(region.data),
                               region.bytes),
        "Host mapping range");
  check(doca_mmap_start(owner->mmap), "Host mapping start");
  const void *descriptor = nullptr;
  size_t bytes = 0;
  check(doca_mmap_export_pci(owner->mmap, s.device, &descriptor, &bytes),
        "Host PCI export");
  require(bytes > 0 && bytes <= (1ULL << 20) - 16,
          "export descriptor too large");
  // Follow DOCA's DMA copy sample: send the registered source address
  // separately.
  Writer export_record;
  export_record.u64(reinterpret_cast<uintptr_t>(region.data));
  export_record.u64(region.bytes);
  export_record.bytes(static_cast<const uint8_t *>(descriptor), bytes);
  s.mappings.emplace(id, std::move(owner));
  require(s.control->capacity() > 128, "export channel too small");
  uint64_t chunk = s.control->capacity() - 128;
  for (uint64_t offset = 0; offset < export_record.data.size();) {
    uint64_t size = std::min(chunk, export_record.data.size() - offset);
    Writer w;
    w.u32(id);
    w.u64(export_record.data.size());
    w.u64(offset);
    w.bytes(export_record.data.data() + offset, size);
    send_message(*s.control, {MessageType::InitPart, s.session, 0, w.data});
    offset += size;
  }
}
// 撤销已结束使用的区域映射；调用方负责先确认远端访问结束。
void DocaMemoryExporter::unbind(uint32_t id) { impl_->mappings.erase(id); }
// 按 session 和连续偏移重组有界导出记录，收齐后才导入 DMA 区域。
bool DocaRegionReceiver::handle(const Message &message) {
  if (message.type != MessageType::InitPart) return false;
  if (session_ == 0) session_ = message.session;
  require(message.session == session_ && session_ > 0 && message.query == 0,
          "invalid export session");
  Reader r(message.payload);
  uint32_t id = r.u32();
  uint64_t total = r.u64(), offset = r.u64();
  require(id >= 1 && id <= 4 && total > 0 && total <= 1ULL << 20,
          "invalid export descriptor bounds");
  auto fragment = r.bytes(r.remaining());
  require(!fragment.empty(), "empty export fragment");
  if (offset == 0) {
    require(assembled_.empty(), "overlapping memory exports");
    current_ = id;
    total_ = total;
  }
  require(id == current_ && total == total_ && offset == assembled_.size(),
          "out-of-order memory export fragment");
  bounds(offset, fragment.size(), total);
  assembled_.insert(assembled_.end(), fragment.begin(), fragment.end());
  if (assembled_.size() == total) {
    dma_.import_region(id, assembled_);
    assembled_.clear();
    current_ = 0;
    total_ = 0;
  }
  return true;
}
}  // namespace anns
