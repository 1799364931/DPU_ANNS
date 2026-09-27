#include <doca_comch.h>
#include <doca_dev.h>
#include <doca_dma.h>
#include <doca_pe.h>

#include <iostream>
// 链接实际 Comch/DMA API 并枚举设备；不将无设备结果视为硬件验收。
int main() {
  doca_devinfo **devices = nullptr;
  uint32_t count = 0;
  auto status = doca_devinfo_create_list(&devices, &count);
  if (status != DOCA_SUCCESS) {
    std::cerr << doca_error_get_descr(status) << '\n';
    return 1;
  }
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t bytes = 0;
    uint32_t message = 0;
    (void)doca_dma_cap_task_memcpy_get_max_buf_size(devices[i], &bytes);
    (void)doca_comch_cap_get_max_msg_size(devices[i], &message);
  }
  doca_devinfo_destroy_list(devices);
  std::cout << "DOCA enumerated devices: " << count << '\n';
  return 0;
}
