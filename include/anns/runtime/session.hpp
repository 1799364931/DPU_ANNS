// runtime/session.hpp：会话初始化和查询提交的数据描述。
// 声明区域长度及布局身份，查询提交仅使用区域 ID、偏移和长度。

#pragma once
#include "anns/protocol/handoff.hpp"
#include "anns/transport/control_channel.hpp"
namespace anns {
struct SessionInit {
  IndexMetadata meta;
  uint64_t asset_bytes = 0, graph_bytes = 0, vector_bytes = 0;
};
Bytes encode_init(const HostIndex &index, uint64_t asset_bytes);
SessionInit decode_init(const Bytes &bytes);
struct SubmitDescriptor {
  uint32_t region = 4;
  uint64_t offset = 0, bytes = 0;
};
Bytes encode_submit(const SubmitDescriptor &desc);
SubmitDescriptor decode_submit(const Bytes &bytes);
}  // namespace anns
