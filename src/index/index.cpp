#include "anns/index/index.hpp"

#include <fstream>
#include <numeric>
namespace anns {
// 仅接受长度完全匹配的二进制资产，读取失败时拒绝返回部分数据。
Bytes read_file(const std::filesystem::path &path, uint64_t expected) {
  require(std::filesystem::file_size(path) == expected,
          "file size mismatch: " + path.string());
  Bytes data(expected);
  std::ifstream input(path, std::ios::binary);
  input.read(reinterpret_cast<char *>(data.data()),
             static_cast<std::streamsize>(data.size()));
  require(bool(input), "file read failed: " + path.string());
  return data;
}
// 按明确的 payload 偏移读取定长数组，先检查文件总长度。
template <class T>
std::vector<T> read_array(const std::filesystem::path &path, uint64_t count,
                          uint64_t offset = 0) {
  auto data = read_file(path, offset + checked_mul(count, sizeof(T)));
  std::vector<T> result(count);
  std::memcpy(result.data(), data.data() + offset, count * sizeof(T));
  return result;
}
// 检查编号映射、簇范围和定长记录布局，确保后续定位不会越界。
void IndexMetadata::validate() const {
  require(n > 0 && d > 0 && d % 32 == 0 && c > 0 && entry < n &&
              stride == checked_mul(uint64_t(rmax) + 1, 4),
          "invalid index shape");
  require(graph_to_dataset.size() == n && graph_to_cluster.size() == n &&
              cluster_offsets.size() == uint64_t(c) + 1,
          "metadata size mismatch");
  std::vector<uint8_t> seen(n, 0);
  for (uint32_t i = 0; i < n; ++i) {
    auto id = graph_to_dataset[i];
    require(id < n && !seen[id], "mapping is not a permutation");
    seen[id] = 1;
  }
  require(cluster_offsets.front() == 0 && cluster_offsets.back() == n,
          "invalid cluster endpoints");
  for (auto offset : cluster_offsets)
    require(offset <= n, "cluster boundary exceeds node count");
  for (uint32_t cluster = 0; cluster < c; ++cluster) {
    require(cluster_offsets[cluster] <= cluster_offsets[cluster + 1],
            "unordered cluster offsets");
    for (uint64_t i = cluster_offsets[cluster];
         i < cluster_offsets[cluster + 1]; ++i)
      require(graph_to_cluster[i] == cluster, "cluster membership mismatch");
  }
}
// 加载 Host 所需资产并验证结构、矩阵头和有限数值。
// 全量 SHA256 与源图等价性由离线验证工具检查。
HostIndex HostIndex::load(const std::filesystem::path &path) {
  static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
                "FP32 required");
  uint16_t endian = 1;
  require(*reinterpret_cast<uint8_t *>(&endian) == 1,
          "little-endian runtime required");
  Json json(path);
  auto *root = json.get();
  HostIndex index;
  auto &m = index.meta;
  require(number(root, "format_version") == 1 &&
              string_value(root, "byte_order") == "little",
          "unsupported manifest");
  auto u32 = [&](const char *name) {
    auto v = number(root, name);
    require(v <= UINT32_MAX, "manifest u32 overflow");
    return uint32_t(v);
  };
  m.n = u32("n");
  m.d = u32("d");
  m.c = u32("c");
  m.rmax = u32("rmax");
  m.entry = u32("entry_graph");
  m.stride = number(root, "stride");
  m.identity = string_value(root, "index_id");
  require(number(root, "vector_payload_offset") == 0 &&
              number(root, "vector_stride") == uint64_t(m.d) * 4,
          "unsupported vector layout");
  auto *files = field(root, "files");
  auto local = [&](const char *name) {
    return path.parent_path() / string_value(field(files, name), "path");
  };
  m.graph_to_dataset = read_array<uint32_t>(local("graph_to_dataset"), m.n);
  m.graph_to_cluster = read_array<uint32_t>(local("graph_to_cluster"), m.n);
  m.cluster_offsets =
      read_array<uint64_t>(local("cluster_offsets"), uint64_t(m.c) + 1);
  m.validate();
  index.graph = read_array<uint32_t>(local("graph"),
                                     checked_mul(m.n, uint64_t(m.rmax) + 1));
  index.pq_codes = read_file(local("pq_codes"), checked_mul(m.n, 32));
  index.centroids =
      read_array<float>(local("pq_centroids"), checked_mul(m.d, 256));
  auto *sources = field(root, "sources");
  auto source = [&](const char *name) {
    return std::filesystem::path(string_value(field(sources, name), "path"));
  };
  index.base = read_array<float>(source("base"), checked_mul(m.n, m.d));
  index.query_count = u32("query_count");
  index.truth_width = u32("truth_width");
  auto check_header = [](const std::filesystem::path &file, uint32_t rows,
                         uint32_t cols) {
    std::ifstream in(file, std::ios::binary);
    uint32_t header[2]{};
    in.read(reinterpret_cast<char *>(header), 8);
    require(bool(in) && header[0] == rows && header[1] == cols,
            "matrix header mismatch");
  };
  check_header(source("queries"), index.query_count, m.d);
  check_header(source("groundtruth"), index.query_count, index.truth_width);
  index.queries = read_array<float>(source("queries"),
                                    checked_mul(index.query_count, m.d), 8);
  index.truth = read_array<uint32_t>(
      source("groundtruth"), checked_mul(index.query_count, index.truth_width),
      8);
  for (uint32_t id = 0; id < m.n; ++id) (void)index.adjacency(id);
  for (float x : index.base) require(std::isfinite(x), "nonfinite base vector");
  for (float x : index.queries) require(std::isfinite(x), "nonfinite query");
  for (float x : index.centroids)
    require(std::isfinite(x), "nonfinite PQ centroid");
  return index;
}
// 检查度数与有效邻居 ID；返回的视图不包含记录尾部 padding。
ArrayView<uint32_t> decode_adjacency(const uint32_t *record, uint32_t rmax,
                                     uint32_t n) {
  require(record[0] <= rmax, "adjacency degree overflow");
  for (uint32_t i = 0; i < record[0]; ++i)
    require(record[i + 1] < n, "neighbor out of range");
  return {record + 1, record[0]};
}
// 使用图 ID 定位定长记录，返回经过边界检查的有效邻居视图。
ArrayView<uint32_t> HostIndex::adjacency(uint32_t id) const {
  require(id < meta.n, "graph ID out of range");
  return decode_adjacency(graph.data() + uint64_t(id) * (meta.rmax + 1),
                          meta.rmax, meta.n);
}
// 将图 ID 映射回数据集 ID，定位 Host 原顺序的全精度向量。
const float *HostIndex::vector(uint32_t id) const {
  require(id < meta.n, "vector graph ID out of range");
  return base.data() + uint64_t(meta.graph_to_dataset[id]) * meta.d;
}
}  // namespace anns
