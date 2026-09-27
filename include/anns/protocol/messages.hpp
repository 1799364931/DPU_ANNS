// protocol/messages.hpp：固定小端协议的字段、控制消息和结果编解码。
// 显式编码整数与浮点位表示，避免传输 STL 布局和本地指针。

#pragma once
#include "anns/search/query_state.hpp"
namespace anns {
class Writer {
 public:
  Bytes data;
  void u32(uint32_t x);
  void u64(uint64_t x);
  void f32(float x);
  void f64(double x);
  void string(const std::string &x);
  void bytes(const uint8_t *data, size_t size);
};
class Reader {
  const uint8_t *data_;
  size_t size_, pos_ = 0;

 public:
  explicit Reader(const Bytes &x) : data_(x.data()), size_(x.size()) {}
  Reader(const uint8_t *data, size_t size) : data_(data), size_(size) {}
  uint32_t u32();
  uint64_t u64();
  float f32();
  double f64();
  std::string string();
  Bytes bytes(size_t size);
  size_t remaining() const { return size_ - pos_; }
  void finish() const { require(remaining() == 0, "trailing protocol bytes"); }
};
enum class MessageType : uint32_t {
  Init = 1,
  Ready = 2,
  Submit = 3,
  Complete = 4,
  QueryError = 5,
  InitError = 6,
  InitPart = 7,
  ClusterRelease = 16,
  ClusterEvicted = 17
};
struct Message {
  MessageType type;
  uint64_t session = 0, query = 0;
  Bytes payload;
};
Bytes encode_message(const Message &message);
Message decode_message(const Bytes &bytes, uint64_t capacity);
Bytes encode_result(const Result &result);
Result decode_result(const Message &message, uint32_t n, uint32_t topk);
Bytes encode_soc_assets(const HostIndex &index);
SocIndex decode_soc_assets(const Bytes &bytes, IndexMetadata meta);
}  // namespace anns
