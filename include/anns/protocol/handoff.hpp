// protocol/handoff.hpp：查询交接快照的头部、前导描述和完整状态恢复。
// 交接边界为触发节点扩展之前，保存候选距离、visited 和剩余预算。

#pragma once
#include "anns/data/prefetch.hpp"
#include "anns/protocol/messages.hpp"
namespace anns {
constexpr uint32_t handoff_magic = 0x534e4e41;
constexpr uint64_t handoff_header_bytes = 24;
struct HandoffHeader {
  uint64_t total = 0, prefix = 0;
};
struct HandoffPreamble {
  uint64_t session = 0, query_id = 0;
  std::string identity;
  SearchConfig config;
  uint32_t n = 0, d = 0, candidate_count = 0, expansions = 0, pending_id = 0,
           pending_index = 0;
  uint64_t predicted_nodes = 0;
  PrefetchPlan plan;
};
HandoffHeader decode_handoff_header(const Bytes &header, uint64_t capacity);
Bytes encode_handoff(const QueryState &state, const IndexMetadata &meta,
                     uint64_t session, uint64_t query,
                     uint64_t prefetch_capacity);
HandoffPreamble decode_preamble(const Bytes &bytes, const IndexMetadata &meta,
                                uint64_t session, uint64_t query,
                                uint64_t prefetch_capacity);
QueryState restore_handoff(const Bytes &body, const HandoffPreamble &preamble,
                           const IndexMetadata &meta);
}  // namespace anns
