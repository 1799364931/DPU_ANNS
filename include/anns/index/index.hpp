// index/index.hpp：索引元数据与 Host、SoC 数据归属。
// 搜索使用图 ID；原始向量访问通过映射转换为数据集 ID。

#pragma once
#include "anns/common/config.hpp"
namespace anns {
struct IndexMetadata {
  uint32_t n = 0, d = 0, c = 0, rmax = 0, entry = 0;
  uint64_t stride = 0;
  std::string identity;
  std::vector<uint32_t> graph_to_dataset, graph_to_cluster;
  std::vector<uint64_t> cluster_offsets;
  void validate() const;
};
struct SocIndex {
  IndexMetadata meta;
  Bytes pq_codes;
};
struct HostIndex {
  IndexMetadata meta;
  std::vector<uint32_t> graph;
  Bytes pq_codes;
  std::vector<float> centroids, base, queries;
  std::vector<uint32_t> truth;
  uint32_t query_count = 0, truth_width = 0;
  static HostIndex load(const std::filesystem::path &manifest);
  ArrayView<uint32_t> adjacency(uint32_t id) const;
  const float *vector(uint32_t graph_id) const;
};
Bytes read_file(const std::filesystem::path &path, uint64_t expected);
ArrayView<uint32_t> decode_adjacency(const uint32_t *record, uint32_t rmax,
                                     uint32_t n);
}  // namespace anns
