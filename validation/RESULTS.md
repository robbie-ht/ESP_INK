# 固件验证记录

当前B2.0.1（2026-11-21）的实现、离线检查、IDF5.2.2构建及新目录打包结果见
[B2.0.1记录](B2.0.1/RESULTS.md)。本版仍未连接或刷写硬件。

以下保留B2.0.0历史验收，不把旧版尺寸或测试数量当作本版结果。

# B2.0.0 历史验收记录

日期：2026-11-17。对象：Moink-APP；参考：同级 moink-R1.3.4。

## 结果

- **ESP-IDF 5.2.2 / ESP32-C3 编译与链接通过**，生成固件；本轮日志无编译 warning/error。
- 工程版本与 BLE 版本均为 B2.0.0，BLE api=1。源码增加 5.2.2 编译版本检查。
- APP 镜像 **444,768 B**；每个 OTA 槽 1,441,792 B，剩余约 69%。
- 编译期静态 D/IRAM **102,154 B**，31.8%；不是运行期堆实测。
- 4 MiB 合并镜像已生成；5 个二进制文件均有 SHA256。没有执行烧录。
- 参考 R1.3.4 **88 个文件的 SHA256 全部保持不变**。
- 分区对齐、无重叠、终点 0x400000 通过；构建输入 ASCII 检查通过。

## 已通过的离线检查

| 检查 | 覆盖 / 结论 |
|---|---|
| `tests/test_store.sh` | 真实 store.c + 模拟 Flash/NVS；8 满槽、空洞追加、当前槽、重启、实际尺寸与缩略方向、CRC、读写/擦除/NVS失败、缺失分区；ASan/UBSan通过 |
| `tests/test_settings.sh` | 真实 settings.c；默认值/重读、单blob、非法设置不应用、UTF-8/GPIO、写入失败、恢复设置；ASan/UBSan通过 |
| `tests/test_cache.sh` | 真实 cache.c；同图去重、**不同图同CRC16仍重写**、错误头/长度、损坏CRC、断写/重启、缺失分区；ASan/UBSan通过 |
| `tests/test_ble_protocol.py` | 提取生产解析函数在主机编译；图片/缩略图跨分片边界、CRC重传、队列失败、满槽、设置TLV、分页/最小MTU、版本过期、OTA错误、休眠前活动重检 |
| `sdk/test_frame.py` | 原帧编码 27 项全部通过，保留180°与四色契约 |
| `sdk/test_ble.py` | **14 项通过**，模拟 BLE；上传/完成事件、分页/下载、错误处理、设置、有限重传及OTA缺片恢复 |
| `sdk/test_ble.ts` | **13 项通过**，Node 22.23.2 执行实际 TS；同类协议/事件/错误测试 |
| Markdown | README/API/接入文档链接与代码围栏检查通过 |

TypeScript 使用 Node 的 experimental transform 执行测试，**没有声称完成独立 tsc 静态类型检查**。C主机测试与错误注入不证明真实硬件并发、Flash掉电或无线链路。

## 功能与兼容结论

补齐设备8槽图库、双击与定时换图、存图并显示/仅存图、列表/删除/清空、四色与JPEG缩略、整帧/缩略回读。刷屏期间的双击最多暂存一次，空闲后执行。既有传图默认 flags=0，仍只显示/缓存；新图库功能通过新增命令和flags选择。

设置NVS单blob保证字段不混搭；实际断电可能是完整旧值或完整新值。NVS错误后禁止继续修改相关状态，重启重读，不能将模拟失败测试解释为任意断电均恢复旧值。图库删除/清空与设备恢复设置分开；恢复设置保留图库与缓存。

R1.3.4 的 hflip/800×600 诊断档并非现行有效能力；源码没有固件版本防降级策略。WiFi网页功能按BLE产品形态替换。目录没有手机APP界面工程或APK。

## 尚未验证

真实BLE扫描、连接、MTU、传输速度、断连恢复；A0/A1屏显示与刷新时序；GPIO自锁、深睡电流与定时唤醒；Flash物理掉电；OTA首次升级、回滚和真实运行堆余量。

**R/B1迁到B2分区须USB整片迁移，不能BLE OTA迁移分区。** 只有采用B2分区后的后续更新可用APP镜像OTA。

## 重现与产物

- `sh tools/build_idf522.sh`：固定官方5.2.2镜像digest构建。
- `sh tools/test_offline.sh`：全部离线检查。
- 在上述IDF环境执行 `python tools/package_firmware.py`：打包，不访问硬件。
- [构建日志](build-idf522.log) · [测试日志](offline-tests.log) · [构建输入SHA](build-input-sha256.json)
- [固件文件](../firmware/B2.0.0/README.txt) · [哈希清单](../firmware/B2.0.0/SHA256SUMS)
