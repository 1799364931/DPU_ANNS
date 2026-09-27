// search/search_core.hpp：两端共享的 PQ 搜索、收敛预测和 FP32 重排。
// 搜索核心不依赖控制通道或 DMA，取数和调度由外层负责。

#pragma once
#include "anns/search/query_state.hpp"
namespace anns {
class PQDistance {
 public:
  static std::vector<float> build_lut(const HostIndex &index,
                                      ArrayView<float> query);
  static float score(uint32_t id, const Bytes &codes,
                     const std::vector<float> &lut);
};
class SearchCore {
 public:
  static QueryState begin(const HostIndex &index, ArrayView<float> query,
                          const SearchConfig &config);
  static bool prepare_next(QueryState &state);
  static bool maybe_trigger(QueryState &state, const IndexMetadata &meta);
  static void expand(QueryState &state, ArrayView<uint32_t> neighbors,
                     const Bytes &codes, const IndexMetadata &meta,
                     QueryWorkspace &workspace, WorkStats &stats);
  static Termination termination(const QueryState &state);
};
class Reranker {
  std::vector<Candidate> exact_;

 public:
  void consume(const Candidate &candidate, ArrayView<float> vector,
               ArrayView<float> query);
  void finish(uint32_t topk, const IndexMetadata &meta, Result &result);
};
}  // namespace anns
