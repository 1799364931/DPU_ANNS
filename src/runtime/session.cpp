#include "anns/runtime/session.hpp"
namespace anns {
// 编码索引身份、布局和导出区域长度，作为 SoC 初始化读取的依据。
Bytes encode_init(const HostIndex &index, uint64_t asset_bytes) {
  Writer w;
  const auto &m = index.meta;
  w.string(m.identity);
  for (auto x : {m.n, m.d, m.c, m.rmax, m.entry}) w.u32(x);
  w.u64(m.stride);
  w.u64(asset_bytes);
  w.u64(checked_mul(m.n, m.stride));
  w.u64(checked_mul(checked_mul(m.n, m.d), 4));
  return w.data;
}
// 校验会话布局和各区域长度，不允许按错误数据形状发起读取。
SessionInit decode_init(const Bytes &bytes) {
  Reader r(bytes);
  SessionInit s;
  auto &m = s.meta;
  m.identity = r.string();
  m.n = r.u32();
  m.d = r.u32();
  m.c = r.u32();
  m.rmax = r.u32();
  m.entry = r.u32();
  m.stride = r.u64();
  s.asset_bytes = r.u64();
  s.graph_bytes = r.u64();
  s.vector_bytes = r.u64();
  r.finish();
  require(m.n > 0 && m.d > 0 && m.d % 32 == 0 && m.c > 0 && m.entry < m.n &&
              m.stride == checked_mul(uint64_t(m.rmax) + 1, 4),
          "invalid session layout");
  require(s.asset_bytes ==
                  checked_mul(m.n, 40) + checked_mul(uint64_t(m.c) + 1, 8) &&
              s.graph_bytes == checked_mul(m.n, m.stride) &&
              s.vector_bytes == checked_mul(checked_mul(m.n, m.d), 4),
          "invalid session region lengths");
  return s;
}
// 编码交接区的逻辑区域 ID、偏移和长度，不传搜索对象指针。
Bytes encode_submit(const SubmitDescriptor &d) {
  Writer w;
  w.u32(d.region);
  w.u64(d.offset);
  w.u64(d.bytes);
  return w.data;
}
// 完整消费提交载荷并核对 v1 使用的交接区域。
SubmitDescriptor decode_submit(const Bytes &bytes) {
  Reader r(bytes);
  SubmitDescriptor d;
  d.region = r.u32();
  d.offset = r.u64();
  d.bytes = r.u64();
  r.finish();
  require(d.region == 4, "unexpected handoff region");
  return d;
}
}  // namespace anns
