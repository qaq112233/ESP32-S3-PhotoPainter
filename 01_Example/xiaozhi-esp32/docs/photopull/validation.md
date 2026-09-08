# PhotoPull 验证记录

验证日期：2026-09-07 至 2026-09-08。范围为软件实现、主机测试与 ESP-IDF 构建；未连接或操作开发板。

## 环境和复现

ESP-IDF 固定为 v5.5.1，安装在 `/home/PhotoPainter/toolchains/esp-idf`；工具与 Python 环境位于 `/home/PhotoPainter/toolchains/tools`。环境变量与主机测试命令见 [README.md](README.md)。工程目标保持 ESP32-S3、16 MB Flash、Octal PSRAM 80 MHz 和原 `partitions/v2/16m.csv`。没有改动用户 shell 启动文件或 `03_Firmware`。

Lite 使用工程根目录的 `sdkconfig` 和 `build-lite`；Full 使用 `build-full/sdkconfig`，复制 Lite 配置后启用 `CONFIG_PHOTOPAINTER_ENABLE_XIAOZHI=y`：

```sh
idf.py -B build-full -D SDKCONFIG="$PWD/build-full/sdkconfig" build
```

两种构建共享组件管理器下载目录，依次执行。完整构建日志保存在忽略提交的 `validation-logs/` 中。首次切换依赖集合可能重新解析和下载组件。构建不执行烧录。

## 主机测试

统一 CTest 包含 10 组测试；除真实证书使用的固定 mbedTLS 库外，生产逻辑测试启用 AddressSanitizer 和 UndefinedBehaviorSanitizer。

| 测试 | 直接验证的代码与范围 |
| --- | --- |
| carousel_test | 生产轮播状态机：ID 游标、SHA 相同跳过、更新、手工显示、空清单、间隔变化、独立失败退避 |
| business_mode_test | 生产模式决策：首次自动、保存 Basic、Lite 旧模式迁移、Full 保留、无 SD 临时维护模式 |
| bmp_validator_test | 生产六色 BMP 流校验：头、尺寸、像素、短读和分块边界 |
| store_fault_test | 生产协议解析与双快照存储、版本限制、ETag、损坏恢复 |
| storage_recovery_test | 文件操作前后故障注入、恢复照片集合、50 ID/3 SHA、镜像再校验、旧完整库回退且保留新版本修复依据、配置容量、命名空间隔离 |
| network_protocol_test | 生产上传分帧和静态文件白名单；不是 ESP32 Wi-Fi 驱动测试 |
| basic_timer_test | 生产 Basic 定时解析：整数下限/上限、小数、非有限数、非法类型、损坏 JSON 和默认值 |
| manager_integration_test | 生产管理循环：200/304 后清理、空间检查前回收、30 秒重试、候选图片复用、活动写入保护；生产显示缓冲验证横竖图后的电池页面一致 |
| wifi_maintenance_test | 生产 Wi-Fi 回调与维护任务：合并事件按最新状态处理，覆盖断开/获得 IP 两种顺序 |
| tls_identity_test | 固定版本 mbedTLS 解析真实证书，验证 IPv4/IPv6 IP SAN，拒绝错误 IP、仅 CN 和数字 DNS SAN |

管理循环和 Wi-Fi 集成测试以主机桩替代 RTOS、网络、GPIO/SPI 等边界；它们验证生产状态机和显示缓冲，不模拟真实任务竞争或硬件电源。

证书测试不等同于设备上的完整 HTTPS 握手测试。存储故障测试使用内存文件系统和图片校验回调，不能代替真实 FatFS/SD 控制器掉电测试。详细场景见 [storage-validation.md](storage-validation.md)、[network-validation.md](network-validation.md)、[display-validation.md](display-validation.md)。

## 构建结果

最终源码的两种构建均通过，10/10 主机测试通过（18.60 秒），`git diff --check` 通过。

| 构建 | 应用镜像 | OTA 槽剩余 | 结果 |
| --- | --- | --- | --- |
| Lite | 1,296,528 字节 | 2,832,240 字节 | 通过 |
| Full | 4,050,912 字节 | 77,856 字节 | 通过 |

Lite 低于 3,604,480 字节门槛，保留超过 512 KiB 的分区空间。Full 仍能放入原分区，但剩余约 76 KiB，构建器提示接近分区上限；这不影响 Lite 的验收门槛。两种配置都已核对外部内存启用、Octal 模式和 80 MHz。

Lite 实际组件图中没有 codec、ESP-SR、LVGL、MQTT、摄像头和 Opus 组件，链接符号也不含小智 Application、CodecPort 或 MQTT 初始化入口。Basic 图片解码、EPD 状态字体、Wi-Fi/AP 上传和 PhotoPull 仍在 Lite 中。项目上下文检查通过；在 Lite 依赖图中，检查器对未下载的 Full 专属组件给出提示属于裁剪预期。

最近日志：`validation-logs/review-fixes-lite-build.log`、`validation-logs/review-fixes-full-build.log`、`validation-logs/review-fixes-host-results.log`。构建产物位于 `build-lite/` 与 `build-full/`，没有复制到发布目录。

## 2026-09-08 审查修复

- 为双槽完整一致但清理未完成的状态补充重试。重启后先等待有效 200/304 确认候选引用，再清理旧残留；保留候选 SHA 文件、拒绝用非法清单替换保留集合，并在空间预检前尝试回收。失败后在空闲时每隔至少 30 秒重试，活动图片写入期间不清理。
- 运行期联网状态仅由 Wi-Fi 回调更新；维护任务读取最新状态，避免无序事件位把“刚恢复连接”覆盖成断开。
- 清屏后恢复固定横版画布；照片加载再设置照片自身方向，使电池/文字页面不继承前一张竖版照片的行宽。
- Basic 的 timer 仅接受 1～UINT32_MAX 的整数秒数；小数、非有限数、非法字段或损坏 JSON 回退到原有 780 秒默认值。

## 尚未执行的硬件验收

以下项目需要独立的上板操作授权和实际设备；本次没有烧录、擦除、串口复位或替换发布镜像。

1. Basic 的 JPG/PNG/BMP 与原 timer 配置、模式按键、电池显示；Network 的 AP/STA 上传、无 SD 内置维护页以及 OTA pending image 确认。
2. 正确/错误 SD 与 NVS 凭据、20 秒 STA 回退、AP 客户端存在时保持热点、主动维护热点保持；TLS 私有 CA/公共 CA、过期证书、IP SAN、错误时钟、重定向拒绝和慢 DNS/慢响应预算。
3. 首次 50 张真实图片同步、增删改排序、同版冲突、失败 ETag、304 时本地损坏、来源切换、缓存离线轮播和 enabled=false；确认现场日志没有密码或 token。
4. 下载、各槽写入/同步、镜像和清理阶段抽样断电；每次重启检查完整照片集合。覆盖 SD 满、短写、拔卡及文件系统仍可挂载时的恢复。
5. 慢网跨越轮播截止时间、连续上传与电池显示交错；无正在刷新时维护请求不会等待整批图库。测量内部堆、PSRAM、最大连续块、栈高水位和队列占用。
6. 注入 BUSY 卡住和 SPI 故障，确认有界退出与退避；实际测量屏幕 POWER_OFF 和异常断电路径。发送断电命令的日志不能证明硬件已断电。
7. 24 小时 Network 常驻，确认无崩溃、栈溢出、持续堆下降或队列增长。固件每分钟记录所需资源指标，尚无实机运行记录。

## 实现约束

HTTPS transport 与证书 bundle 扩展依据 ESP-IDF v5.5.1 的具体接口实现，特别是 mbedTLS 校验回调组合；升级 IDF 时需要重新复核。文件同步保证限定为可挂载文件系统且已同步内容未发生介质级损坏，双快照不承诺修复任意 SD 介质损坏。
