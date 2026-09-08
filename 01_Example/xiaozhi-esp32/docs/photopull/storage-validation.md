# PhotoPull 存储验证记录

这份记录对应 `components/photopull/core.cpp`、`store.cpp` 以及主机端
`tests/store_fault_test.cpp` 和 `tests/storage_recovery_test.cpp`。测试不需要
ESP-IDF 或 SD 卡，使用内存文件系统模拟写入、同步、改名、删除和重启。

## 执行方式

```sh
g++ -std=c++11 -O2 -Wall -Wextra -Werror \
  -Icomponents/photopull \
  components/photopull/core.cpp components/photopull/store.cpp \
  components/photopull/tests/store_fault_test.cpp \
  -o /tmp/photopull-store-fault-test
/tmp/photopull-store-fault-test

g++ -std=c++11 -O2 -Wall -Wextra -Werror \
  -Icomponents/photopull \
  components/photopull/core.cpp components/photopull/store.cpp \
  components/photopull/tests/storage_recovery_test.cpp \
  -o /tmp/photopull-storage-recovery-test
/tmp/photopull-storage-recovery-test
```

两组测试均在 2026-09-07 的 Linux 主机上通过。编译使用系统 g++，不代表
ESP-IDF 构建或真实 SD 介质已经验证。

## 覆盖范围

- SHA-256 交叉向量：1,152,054 字节的全 0、全 1 和全 `0xff` 数据，摘要
  与独立实现一致；图片流按 4 KiB 块写入并再次从文件流校验。
- 清单最多 50 项、顺序和空清单；50 个有序 ID 复用 3 个 SHA，验证内容
  寻址、重复 SHA 只计算一次缺失空间以及重启后 50 项完整性掩码。
- 配置字段边界：8 KiB 文档、512 字节 HTTPS origin、256 字节路径、
  2,048 字节 token、32/64 字节 Wi-Fi 字段，以及轮询/显示间隔的
  60～86,400 秒边界。
- 来源键会折叠主机名大小写并去掉显式 `:443`；HTTP、路径穿越和百分号
  编码被拒绝。快照只保存规范化来源。
- 严格 BMP 回调在 `.part` 发布前和重启恢复时执行；回调拒绝时不会出现
  SHA 命名的最终文件。
- 失败候选的 ETag 不会覆盖旧快照。两槽一致后才 GC；`.part`、旧 SHA
  文件和目录内候选文件可在后续安全时机清理，未知文件和目录外同名文件
  保留。
- 对写入、`SyncFile`、改名、删除和目录同步分别注入操作前/操作后故障，
  每次都新建 Store 执行 Recover，并检查至少一套完整照片集合仍可播放。
- `defer_mirror=true` 将第一槽写入作为逻辑提交点。RepairMirror 在镜像
  前重新验证当前快照的全部图片；缺损时返回 `kNoCompleteSnapshot`，不把
  缺损集合降级成空清单。

掉电矩阵只验证文件系统操作的可观察语义。它不覆盖 SD 控制器掉电时序、
介质损坏、ESP-IDF FatFS 驱动实现或硬件显示刷新。
