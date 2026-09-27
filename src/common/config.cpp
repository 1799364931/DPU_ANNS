#include "anns/common/config.hpp"
namespace anns {
// 读取 JSON 并接管引用；解析失败时拒绝继续加载配置。
Json::Json(const std::filesystem::path &path)
    : value_(json_object_from_file(path.c_str())) {
  require(value_ != nullptr, "invalid JSON: " + path.string());
}
// 读取必需字段，缺失时报告字段名。
json_object *field(json_object *object, const char *name) {
  json_object *value = nullptr;
  require(json_object_object_get_ex(object, name, &value),
          std::string("missing field: ") + name);
  return value;
}
// 读取非负整数，避免配置中的负数被转换成无符号容量。
uint64_t number(json_object *object, const char *name) {
  auto *value = field(object, name);
  require(json_object_is_type(value, json_type_int),
          std::string("integer required: ") + name);
  require(json_object_get_int64(value) >= 0,
          std::string("negative integer: ") + name);
  return json_object_get_uint64(value);
}
std::string string_value(json_object *object, const char *name) {
  auto *value = field(object, name);
  require(json_object_is_type(value, json_type_string), "string required");
  return json_object_get_string(value);
}
uint64_t optional_number(json_object *o, const char *n, uint64_t fallback) {
  json_object *value = nullptr;
  return json_object_object_get_ex(o, n, &value) ? number(o, n) : fallback;
}
std::string optional_string(json_object *o, const char *n,
                            const std::string &fallback) {
  json_object *value = nullptr;
  return json_object_object_get_ex(o, n, &value) ? string_value(o, n)
                                                 : fallback;
}
// 校验候选容量、全局扩展预算及预测窗口参数。
void SearchConfig::validate() const {
  require(
      ef > 0 && ef <= 1000000 && topk > 0 && topk <= ef && max_expansions > 0,
      "invalid search capacities");
  require(std::isfinite(w) && w > 0 && w <= 1 && std::isfinite(x) && x > 0 &&
              x <= ef && (x <= 1 || std::floor(x) == x),
          "invalid w/x");
}
// 校验传输资源上限及支持的统计模式。
void RuntimeConfig::validate() const {
  require(prefetch_capacity > 0 && state_capacity > 0 && max_message >= 256 &&
              chunk_bytes > 0 && in_flight > 0,
          "invalid runtime capacities");
  require(metrics == "off" || metrics == "basic" || metrics == "trace",
          "invalid metrics mode");
}
// 解析配置并相对配置文件定位路径；三类读取路线分别指定。
AppConfig load_config(const std::filesystem::path &path) {
  Json json(path);
  auto *root = json.get();
  AppConfig c;
  c.manifest = std::filesystem::absolute(path.parent_path() /
                                         string_value(root, "manifest"));
  c.output = std::filesystem::absolute(path.parent_path() /
                                       string_value(root, "output"));
  auto *s = field(root, "search");
  auto *r = field(root, "runtime");
  auto u32 = [](uint64_t v) {
    require(v <= UINT32_MAX, "u32 overflow");
    return static_cast<uint32_t>(v);
  };
  c.search.ef = u32(number(s, "ef"));
  c.search.topk = u32(number(s, "topk"));
  c.search.max_expansions = u32(number(s, "max_expansions"));
  c.search.w = json_object_get_double(field(s, "w"));
  c.search.x = json_object_get_double(field(s, "x"));
  c.search.prediction = optional_number(s, "prediction", 1) != 0;
  c.query_limit = u32(optional_number(root, "query_limit", 100));
  c.warmup = u32(optional_number(root, "warmup", 0));
  c.compare = optional_number(root, "compare", 1) != 0;
  c.runtime.resident_capacity =
      optional_number(r, "resident_capacity", c.runtime.resident_capacity);
  c.runtime.prefetch_capacity =
      optional_number(r, "prefetch_capacity", c.runtime.prefetch_capacity);
  c.runtime.state_capacity =
      optional_number(r, "state_capacity", c.runtime.state_capacity);
  c.runtime.max_message =
      optional_number(r, "max_message", c.runtime.max_message);
  c.runtime.chunk_bytes =
      optional_number(r, "chunk_bytes", c.runtime.chunk_bytes);
  c.runtime.in_flight = optional_number(r, "in_flight", c.runtime.in_flight);
  c.runtime.delay_ticks = optional_number(r, "delay_ticks", 1);
  c.runtime.fail_task = optional_number(r, "fail_task", 0);
  c.runtime.reverse_completion =
      optional_number(r, "reverse_completion", 0) != 0;
  c.runtime.trace_limit = u32(optional_number(r, "trace_limit", 0));
  c.runtime.metrics = optional_string(r, "metrics", "basic");
  auto *routes = field(r, "routes");
  c.runtime.handoff_prefetch = string_value(routes, "handoff_prefetch");
  c.runtime.graph_miss = string_value(routes, "graph_miss");
  c.runtime.rerank_vectors = string_value(routes, "rerank_vectors");
  c.search.validate();
  c.runtime.validate();
  return c;
}
}  // namespace anns
