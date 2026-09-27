#include "anns/common/report.hpp"
// 运行完整 Host 基线；参数解析和报告由共享软件入口处理。
int main(int argc, char **argv) {
  return anns::run_application(argc, argv, false);
}
