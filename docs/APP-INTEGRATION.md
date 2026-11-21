# APP 接入指南

目标读者：要做 Android / iOS / uni-app / React Native 客户端的同学。
协议字节级定义在 [API.md](API.md)。当前为 ESP-IDF 5.2.2 的 B2.0.1 设备固件和参考 SDK；
本仓库没有完整 Android/iOS 手机界面工程。本文说明如何把这些能力接入手机客户端。

---

## 1. 一次完整的刷图流程

```
扫描(namePrefix = "MoInk")        ← 只认扫描响应里的名字，必须 active scan
  └→ 连接（不配对、不 bonding）
       └→ 服务发现 6D6F496E-4B00-4000-8000-000000000000
            └→ 订阅 EVT（Notify）
                 └→ PING / GET_STATUS（确认 api 版本 + 电量 + 状态）
                      └→ [本地渲染 768×552] → 量化成 4 色 2bit → pack2bpp
                           └→ FRAME_BEGIN → DATA ×N → FRAME_END
                                └→ 等 EVT DISPLAY_DONE（1 = 失败，0 = 成功）
                                     └→ 可选 SLEEP_NOW 立刻断开省电
```

关键点：**FRAME_END 的 RSP 只代表「收完并排队刷屏」，不代表刷完**。
真正的完成信号是 `EVT 0x01 DISPLAY_DONE`。A1 驱动包含长等待和重试，
如果用户在刷屏中关掉 APP，设备照样会刷完（固件侧不受影响），下次连上
`GET_STATUS.state` 可查询当时状态。不要在刷屏后才注册完成监听：事件可能比 RSP 更早到。
SDK 默认 send_frame/sendFrame 在排队后返回；开启 wait_display/waitDisplay 等刷完。
上传保存和显示等待默认预留 900 秒（含面板重试），删除/清空默认 60 秒，可以传入更适合产品的超时。

## 2. 发现与连接

| 项 | 值 |
|---|---|
| 广播名 | 设置里的 `name`，默认 `MoInk-XXXX`（XXXX = MAC 后两字节） |
| 广播内容 | `flags` + 128-bit 服务 UUID |
| 扫描响应 | 完整设备名 |
| 服务 UUID | `6D6F496E-4B00-4000-8000-000000000000` |
| CTRL | `...0001` Write / Write Without Response |
| DATA | `...0002` Write Without Response |
| EVT | `...0003` Notify |
| 配对 | **不需要**，无加密无绑定，连上就能写 |
| 并发 | 只允许 1 条连接；已连接时停止广播 |

定时唤醒等待连接的窗口最多 **180 秒**；按键唤醒按配置的 `sleep_s` 计时。
超时根据 `timeout_action` 深睡或关机，`sleep_s=0` 关闭普通空闲超时。
APP 扫描不到时提示用户「按一下设备按键」。

## 3. MTU 与分片

`chunk = min(MTU - 5, 512)`。`FRAME_BEGIN` 里带上分片长度，固件回
`[chunk_used, chunk_max]`，**以固件回的为准**（它可能因为链路状态给得更小）。

| 协商 MTU（示例） | chunk | 105984 B 整帧分片数 |
|---|---|---|
| 517 | 512 | 207 |
| 185 | 180 | 589 |
| 23 | 18 | 5888 |

以上是字节数计算，并非本版本真机耗时。Web Bluetooth 不暴露 MTU，SDK 发建议 chunk，
由 FRAME_BEGIN 响应给出可用值。GET_STATUS/SETTINGS 装不下时返回 UNSUPPORTED；
图库非空 LIST 至少要求 MTU 34，实际接入建议协商 100 或更大。

**Android 必须显式 `requestMtu(517)`**，否则吞吐慢十倍。iOS 无法设置 MTU，
只能靠 `canSendWriteWithoutResponse` 做流控。

### Android 写入节流

传输层维护串行写入队列；依据对应 Android API 和插件的写入结果/回调推进，
调用失败要保留当前数据包重试。不要把一次 API 调用成功当成设备已收到完整图像。
协议层的 TRANSFER_GAP / INCOMPLETE 能提示回退位置；SDK 限制重试次数，失败后 ABORT。

### iOS 流控（必做）

iOS 的 `writeValue(_:for:type:.withoutResponse)` 在发送队列满时数据会被丢掉，
必须等 `peripheralIsReady(toSendWriteWithoutResponse:)`：

```swift
func pump() {
    while !queue.isEmpty && peripheral.canSendWriteWithoutResponse {
        let pkt = queue.removeFirst()
        evtPump(pkt)
    }
}
func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) { pump() }
```

## 4. 渲染与打包（最容易错的一步）

### 4.1 图像规格

- 768 × 552，4 色：黑(0) / 白(1) / 黄(2) / 红(3)
- 2bit/像素，4 像素/字节，MSB 优先
- 载荷固定 105984 字节

### 4.2 色彩与抖动

屏只有 4 色，照片必须**量化 + 抖动**。推荐：

1. 保留 RGB 彩色输入，先缩放/裁剪；灰度模式只用于用户主动选择的黑白图
2. 4 色最近邻量化（Lab 空间比 RGB 准）
3. 用 Floyd–Steinberg 或有序抖动（Bayer 4×4）提升观感
4. 文字类内容**不要抖动**（会糊），直接二值化成黑/白

### 4.3 打包函数（照抄即可）

```kotlin
// codes: w*h 个 0..3，行优先、自上而下、左到右
fun pack2bpp(codes: ByteArray, w: Int = 768, h: Int = 552): ByteArray {
    val rowBytes = w / 4
    val out = ByteArray(rowBytes * h)
    for (y in 0 until h) {
        val dst = (h - 1 - y) * rowBytes          // 缓冲自下而上
        val src = y * w
        for (x in 0 until w) {
            val c = (codes[src + x].toInt() and 3)
            val sx = w - 1 - x                    // 列倒序
            val sh = 6 - 2 * (sx and 3)
            out[dst + (sx shr 2)] = (out[dst + (sx shr 2)].toInt() or (c shl sh)).toByte()
        }
    }
    return out
}
```

为什么是「旋转 180°」：面板从上往下扫描，而固件按 `缓冲自下而上 + 逐行整行反转`
的契约取数（见 `epd_drv.c` 的 `row_mirror` / `H-1-j`）。所以 APP 交给固件的必须是
正向图转 180°。**你只管画一张正常的图**，`pack2bpp()` 负责剩下的。

现成实现：`sdk/moink_frame.py`（纯 Python、无 BLE 依赖，可直接单测）、
`sdk/moink_ble.py::pack_2bpp`（与它同一实现，只是转导出）、`sdk/moink_ble.ts::pack2bpp`。
调试图可以 `unpack2bpp()` 逆出来看，用来排查「方向反了 / 红黄互换」。
`python sdk/test_frame.py` 里有 180° 契约的四个锚点断言，移植 Kotlin/Swift 时照着抄。

### 4.4 方向自检

第一次上机建议按顺序发三张：整屏白 → 左半黑右半白 → 左上角一个红方块。
- 整屏白有雪花 → SPI 速率或 BUSY 超时问题
- 左右反了 → `pack2bpp` 的列倒序写错
- 上下反了 → 行倒序写错
- 红黄互换 → 色码顺序错（0=黑 1=白 2=黄 3=红，**不是** RGB 顺序）

## 5. uni-app / React Native

两者都没有标准 BLE API，推荐**协议层用 `sdk/moink_ble.ts`，只换传输层**：

```ts
import { MoinkBle, BleTransport, solidFrame, COLOR } from './sdk/moink_ble'

class UniTransport implements BleTransport {
  mtu = 517
  private cb: ((d: Uint8Array) => void) | null = null
  private dev: any

  async connect(opts: any) {
    // 用 uni-app 的蓝牙 API（或 plus.bluetooth）
    this.dev = await openBluetoothAdapterAndConnect(opts.namePrefix ?? 'MoInk')
    this.mtu = await this.dev.requestMtu(517)
    subscribeEvt((hex: string) => this.cb?.(hexToBytes(hex)))   // 0x81/0x01 包
  }
  async disconnect() { await this.dev.close() }
  onNotify(cb) { this.cb = cb }
  async writeCtrl(d: Uint8Array) { await this.dev.write(CTRL_UUID, d, true) }
  async writeData(d: Uint8Array) { await this.dev.writeNoRsp(DATA_UUID, d) }
}

const transport = new UniTransport()
const cli = new MoinkBle(transport)
await transport.connect({ namePrefix: 'MoInk' })
await cli.ping()
await cli.sendFrame(solidFrame(COLOR.WHITE), 768, 552, { waitDisplay: true })
```

React Native 用 `react-native-ble-plx`，传输层写法同构
（`writeCharacteristicWithResponseForService` / `writeCharacteristicWithoutResponseForService`）。

## 6. 事件表

| evt | 含义 | arg | payload |
|---|---|---|---|
| `0x01` | DISPLAY_DONE | 0 成功 / 1 失败 / 2 清理完成 / 3 缓存重刷完成 / 4 图库显示完成 | — |
| `0x02` | BUTTON | 1 单击 / 2 双击 / 3 长按 3 s 预警（20 s 恢复出厂） | — |
| `0x03` | LOW_BATTERY | — | `i16 mv` |
| `0x04` | STATE | 原状态 | B2.0.1可选u8图库只读flags；旧事件无payload |
| `0x05` | TRANSFER_GAP | — | `u16 expected_seq` |
| `0x06` | STORE_DONE | status，0 成功 | `slot u8, count u8, current u8`；255 表示无当前项/清空槽 |

`DISPLAY_DONE = 1` 时建议在 APP 上提示「刷新失败，请重试」，并附上
`GET_STATUS` 里的 `state` / `battery_mv`。

## 7. 错误码与处理建议

| status | 名称 | APP 该怎么做 |
|---|---|---|
| `0x02` | BAD_ARG | 参数写错（分片长度、cycles 越界），修 APP |
| `0x03` | BAD_STATE | 上一帧还在刷，等 `DISPLAY_DONE` 再发 |
| `0x04` | BUSY | 同上 / 队列满，退避 500 ms 重试 |
| `0x06` | GEOMETRY | 宽高不是 768×552，检查渲染尺寸 |
| `0x07` | LEN_MISMATCH | 载荷不是 105984，检查打包函数 |
| `0x08` | CRC | 传输丢包，**从 seq 0 重发即可，不用重新 BEGIN** |
| `0x09` | INCOMPLETE | payload 是缺的第一个 seq，从那里补发 |
| `0x0A` | SEQ_GAP | 同上（通常由 `TRANSFER_GAP` 事件提前告知） |
| `0x0B` | FLASH_ERROR | OTA 写 flash 失败，重来 |
| `0x0C` | OTA_VERIFY | 镜像 CRC 或头校验失败，确认 bin 没选错 |
| `0x0D` | NOT_READY | 比如 `CACHE_SHOW` 时还没有缓存 |
| `0x0E` | UNSUPPORTED | 操作不可用或响应放不下；检查 MTU |
| `0x0F` | FULL | 图库 8 槽已满；由用户选择删除后再存，不自动覆盖 |
| `0x10` | STALE | 图库有变化；丢弃当前部分下载，重新列举和下载 |

帧上传实现 CRC / INCOMPLETE / TRANSFER_GAP 有限次数恢复；OTA 实现 INCOMPLETE /
TRANSFER_GAP 恢复，最终镜像校验失败要 ABORT 并重新 BEGIN，不能在已终止会话中重发。

## 8. 电源与重连体验

- 空闲 `sleep_s`（默认 180 s）后根据 `timeout_action` 深睡或关机；BLE 断开，
  按键可唤醒；深睡还可以通过配置的定时器唤醒。
- `timeout_action = 1` 时是**断电**（需要硬件 MOS 自锁电路），唤醒同样要按键。
- APP 应订阅 `DISCONNECT` 并在 UI 上提示「设备已休眠，按一下设备按键唤醒」。
- 等待 DISPLAY_DONE 后调用 SLEEP_NOW。忙时以及 OTA 新镜像待确认时返回 BUSY；
  SLEEP_NOW 不会代替客户端等待刷屏完成。
- 用户可以在 APP 里把 `sleep_s` 调大（比如 900 s）或者设 `auto_wake_s`
  让设备定时醒来等人连。

重连策略建议：断开后扫描 3 轮 × 5 s，扫不到就提示按键唤醒，别无限重试（耗电）。

## 9. BLE OTA

B1 → B2.0.0 更换了分区布局，首次迁移需要完整串口刷写，不能只传应用 .bin。
B2 以后相同分区布局的版本可使用以下 BLE OTA 流程。

1. `OTA_BEGIN(total, crc32, chunk)` —— `crc32` 用 **zlib 标准**（Python `zlib.crc32`、
   Java `CRC32`、JS 用 `sdk/moink_ble.ts` 里的 `crc32()`）。
2. 发 DATA 分片（同传图，`seq` 从 0 开始）。
3. `OTA_END` → 固件校验 CRC32 + ESP 镜像头 + `esp_ota_set_boot_partition` → 回 OK。
4. 设备 1.5 s 后重启进新固件。新固件跑满 **45 s** 才 `mark_app_valid`，
   这期间断电/复位会被 bootloader 自动回滚到旧版本（`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`）。

失败/用户取消就发 `OTA_ABORT`（写的是另一个槽，不会弄坏当前固件）。

APP 侧建议：先比对 `version` 再决定是否升级；升级期间禁止休眠（保持屏幕常亮、
别切后台）；进度条按 `idx / total`。

## 10. 调试清单

| 现象 | 排查 |
|---|---|
| 扫不到设备 | 是否在 180 s 窗口内；是否已有连接；名字前缀对不对；iOS 是否 active scan |
| 连上但收不到 RSP | EVT 订阅了吗；seq 是不是回填给了错的 future |
| 传图极慢（>60 s） | Android 没 `requestMtu(517)`；或用了 Write With Response |
| `INCOMPLETE` 反复出现 | APP 丢包没重发；或分片长度与 `FRAME_BEGIN` 声明的 `chunk` 不一致 |
| 刷出来是白屏 | A0/A1 选错（`GET_STATUS.panel`）；或面板未接好 |
| 刷出来是雪花 | 传输数据错位（`seq` 端序写反：必须小端） |
| 关机无效 | 自锁脚硬件问题；固件侧已修 pad hold |
| 电量一直 `None` | 没接电池，或分压比/`BAT_FULLSCALE_MV` 不符 |

## 11. 设备图库与相框

B2.0.0 增加独立 1 MiB 的 8 槽图库。每项为 768×552 整帧和缩略图，
保留双 OTA 槽和独立的最近一帧缓存。图库编号为追加顺序的逻辑编号，删除后会前移，
手机不要永久保存旧编号当作图片 ID；每次列表/下载使用 revision 检查一致性。

| 功能 | Python SDK | TypeScript SDK |
|---|---|---|
| 数量/当前图/容量/版本 | gallery_info() | galleryInfo() |
| 完整分页列表 | gallery_list() | galleryList() |
| 显示图库项并等完成 | gallery_show(slot) | galleryShow(slot) |
| 下一张并等完成 | gallery_next() | galleryNext() |
| 删除并等落盘 | gallery_delete(slot) | galleryDelete(slot) |
| 清空并等落盘 | gallery_clear() | galleryClear() |
| 回读整帧 | gallery_download(slot) | galleryDownload(slot) |
| 回读缩略图 | gallery_download(slot, thumbnail=True) | galleryDownload(slot, true) |
| 保存并显示 | send_frame(..., save_gallery=True) | sendFrame(..., {saveGallery: true}) |
| 只保存，不刷屏 | send_frame(..., save_gallery=True, store_only=True) | sendFrame(..., {saveGallery: true, storeOnly: true}) |

参数 `saveGallery` 不隐式开启 waitDisplay；它等待 STORE_DONE，若还要确认刷新成功，
需同时启用 waitDisplay。仅存时没有 DISPLAY_DONE。删除/清空不改变屏幕像素，也不删 fcache。
同时调用两个修改类操作会遇到 BUSY；UI 在一次操作完成前应禁用新的写入。

B2.0.1在GET_STATUS版本字符串后兼容追加`gallery_read_flags`：bit0表示支持安全只读窗口，
bit1表示当前窗口开放，bit2表示本窗口前已成功追加（仅FRAME save_store成功后置位）；
STATE窗口开关事件也附同一字节，arg仍为3/4。只有idle或bit0/bit1均为1
时发起INFO/LIST/READ，state=3不能单独作为依据。保存/加载及刷新后的current/cache写入
不在窗口内；STORE_DONE与DISPLAY_DONE仍在原来的作业完成位置，不提前通知保存结果。
关闭窗口会排空当前固件读取回调；客户端暂停后续分片，遇BUSY稍后继续，revision变化则丢弃
旧部分数据。保持修改作业互斥和逐命令CTRL顺序，下载使用独立小响应缓冲，不写上传/显示帧。
通知属性值上限512字节，READ数据上限`min(MTU-19,496)`。SDK解析新增flags，旧固件默认0；
参考SDK仍保留原有修改作业串行机制，新增尾字段本身不代表所有客户端已开放并行调度。
flags=7不是提前的STORE_DONE：客户端若要在自己“存并显示”的窗口中保留旧缓存，需要同会话
pending追加、count+1、单步revision与全部旧prefix元数据一致的证据，最终回执仍须核对。
其他显示窗口flags=3；窗口关闭/complete同时清bit1/bit2，只有支持位保留为1。

```python
# 整帧先由 pack_2bpp() 编码；自动生成缩略图，无需 JPEG 依赖。
await cli.send_frame(frame, save_gallery=True, store_only=True)
items = await cli.gallery_list()
await cli.gallery_next()
packed = await cli.gallery_download(items[0]["slot"])
thumb = await cli.gallery_download(items[0]["slot"], thumbnail=True)
# 默认缩略图是 192x138 packed2bpp，使用条目中的宽高和格式解码。
# 删除后重新拉列表，不复用旧编号。
await cli.gallery_delete(items[0]["slot"])
await cli.set_settings(auto_wake_s=3600, wake_show=True)
```

可选上传 JPEG 缩略图：

```ts
await cli.sendFrame(frame, 768, 552, {
  saveGallery: true, storeOnly: true,
  thumbnail: jpegBytes, thumbnailFormat: 2, thumbnailW: 192, thumbnailH: 138,
})
const items = await cli.galleryList()
const image = await cli.galleryDownload(items[0].slot, true)
// thumbnailFormat=2 时交给 JPEG 解码器；=1 时 unpack2bpp(image, thumbnailW, thumbnailH)。
```

JPEG 必须由手机生成，最大 20000 字节；固件仅检查边界标记和 CRC，不负责 JPEG 解码。
未提供缩略图时，固件自动从整帧生成 192×138 packed2bpp 缩略图。
下载由 EVT 的逐命令 RSP 返回，**不要订阅 DATA**。SDK 会拒绝 CRC、错偏移、空片和版本不匹配。

双击按键显示图库下一张，空图库保持当前屏幕。需要重刷缓存时单独调用 cache_show/cacheShow。
设置 `auto_wake_s` 和 `wake_show=True` 后，定时唤醒可离线换下一张，不依赖手机在线。
单纯设置 `auto_wake_s` 只控制唤醒时间，`wake_show=False` 时保留画面。
B1 固件不支持图库扩展，先探测 `gallery_info()`，遇到 UNKNOWN_OP 则禁用图库入口。

## 12. 离线验证与上机验收

```sh
python sdk/test_frame.py
python sdk/test_ble.py
node --experimental-transform-types --test sdk/test_ble.ts
```

TypeScript mock 测试使用支持类型转换的 Node 22.7+，不需要手机插件。
这些测试覆盖线格式、设置兼容、列表分页、完成事件早于响应、分片恢复、CRC/偏移/版本错误、
存储失败和有限重试。它们没有建立真实 BLE 连接。
恢复出厂只清除设备设置，保留独立图库和最近一帧缓存；清图库需单独操作。

仍需在目标硬件验证：A0/A1 显示方向和颜色、8 槽存满/重启/断电恢复、双击切图、定时唤醒换图、
JPEG/默认缩略图手机预览、不同 MTU 读回及 BLE OTA 回滚。
