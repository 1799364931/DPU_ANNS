#include "anns/common/report.hpp"
// 运行使用真实字节复制和完成事件的 Host/SoC 软件闭环。
int main(int argc, char **argv) {
  return anns::run_application(argc, argv, true);
}
