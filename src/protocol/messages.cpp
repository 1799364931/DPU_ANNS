#include "anns/protocol/messages.hpp"
namespace anns {
// 将整数按固定小端字节序编码，不依赖当前机器的结构布局。
void Writer::u32(uint32_t x) {
  for (int i = 0; i < 4; ++i) data.push_back(uint8_t(x >> (8 * i)));
}
// 按小端字节序编码 64 位长度、偏移或查询标识。
void Writer::u64(uint64_t x) {
  for (int i = 0; i < 8; ++i) data.push_back(uint8_t(x >> (8 * i)));
}
// 保留 FP32 的位表示，通过整数编码统一字节序。
void Writer::f32(float x) {
  uint32_t bits;
  std::memcpy(&bits, &x, 4);
  u32(bits);
}
// 保留双精度配置值的位表示，通过整数编码统一字节序。
void Writer::f64(double x) {
  uint64_t bits;
  std::memcpy(&bits, &x, 8);
  u64(bits);
}
// 编码长度前缀及原始字符串字节，拒绝超过协议上限的长度。
void Writer::string(const std::string &x) {
  require(x.size() <= UINT32_MAX, "string too large");
  u32(uint32_t(x.size()));
  bytes(reinterpret_cast<const uint8_t *>(x.data()), x.size());
}
void Writer::bytes(const uint8_t *x, size_t size) {
  if (size) data.insert(data.end(), x, x + size);
}
// 检查剩余字节后解码小端整数，截断载荷直接报错。
uint32_t Reader::u32() {
  bounds(pos_, 4, size_);
  uint32_t x = 0;
  for (int i = 0; i < 4; ++i) x |= uint32_t(data_[pos_++]) << (8 * i);
  return x;
}
// 检查范围后解码小端 64 位字段。
uint64_t Reader::u64() {
  bounds(pos_, 8, size_);
  uint64_t x = 0;
  for (int i = 0; i < 8; ++i) x |= uint64_t(data_[pos_++]) << (8 * i);
  return x;
}
float Reader::f32() {
  auto bits = u32();
  float x;
  std::memcpy(&x, &bits, 4);
  return x;
}
double Reader::f64() {
  auto bits = u64();
  double x;
  std::memcpy(&x, &bits, 8);
  return x;
}
// 复制指定长度的载荷并推进游标，保证不会读取协议缓冲之外。
Bytes Reader::bytes(size_t size) {
  bounds(pos_, size, size_);
  Bytes result(data_ + pos_, data_ + pos_ + size);
  pos_ += size;
  return result;
}
std::string Reader::string() {
  auto n = u32();
  auto b = bytes(n);
  return std::string(b.begin(), b.end());
}
// 封装版本、消息类型、session/query 标识及载荷长度。
Bytes encode_message(const Message &m) {
  Writer w;
  w.u32(0x4d534e41);
  w.u32(1);
  w.u32(uint32_t(m.type));
  w.u64(m.session);
  w.u64(m.query);
  w.u64(m.payload.size());
  w.bytes(m.payload.data(), m.payload.size());
  return w.data;
}
// 先检查控制通道容量，再校验版本及载荷长度。
Message decode_message(const Bytes &b, uint64_t capacity) {
  require(b.size() <= capacity, "message capacity exceeded");
  Reader r(b);
  require(r.u32() == 0x4d534e41 && r.u32() == 1,
          "unsupported message protocol");
  Message m;
  m.type = MessageType(r.u32());
  m.session = r.u64();
  m.query = r.u64();
  auto len = r.u64();
  require(len == r.remaining(), "message length mismatch");
  m.payload = r.bytes(len);
  return m;
}
static void write_work(Writer &w, const WorkStats &s) {
  w.u64(s.expansions);
  w.u64(s.distances);
  w.u64(s.visited_hits);
  w.u64(s.convergence);
  w.u64(s.prediction_hits);
}
static WorkStats read_work(Reader &r) {
  WorkStats s;
  s.expansions = r.u64();
  s.distances = r.u64();
  s.visited_hits = r.u64();
  s.convergence = r.u64();
  s.prediction_hits = r.u64();
  return s;
}
// 编码紧凑结果和基础统计；完整 trace 不随完成消息传输。
Bytes encode_result(const Result &result) {
  Writer w;
  w.u32(uint32_t(result.termination));
  w.u32(result.safe ? 1 : 0);
  w.string(result.error);
  w.u32(uint32_t(result.ids.size()));
  require(result.ids.size() == result.distances.size(),
          "result shape mismatch");
  for (size_t i = 0; i < result.ids.size(); ++i) {
    w.u32(result.ids[i]);
    w.f32(result.distances[i]);
  }
  const auto &s = result.soc;
  write_work(w, s.work);
  for (auto v :
       {s.d0, s.ds, s.dp, s.start, s.end, s.rerank, s.done, s.graph_hits,
        s.graph_misses, s.first_wait_ns, s.wait_ns, s.rerank_vectors})
    w.u64(v);
  for (const auto &t : s.transfers)
    for (auto v : {t.logical_requests, t.logical_bytes, t.tasks,
                   t.submitted_bytes, t.completed_bytes, t.failures, t.task_ns})
      w.u64(v);
  return w.data;
}
// 校验结果类型、资源安全标志、top-k 及 ID，拒绝不一致的完成状态。
Result decode_result(const Message &m, uint32_t n, uint32_t topk) {
  require(m.type == MessageType::Complete || m.type == MessageType::QueryError,
          "not a result message");
  Reader r(m.payload);
  Result result;
  result.session = m.session;
  result.query_id = m.query;
  result.offloaded = true;
  result.termination = Termination(r.u32());
  require(uint32_t(result.termination) <= uint32_t(Termination::Error),
          "invalid termination");
  result.safe = r.u32() == 1;
  require(result.safe, "unsafe result cannot release resources");
  result.error = r.string();
  uint32_t count = r.u32();
  require(count <= topk, "result topk overflow");
  require((m.type == MessageType::QueryError) ==
              (result.termination == Termination::Error),
          "inconsistent result status");
  for (uint32_t i = 0; i < count; ++i) {
    auto id = r.u32();
    auto distance = r.f32();
    require(id < n && std::isfinite(distance), "invalid result item");
    require(
        std::find(result.ids.begin(), result.ids.end(), id) == result.ids.end(),
        "duplicate result ID");
    result.ids.push_back(id);
    result.distances.push_back(distance);
  }
  auto &s = result.soc;
  s.work = read_work(r);
  uint64_t *fields[] = {&s.d0,           &s.ds,
                        &s.dp,           &s.start,
                        &s.end,          &s.rerank,
                        &s.done,         &s.graph_hits,
                        &s.graph_misses, &s.first_wait_ns,
                        &s.wait_ns,      &s.rerank_vectors};
  for (auto *p : fields) *p = r.u64();
  for (auto &t : s.transfers) {
    uint64_t *values[] = {&t.logical_requests, &t.logical_bytes,   &t.tasks,
                          &t.submitted_bytes,  &t.completed_bytes, &t.failures,
                          &t.task_ns};
    for (auto *p : values) *p = r.u64();
  }
  r.finish();
  return result;
}
// 编码 SoC 常驻映射、簇目录和按图 ID 排列的 PQ 编码。
Bytes encode_soc_assets(const HostIndex &index) {
  Writer w;
  for (auto id : index.meta.graph_to_dataset) w.u32(id);
  for (auto c : index.meta.graph_to_cluster) w.u32(c);
  for (auto offset : index.meta.cluster_offsets) w.u64(offset);
  w.bytes(index.pq_codes.data(), index.pq_codes.size());
  return w.data;
}
// 恢复常驻 PQ 与元数据，完整消费载荷后验证映射和簇布局。
SocIndex decode_soc_assets(const Bytes &bytes, IndexMetadata m) {
  Reader r(bytes);
  SocIndex index;
  m.graph_to_dataset.reserve(m.n);
  m.graph_to_cluster.reserve(m.n);
  m.cluster_offsets.reserve(uint64_t(m.c) + 1);
  for (uint32_t i = 0; i < m.n; ++i) m.graph_to_dataset.push_back(r.u32());
  for (uint32_t i = 0; i < m.n; ++i) m.graph_to_cluster.push_back(r.u32());
  for (uint64_t i = 0; i < uint64_t(m.c) + 1; ++i)
    m.cluster_offsets.push_back(r.u64());
  index.pq_codes = r.bytes(checked_mul(m.n, 32));
  r.finish();
  m.validate();
  index.meta = std::move(m);
  return index;
}
}  // namespace anns
