# MoInk · APP / BLE 固件 B2.0.1

本工程是 **ESP32-C3 上供手机 APP 连接的 BLE 固件**，附 Python / TypeScript 接入 SDK。使用 **ESP-IDF 5.2.2**，以同级 `moink-R1.3.4` 为只读功能参考；不是 Android/iOS 界面工程。

B2.0.0 补齐设备内图库与离线轮播。控制通道仍为 BLE，图像合成留在手机端。

B2.0.1新增物理刷屏期间的图库安全只读窗口：保存/加载完成后开放，刷屏及面板深睡结束后、
任何图库current/NVS/cache写入前关闭并排空在途读取。GET_STATUS尾字段与STATE事件报告
支持位、当前窗口及本窗口前已成功追加标志（flags1/3/7），状态码、
STORE_DONE/DISPLAY_DONE时序、分区和已有NVS均不改变。
所有修改操作仍互斥，READ数据最多496字节，通知属性值最多512字节。客户端也需配合新的
读取调度；该源码修改和离线验证不代表已刷入设备或通过真实BLE与刷屏并发验收。

| 项目 | 当前实现 |
|---|---|
| 芯片 / Flash | ESP32-C3、无 PSRAM、4 MiB Flash |
| 工具链 | 原生 ESP-IDF **5.2.2**；不使用废弃的 platformio.ini |
| 屏幕 | A0 / A1，均为 768×552，黑白黄红 2bpp |
| 图库 | 8 张，每槽 128 KiB；删除不搬移图片，复用空槽后仍按追加顺序排列 |
| 缩略图 | 自动生成 192×138 四色图，或上传不超过 20,000 B 的 JPEG/四色缩略图 |
| 独立操作 | 双击下一张、定时唤醒下一张、长按 20 秒恢复设备设置 |
| 保留功能 | 单帧缓存、BLE OTA 双槽回滚、引脚配置、电池检测、自锁/休眠 |

协议与 SDK：[API.md](docs/API.md)、[APP-INTEGRATION.md](docs/APP-INTEGRATION.md)。

## 与 R1.3.4 的逐项对账

| 功能 | B1.0.0 缺口 | B2.0.0 实现 |
|---|---|---|
| 8 槽设备图库 | 无 | `store.c`，满槽拒绝新增，不覆盖已有图片 |
| 双击切下一张 | 只能重刷缓存 | 从设备图库循环下一张，无需手机连接 |
| 定时唤醒换图 | 只开广播窗口 | `wake_s` + `wake_show`；默认开启换图，可关闭 |
| 存图并显示 / 只存图 | 无 | FRAME_BEGIN flags bit1 / bit2，适用于批量上传 |
| 图库容量与当前图片 | 无 | CAROUSEL_INFO，未显示/已删当前图片使用 255 表示无 |
| 图库列表 | 无 | CAROUSEL_LIST 按 MTU 分页，包含尺寸/CRC/缩略图元数据 |
| 四色 / 彩色缩略图 | 无 | 默认四色缩略；可附 JPEG，手机可回读原字节 |
| 删除 / 清空 | 无 | 异步作业与完成事件，更新元数据，不搬移整图 |
| 整帧与缩略图读取 | 无 | CAROUSEL_READ 按偏移拉取，版本变化拒绝继续；SDK 检查 CRC |
| 单图刷屏、清理残影、设备信息、设置 | 已有 | 保留，修正忙碌保护及设置事务 |
| OTA、自锁、深睡、电池 ADC | 已有 | 保留；新增休眠状态占用，避免刷新/传输期间断电 |
| WiFi 热点、强制门户、网页热更新 | BLE 产品形态已移除 | 手机经 BLE 控制，无需恢复 WiFi |
| 网页裁剪、滤镜、文字、拍立得/便签 UI | 原版运行在浏览器 | 手机界面仍需独立 APP 工程；本目录不包含该工程 |

**实际源码优先于旧 README**：R1.3.4 的 A1 800×600 诊断档已删除，本工程不恢复该旧档位。两种屏都接受 105,984 B 的 768×552 帧，色码和 180° 打包契约保持不变。

## 固件下载

当前源码为 **B2.0.1**，下载入口：[B2.0.1 Release](https://github.com/robbie-ht/ESP_INK/releases/tag/B2.0.1)。设备固件源码和发布文件由本仓库维护；手机APP位于 [ESP-INK-for-Andriod](https://github.com/robbie-ht/ESP-INK-for-Andriod)。刷屏期间读取图库需配合APP 1.0.7或更新的兼容版本。

已有B2分区的设备使用 `moink_B2.0.1_app.bin` 走BLE OTA，无需擦除图库或恢复设置。`moink_B2.0.1_merged.bin` 是4MiB全量镜像，不能当作OTA应用使用；R/B1分区不兼容，需按迁移说明处理。附件包含5个bin、manifest、SHA256SUMS和使用说明；GitHub同时提供此tag的完整源码下载。

本版已通过离线回归、ESP-IDF 5.2.2编译及镜像核验，尚未进行真实BLE/面板并行验收；详见[验证记录](validation/B2.0.1/RESULTS.md)。

## 编译

已有 IDF 5.2.2 环境：

```bash
idf.py -B build-idf522 -D SDKCONFIG=build-idf522/sdkconfig build
```

使用独立输出目录，避免覆盖随工程带来的旧 Windows `build/`。`CMakeLists.txt`、`sdkconfig.defaults`、`partitions.csv` 保持 ASCII，避免中文 Windows 配置工具的编码问题。旧环境说明在 [IDF-5.2.2.md](docs/IDF-5.2.2.md)，其中 B1 体积数字是历史记录。

本轮使用官方 `espressif/idf:v5.2.2` Docker 镜像构建；可用 [tools/build_idf522.sh](tools/build_idf522.sh) 重现，无需改系统 Python 或全局 IDF。

## 首次迁移与分区

**R 系列或 B1.0.0 升到 B2.0.0，必须通过 USB 刷写完整分区表，不能直接走 BLE OTA。** 本次仅生成固件，不连接或烧写设备。以下操作由使用者确认设备串口后手动执行；整片擦除会清除原设置、图库和缓存，应先备份需要保留的图片。

```bash
idf.py -B build-idf522 -p PORT erase-flash
idf.py -B build-idf522 -p PORT flash monitor
```

| 分区 | 起点 | 大小 |
|---|---|---|
| nvs | 0x9000 | 0x4000 |
| otadata | 0xD000 | 0x2000 |
| phy | 0xF000 | 0x1000 |
| ota_0 | 0x10000 | 0x160000（1408 KiB） |
| ota_1 | 0x170000 | 0x160000（1408 KiB） |
| store | 0x2D0000 | 0x100000（8 × 128 KiB） |
| fcache | 0x3D0000 | 0x30000（192 KiB） |

B2 后续在此分区布局内可通过 BLE OTA 更新。图库采用独立 `app_gallery` 命名空间和格式，不读取 R 系列的存储元数据。按键/远程恢复出厂只清 `moink` 设备设置；清图库使用 CAROUSEL_CLEAR，缓存独立保留。

## 手机接入示例

```python
await cli.send_frame(frame, save_gallery=True, store_only=True)
await cli.send_frame(other_frame, save_gallery=True, wait_display=True)
print(await cli.gallery_info())
print(await cli.gallery_list())
await cli.gallery_next()
thumbnail = await cli.gallery_download(0, thumbnail=True)
full_frame = await cli.gallery_download(0)
await cli.gallery_delete(0)
```

新增命令为 `0x22`～`0x27`，原 api=1 既有字段不移动。`GET_SETTINGS` 在设备名之后追加 `wake_show`；旧客户端可忽略尾字段。固件成功接收 FRAME_END 只表示已排队，`STORE_DONE` 才表示存储结果，`DISPLAY_DONE` 才表示显示作业完成。`store_only` 不发送刷屏完成事件。长刷新需要足够长的客户端等待时间。

单击只唤醒，双击切图；上电/重启不自动刷图，只有定时唤醒且 `wake_show=1` 时自动切图。图库为空时不刷屏。定时唤醒保留最长 180 秒空闲连接窗口，执行刷新时不受该空闲计时打断。

## 本轮修复的接入与一致性问题

- TypeScript `GET_SETTINGS` 的设备名起点由误用的 27 修正为 21。
- 设置先校验整组 TLV，再写单个版本化 NVS blob；非法字段不产生部分设置，存储错误后要求重启重新读取状态。
- 设置变更、睡眠、重启、图库任务与传输共用状态保护；定时器不能在接收过程中释放缩略图缓冲。
- 图片 / 缩略图分别校验 CRC，单个 DATA 可跨两者边界；丢包、错误片长和有限次重试有明确处理。
- 最近一帧去重除 CRC16 外还比较实际数据，避免 CRC16 碰撞造成缓存仍是旧图；缓存头也核对几何与长度。
- 空闲计时使用 64 位微秒，避免大秒数换算溢出；自动休眠等待 OTA 确认，新镜像运行 45 秒后确认。

## 验证与硬件边界

本轮B2.0.1 ESP-IDF 5.2.2已编译、打包成功，见[本版记录](validation/B2.0.1/RESULTS.md)及[固件清单](firmware/B2.0.1/manifest.json)；[validation/RESULTS.md](validation/RESULTS.md)保留历史记录。存储、设置、协议与SDK使用主机模拟进行错误注入和离线检查，**不等于真机验收**。

未验证：真实 BLE 连接/吞吐/断连重连，A0/A1 面板、GPIO 自锁、深睡电流与定时唤醒、实际 Flash 掉电、OTA 回滚。第一次上机应依次检查扫描连接、单色帧、图库/按键/定时轮播、断连恢复、OTA，不把旧 R1.3.4 的硬件结果当作新 BLE 版验收。

只分配一张整帧缓冲；附带 JPEG 时额外分配最多 20,000 B。实际运行堆余量需从真机日志确认，编译期静态 RAM 不能证明运行期余量。
