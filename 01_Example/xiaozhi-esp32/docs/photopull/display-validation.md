# PhotoPull 显示与图片链路验证记录

日期：2026-09-07（Asia/Shanghai）

本记录覆盖 `display_bsp` 的按需初始化、有限等待、SPI 错误返回、失败断电和统一 `EPD_ShowBmp`/`EPD_ShowClear` 事务，以及 BMP/JPEG/PNG 公共解码入口的边界检查。显示类继续使用内部互斥；PhotoPull 的外层 `epaper_gui_semapHandle` 是独立的调用方互斥，事务入口不会再次获取该外层锁。

已收敛的行为：

- 构造阶段把显示缓存初始化为白色；`EPD_InitLocked` 不再清空已经准备好的图像。
- 初始化、上电、刷新和断电等待分别有 10 秒、10 秒、120 秒和 10 秒上限。
- `POWER_ON` 前先记录可能已上电的状态；上电、刷新或断电阶段失败均返回错误并尝试有界 `POWER_OFF`。无法确认断电时保留故障状态并要求重新初始化。
- `EPD_ShowBmp` 与 `EPD_ShowClear` 在同一内部互斥事务中准备缓存并刷新，返回 `esp_err_t`；刷新未成功不会被上层视为显示成功。
- BMP 显示入口检查文件头、尺寸、偏移、行跨度、文件长度和短读；解码器限制输入尺寸、像素数和输出长度，缩放/抖动内存失败会向显示事务传播。
- 主机 BMP 校验覆盖分块短读、非法像素、偏移、保留字段、图像长度、回调超报和失败时清空输出信息。

主机验证命令（均在 `01_Example/xiaozhi-esp32` 执行）：

```text
g++ -std=c++17 -Wall -Wextra -Werror -Icomponents/photopull \
  components/photopull/bmp_validator.cpp \
  components/photopull/tests/bmp_validator_host_test.cpp \
  -o /tmp/photopull-tests/bmp_validator_host_test
/tmp/photopull-tests/bmp_validator_host_test

g++ -std=c++11 -Wall -Wextra -Werror -Icomponents/photopull \
  components/photopull/core.cpp components/photopull/store.cpp \
  components/photopull/tests/store_fault_test.cpp \
  -o /tmp/photopull-tests/store_fault_test
/tmp/photopull-tests/store_fault_test

g++ -std=c++17 -Wall -Wextra -Werror -Icomponents/photopull \
  components/photopull/carousel.cpp components/photopull/tests/carousel_test.cpp \
  -o /tmp/photopull-tests/carousel_test
/tmp/photopull-tests/carousel_test


```

结果：BMP、存储和轮播三个生产逻辑主机测试均返回 0；轮播测试输出 `carousel scenarios passed`。显示驱动阶段错误传播目前经过代码审查，尚未用实际驱动配合硬件执行故障注入。

`test_decoder.c` 已用 ESP JPEG 头文件做主机侧语法检查：

```text
gcc -std=c11 -Wall -Wextra -Werror -fsyntax-only \
  -Icomponents/app_bsp/jpg_src \
  -Imanaged_components/espressif__esp_new_jpeg/include \
  components/app_bsp/jpg_src/test_decoder.c
```

该检查通过。固件构建结果统一记录在 `validation.md`。未进行刷写、串口启动或实际墨水屏电源行为验证；BUSY、SPI 故障、实际 `POWER_OFF` 和屏幕残影仍须硬件验收。
