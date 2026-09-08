# PhotoPull 使用与构建

修订方案保存在 [implementation-plan.md](implementation-plan.md)。软件与硬件的验收状态见 [validation.md](validation.md)。

## SD 配置

将 [photopull.json.example](photopull.json.example) 按实际网络和 HTTPS 静态服务修改，保存到 SD 根目录 `photopull.json`，重启后生效。公共 CA 使用固件证书 bundle；私有 CA 需在 `server` 内指定 `ca_file`，例如 `/sdcard/photopull-ca.pem`，并将 PEM 文件放在对应位置。指定 CA 缺失或解析失败时停止同步，不会降级证书校验。

省略 Wi-Fi 或 SSID 为空时使用原 NVS 凭据；SD 中的凭据只在本次运行生效。初次没有业务模式记录时，有效启用配置选择 Network，否则选择 Basic；之后保留业务模式选择。Lite 遇到旧小智模式时一次性迁移为 Network。

关闭远程同步可以使用最小配置：

```json
{"schema":1,"enabled":false}
```

在 Network 中，关闭同步仍会按已经提交的本地清单播放。图库位于 `/sdcard/07_server_photos/`，由 SHA 文件与两个带校验和的快照组成；不要手工编辑快照。配置、CA 和图库不会成为 Web 静态下载入口。

Network 保持常驻。单击 BOOT 进入本次运行保持开启的维护热点；双击显示电池状态；原模式选择按键仍保留。STA 失败会开启维护热点并后台重连。维护热点沿用原名称 `esp_network` 和密码 `1234567890`，上传用于可信局域网。

`/dataUP` 保留一字节模式值加规范化 BMP 的请求格式；HTTP 200 表示文件保存成功、显示请求已接受。显示期间上传或文件读取会背压等待；忙或校验失败返回错误，不覆盖已接受的显示请求。BMP 必须满足方案中的精确尺寸、长度和六色约束。

## 构建

本次安装在工作目录中的环境不修改用户 shell 配置：

```sh
export IDF_TOOLS_PATH=/home/PhotoPainter/toolchains/tools
export XDG_CACHE_HOME=/home/PhotoPainter/toolchains/cache
. /home/PhotoPainter/toolchains/esp-idf/export.sh
cd /home/PhotoPainter/ESP32-S3-PhotoPainter/01_Example/xiaozhi-esp32
idf.py -B build-lite build
```

默认关闭 `CONFIG_PHOTOPAINTER_ENABLE_XIAOZHI`。Full 使用独立 `SDKCONFIG` 和构建目录，并启用该选项；验证记录给出本次执行方式和实际尺寸。可先复制 `sdkconfig` 到 `build-full/sdkconfig`，再将该选项改为 `y`；两个构建依次执行。分区表未修改，`03_Firmware` 发布镜像未被覆盖。构建不包含烧录。

## 主机测试

```sh
cmake -S components/photopull/tests -B build-host-tests \
  -DMBEDTLS_SOURCE_DIR=/home/PhotoPainter/toolchains/esp-idf/components/mbedtls/mbedtls \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-host-tests -j 4
ctest --test-dir build-host-tests --output-on-failure
```

主机测试默认启用地址和未定义行为检查；真实证书测试使用同版本 mbedTLS 和临时生成的证书。存储测试模拟文件操作前后的中断并在恢复后检查图片集合；真实 SD 控制器掉电、BUSY/SPI 故障、实际电源行为与 24 小时常驻仍需单独的硬件测试。
