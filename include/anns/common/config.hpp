// common/config.hpp：JSON 配置读取与搜索、运行参数校验。
// 分别配置三类读取路线、资源容量和统计模式。

#pragma once
#include <json-c/json.h>

#include <filesystem>

#include "anns/common/types.hpp"

namespace anns {
class Json {
  json_object *value_ = nullptr;

 public:
  explicit Json(const std::filesystem::path &path);
  ~Json() {
    if (value_) json_object_put(value_);
  }
  Json(const Json &) = delete;
  Json &operator=(const Json &) = delete;
  json_object *get() const { return value_; }
};
json_object *field(json_object *object, const char *name);
uint64_t number(json_object *object, const char *name);
std::string string_value(json_object *object, const char *name);
uint64_t optional_number(json_object *object, const char *name,
                         uint64_t fallback);
std::string optional_string(json_object *object, const char *name,
                            const std::string &fallback);

struct SearchConfig {
  uint32_t ef = 64, topk = 10, max_expansions = 10000;
  double w = 0.75, x = 0.25;
  bool prediction = true;
  // 将比例阈值转换为从 1 开始的触发 rank。
  uint32_t w_rank() const { return static_cast<uint32_t>(std::ceil(ef * w)); }
  // 预测窗口支持比例或整数个数；比例按 ef 向上取整。
  uint32_t x_count() const {
    return x <= 1 ? static_cast<uint32_t>(std::ceil(ef * x))
                  : static_cast<uint32_t>(x);
  }
  void validate() const;
};
struct RuntimeConfig {
  uint64_t prefetch_capacity = 64ULL << 20, state_capacity = 8ULL << 20,
           max_message = 4096;
  uint64_t resident_capacity = 512ULL << 20;
  uint64_t chunk_bytes = 65536, in_flight = 8, delay_ticks = 1, fail_task = 0;
  bool reverse_completion = false;
  uint32_t trace_limit = 0;
  std::string metrics = "basic", handoff_prefetch = "simulated",
              graph_miss = "simulated", rerank_vectors = "simulated";
  void validate() const;
};
struct AppConfig {
  std::filesystem::path manifest, output;
  SearchConfig search;
  RuntimeConfig runtime;
  uint32_t query_limit = 100, warmup = 0;
  bool compare = true;
};
AppConfig load_config(const std::filesystem::path &path);
}  // namespace anns
