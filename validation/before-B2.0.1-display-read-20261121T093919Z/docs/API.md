# 墨印 MoInk · BLE 协议规范（api = 1）

> 本文档是 **APP 与固件之间的唯一权威接口定义**。固件实现见 `src/ble_api.c`，
> 参考客户端见 `sdk/moink_ble.py`（Python）与 `sdk/moink_ble.ts`（TypeScript）。
> 当前固件 B2.0.0，ESP-IDF 5.2.2。不兼容的字节布局变更才增加 `MOINK_API_VERSION`；
> 本次新增命令、TLV 和可选末尾字段，api 保持 1。分区变化需要首次完整串口刷入，见 README。

---

## 1. 角色与总体设计

- 设备是 **GATT Server（从机）**，APP 是 Client（主机）。设备不主动连接。
- 全部控制与数据走 **一条 BLE 连接上的 3 个特征**：
  | 特征 | 用途 | 方向 |
  |---|---|---|
  | **CTRL** | 命令（几十字节的小包） | APP → 设备 |
  | **DATA** | 大数据流（帧载荷 / 固件镜像） | APP → 设备 |
  | **EVT** | 响应 + 异步事件（notify） | 设备 → APP |
- **没有** HTTP、没有 WiFi 热点、没有配对弹窗（NoInputNoOutput，开放连接）。
- 整数一律 **Little-Endian**（C3 原生字节序，APP 侧 `DataView` 直接读）。

## 2. 广播与设备发现

| 项 | 值 |
|---|---|
| 设备名 | `MoInk-XXXX`（XXXX = MAC 后 2 字节，可被设置里的自定义名覆盖，≤ 20 字节 UTF-8） |
| 广播包 | Flags + **128-bit Service UUID**（不塞名字，避免超 31 字节） |
| 扫描响应包 | Complete Local Name |
| Service UUID | `6D6F496E-4B00-4000-8000-000000000000` |

> ⚠️ 名字在**扫描响应**里，APP 必须用 **active scan**（Android `SCAN_MODE_LOW_LATENCY`
> / iOS `CBScanOptionAllowDuplicates=false` 默认就是 active）。只按服务 UUID 过滤即可，
> 不要按名字做连接判据。

## 3. GATT 表

| 特征 | UUID | 属性 | 说明 |
|---|---|---|---|
| **CTRL** | `6D6F496E-4B00-4000-8000-000000000001` | Write, WriteWithoutResponse | 命令通道 |
| **DATA** | `6D6F496E-4B00-4000-8000-000000000002` | WriteWithoutResponse | 大数据通道（帧 / OTA） |
| **EVT**  | `6D6F496E-4B00-4000-8000-000000000003` | Notify | 响应与事件 |

- 连上后**先订阅 EVT**（写 CCCD = `0x0001`），否则收不到任何回执。
- 建议连上后立刻 `requestMtu(517)`；建议 MTU ≥ 100，确保状态、设置和图库列表响应可容纳。
- 一条连接上限 = 1。设备已被别的手机连上时，新的连接会被拒绝（NimBLE 直接抛弃）。

## 4. 三条通道的帧格式

### 4.1 CTRL（APP → 设备）

```
偏移  长度  字段
0     1    op      命令码（见 §5）
1     1    seq     主机自增序号 0..255，回绕；设备原样回显，用于匹配响应
2     ..   args    命令参数
```

- 用 **Write（有响应）** 发命令最稳（丢包会被 ATT 层重传）；
  WriteWithoutResponse 也可以，但**真正的回执看 EVT**，不要依赖 GATT 写成功。
- `seq` 只需在「还没收到对应 RSP」的窗口内唯一。APP 串行发命令即可。

### 4.2 DATA（APP → 设备）

```
偏移  长度  字段
0     2    seq   分片序号（从 0 开始，逐片 +1，u16）
2     ..   chunk 载荷；除最后一片外长度恒等于 FRAME_BEGIN/OTA_BEGIN 协商的 chunk
```

- 只能 **WriteWithoutResponse**（有响应会慢一个数量级）。
- 设备按 `seq * chunk` 顺序落位，**必须严格有序**；乱序时设备丢弃后续分片并
  发一次 `TRANSFER_GAP` 事件（§8），最终 `FRAME_END` 会回 `INCOMPLETE + expected_seq`。

### 4.3 EVT（设备 → APP，notify）

两种消息，靠首字节区分：

```
RSP   （命令响应）  [0x01][seq][status][payload...]
EVENT （异步事件）  [0x81][evt ][arg   ][payload...]
```

- 一条 RSP 一定对应一条刚发出的 CTRL 命令（`seq` 相同）。
- 每个 RSP 均为单包（≤ MTU-3）。B2 在 MTU 装不下完整响应时返回 `UNSUPPORTED`，
  不把截断数据当作成功；图库 LIST 自动分页、READ 自动缩短数据片。建议协商 MTU ≥ 100。

## 5. 命令码总表

| op | 名称 | 方向语义 |
|---|---|---|
| `0x01` | PING | 握手 / 探测延迟 |
| `0x02` | GET_STATUS | 设备状态、电量、唤醒原因 |
| `0x03` | GET_SETTINGS | 读设置 |
| `0x04` | SET_SETTINGS | 写设置（TLV） |
| `0x10` | FRAME_BEGIN | 开始传一帧 |
| `0x12` | FRAME_END | 结束并校验 + 触发刷屏 |
| `0x13` | FRAME_ABORT | 放弃本次传输 |
| `0x20` | CACHE_SHOW | 用设备内缓存的上一帧重刷 |
| `0x21` | CACHE_INFO | 查询缓存状态 |
| `0x30` | GHOST_CLEAR | 残影清理 |
| `0x40` | OTA_BEGIN | 开始固件升级 |
| `0x42` | OTA_END | 结束升级并重启 |
| `0x43` | OTA_ABORT | 放弃升级（不切槽） |
| `0x50` | REBOOT | 重启 |
| `0x51` | FACTORY_RESET | 恢复出厂设置（清 moink 设置命名空间 + 重启） |
| `0x52` | SLEEP_NOW | 立刻深睡（释放连接） |
| `0x22` | CAROUSEL_INFO | 设备图库数量、当前项、容量、版本 |
| `0x23` | CAROUSEL_SHOW | 显示指定图库项；255 = 下一张 |
| `0x24` | CAROUSEL_DELETE | 删除图库项，等待 STORE_DONE |
| `0x25` | CAROUSEL_CLEAR | 清空图库，等待 STORE_DONE |
| `0x26` | CAROUSEL_LIST | 分页获取图库和缩略图元数据 |
| `0x27` | CAROUSEL_READ | 按偏移回读整帧或缩略图，数据由 EVT 的 RSP 返回 |

## 6. 命令详解

### 6.1 `0x01` PING

args：无
RSP payload：

| 偏移 | 类型 | 字段 |
|---|---|---|
| 0 | u8 | api |
| 1 | u8 | fw_len |
| 2 | u8[] | fw（ASCII，如 `B2.0.0`） |

### 6.2 `0x02` GET_STATUS

args：无
RSP payload：

| 偏移 | 类型 | 字段 | 说明 |
|---|---|---|---|
| 0 | u8 | api | 协议版本 = 1 |
| 1 | u8 | state | 0 idle / 1 收帧中 / 2 OTA 中 / 3 显示或图库作业中 / 4 清理中 / 6 准备休眠或重启 |
| 2 | u8 | panel | 0 = A0，1 = A1 |
| 3 | i16 | bat_mv | 电池 mV；**-1 = 未接电池/悬空** |
| 5 | u32 | uptime_s | 本次上电运行秒数 |
| 9 | u32 | heap_free | 空闲堆字节 |
| 13 | u16 | mtu | 当前协商 MTU |
| 15 | u8 | wake_reason | 0 上电/复位 / 1 按键 / 2 定时 / 3 其它 |
| 16 | u32 | sleep_s | 空闲超时秒（0 = 不超时） |
| 20 | u8 | timeout_action | 0 = 深睡 / 1 = 断电关机 |
| 21 | u32 | wake_s | 定时自动唤醒秒（0 = 关） |
| 25 | u8 | cache_valid | 1 = fcache 里有可用帧 |
| 26 | u8 | fw_len | |
| 27 | u8[] | fw | ASCII |

### 6.3 `0x03` GET_SETTINGS

args：无
RSP payload：

| 偏移 | 类型 | 字段 |
|---|---|---|
| 0 | u8 | panel |
| 1 | u32 | sleep_s |
| 5 | u8 | timeout_action |
| 6 | u32 | wake_s |
| 10 | u8 | gpio_preset（0 默认 / 1 排线 / 2 整合板 / 3 自定义） |
| 11 | u8[8] | pins = [ADC, WAKE, SCK, MOSI, CS, DC, RST, BUSY] |
| 19 | u8 | cache_on |
| 20 | u8 | name_len |
| 21 | u8[] | name（UTF-8） |
| 21 + name_len | u8 | wake_show（B2 新增；旧固件缺省为 0） |

### 6.4 `0x04` SET_SETTINGS

args：TLV 列表，可一次带多个字段：

```
[id u8][len u8][value ...]  重复
```

| id | 字段 | len | 取值 |
|---|---|---|---|
| `0x01` | panel | 1 | 0 = A0，1 = A1（**立即生效**） |
| `0x02` | sleep_s | 4 | 0 = 不超时 |
| `0x03` | timeout_action | 1 | 0 = 深睡，1 = 断电关机 |
| `0x04` | wake_s | 4 | 0 = 关闭 |
| `0x05` | name | 0..20 | UTF-8 设备名；长度 0 恢复默认名称 |
| `0x06` | gpio_preset | 1 | 0/1/2/3 |
| `0x07` | pins | 8 | 仅当 preset = 3；8 字节顺序同上，**不得占用 GPIO2（自锁）** |
| `0x08` | cache_on | 1 | 1 = 刷屏后落盘缓存 |
| `0x09` | wake_show | 1 | 0 = 唤醒不刷；1 = 定时唤醒显示图库下一张（空图库不刷屏） |

RSP payload：

| 偏移 | 类型 | 字段 |
|---|---|---|
| 0 | u8 | applied | 成功应用的字段数 |
| 1 | u8 | err_id | 首个被拒字段的 id；0 = 无错误 |
| 2 | u8 | err_code | 见 §7（通常 BAD_ARG） |
| 3 | u8 | flags | bit0 = 1 表示**需要重启**才生效（pins / gpio_preset 变更时置位） |

**语义：全有或全无。** 任一字段非法则整个请求不生效，`applied = 0`。
同一次请求中的 `gpio_preset` / `pins` 顺序不限；任意尾部残缺 TLV 拒绝。
改过 `gpio_preset` / `pins` 后需 `REBOOT` 才生效（SPI 总线与 ADC 通道只在启动时初始化）。

### 6.5 `0x10` FRAME_BEGIN

args：

| 偏移 | 类型 | 字段 | 说明 |
|---|---|---|---|
| 0 | u8 | flags | bit0 = 不写缓存；bit1 = 追加设备图库；bit2 = 仅存图库不刷屏（必须有 bit1）；其余位须 0 |
| 1 | u16 | w | 固定 768 |
| 3 | u16 | h | 固定 552 |
| 5 | u32 | len | 载荷长度 = w/4 × h = 105984 |
| 9 | u16 | crc16 | 载荷的 CRC-16/CCITT-FALSE |
| 11 | u16 | chunk | 主机打算用的 DATA 载荷长度，建议 512 |

原有参数固定 13 字节。启用 bit1 时，可追加以下 **11 字节**缩略图描述，参数总长 24：

| 偏移 | 类型 | 字段 |
|---|---|---|
| 13 | u8 | thumb_format：1 packed2bpp / 2 JPEG |
| 14 | u16 | thumb_w |
| 16 | u16 | thumb_h |
| 18 | u32 | thumb_len |
| 22 | u16 | thumb_crc16（仅缩略图数据的 CRC16） |

DATA 连续发送 `整帧 + 缩略图`，分片序号不重置，边界可落在片内。
`len` 和原 `crc16` 仍只覆盖整帧；缩略图单独校验。
JPEG 最大 20000 字节，须有 SOI `FF D8` / EOI `FF D9`；固件不解码 JPEG。
packed2bpp 缩略图宽必须为 4 的倍数，最大 200×150，长度 `w*h/4`，方向同整帧。
两种缩略图宽高均须非零，且 `64 + frame_len + thumb_len <= 131072`。
不追加缩略图时，设备自动从整帧生成 192×138 的 packed2bpp 缩略图。
图库满返回 `FULL`，不覆盖旧图。旧的 flags=0/1 调用行为保持不变；B1 固件不支持这些新增位。

RSP payload：`chunk_used u16, chunk_max u16`
（`chunk_max` = min(MTU-5, 512)。`chunk` 只是主机的**建议值**：超过 `chunk_max`
时设备自动降为 `chunk_max`，**主机必须以 RSP 的 `chunk_used` 为准**；小于 16
返回 `BAD_ARG` 并回填 `[MIN, MAX]`。这样 Web Bluetooth 这类拿不到真实 MTU 的
平台可以一律先发 512，由设备裁决。）

> payload 是**已按 180° 契约打包好的 2bpp 位图**，设备不做任何图像处理。
> 打包规则见 §10，参考实现见 `sdk/moink_ble.py` 的 `pack_2bpp()`。

错误：`GEOMETRY`（不是 768×552）、`LEN_MISMATCH`、`BUSY`（正在刷屏/OTA）。

### 6.6 `0x12` FRAME_END

args：无

| 结果 | status | payload |
|---|---|---|
| 收满且 CRC 正确 | `OK` | `queued u8` = 1（显示/存储作业已排队） |
| 未收满 | `INCOMPLETE` | `expected_seq u16`（从这里重发即可） |
| CRC 错 | `CRC` | `expected_seq u16` = 0（从头发，不必重新 BEGIN） |

CRC 正确后回 OK 仅表示作业入队。正常刷屏完成发送 `DISPLAY_DONE`；
启用 save_store 时另发 `STORE_DONE`，其 arg 是存储状态码。
仅存图库（bit2）不刷屏、不发送 `DISPLAY_DONE`，必须等 `STORE_DONE`。
上传缩略图 CRC 错也会回 `CRC`，重发整个 `整帧 + 缩略图`。
APP 要在发 FRAME_END **之前**登记完成监听，避免先到事件后到 RSP 的竞态。
刷屏时长取决于面板，SDK 建议 A1 完成等待预留 900 秒；这里没有真机耗时测量。

### 6.7 `0x13` FRAME_ABORT
args：无 → `OK`，状态回 idle，丢弃已收数据。

### 6.8 `0x20` CACHE_SHOW

args：无 → `OK`，随后刷屏并异步 `DISPLAY_DONE(arg=3)`；缓存无效返回 `NOT_READY`。

CACHE 是独立的最近成功刷屏缓存（192 KB fcache 分区）。8 槽图库使用独立 store
分区；CACHE_SHOW 不等同于图库下一张，切图库应使用 CAROUSEL_SHOW。

### 6.9 `0x21` CACHE_INFO
args：无
RSP payload：`valid u8, w u16, h u16, crc16 u16, len u32`

### 6.10 `0x30` GHOST_CLEAR
args：`cycles u8`（1..4，屏幕约 15~25 s / 次）
→ `OK`，随后异步 `DISPLAY_DONE(arg=2)`。

### 6.11 `0x40` OTA_BEGIN
args：`total u32, crc32 u32（整包 CRC-32/ISO-HDLC）, chunk u16`
RSP payload：`chunk_used u16, chunk_max u16`（与 §6.5 同规则：`chunk` 由设备裁决，
超过上限自动降级，主机以 `chunk_used` 为准）

- 设备选「非当前启动槽」写入，**只有在 `OTA_END` 校验通过后才切槽**；
  中途断电/断连 = 旧固件照常启动，不会变砖。
- OTA 期间拒绝一切其它写操作（`BUSY`），只接受 DATA / OTA_END / OTA_ABORT。

### 6.12 `0x42` OTA_END
args：无
- 校验 CRC32 + 镜像头（0xE9 魔数 / chip_id = 0x0005 / app 描述符）：
  - 通过 → `OK`，约 1.5 s 后重启（**连接会断开，属正常**）
  - 失败 → `OTA_VERIFY`，状态回 idle，需重新 `OTA_BEGIN`
- 升级**不重置**用户设置（屏型 / 引脚 / 电源 / 名称全部保留）。

### 6.13 `0x43` OTA_ABORT
args：无 → `OK`，放弃本次升级，不切槽。

### 6.14 `0x50` REBOOT
args：`delay_s u8`（可选，缺省 1）→ `OK`，随后重启。用于让引脚设置生效。

### 6.15 `0x51` FACTORY_RESET
args：无 → `OK`（约 500 ms 后清理 `moink` 设置命名空间并重启，连接断开）。
独立图库和最近一帧缓存保留；清图库须另发 CAROUSEL_CLEAR。

### 6.16 `0x52` SLEEP_NOW
args：无 → `OK`，约 300 ms 后进入深睡、停止广播。用于 APP「今天就到这儿」。
深睡后需**按设备按键**才能重新被扫描到（除非设置了 `wake_s` 定时唤醒）。
忙时或 OTA 新镜像尚待确认（正常约 45 秒）返回 BUSY，不会立刻休眠。

### 6.17 `0x22` CAROUSEL_INFO

args：无。RSP：`count u8, current u8, slots u8, revision u32`（7 字节）。
`current=255` 表示没有已确认显示的图库项，正常容量 `slots=8`。
`slot` 是追加顺序的逻辑索引 `0..count-1`；删除后后续索引前移，须重新列举。
`revision` 随成员/排序修改而变化，仅显示另一张不改变它；重连后重新取版本。

### 6.18 `0x23` CAROUSEL_SHOW

args：`slot u8`，`255` 表示循环下一张（当前为空则从第 0 张开始）。
RSP OK 表示入队，完成后 `DISPLAY_DONE arg=4`，失败为 `arg=1`。
只有显示成功才更新 `current`。空图库返回 `NOT_READY`。

### 6.19 `0x24` CAROUSEL_DELETE / `0x25` CAROUSEL_CLEAR

DELETE args：`slot u8`；CLEAR args：无。
RSP OK 仅表示入队；最终结果是 `STORE_DONE`。
这是删除图库记录；不会改当前墨水屏像素，不会清掉独立 fcache。
删除后重新获取列表和版本。清空/删除更新存储元数据，不是安全擦除 Flash 内容。

### 6.20 `0x26` CAROUSEL_LIST

args：`from u8, num u8`，num 必须大于 0。
RSP 头：`revision u32, from u8, returned u8`（6 字节），后跟 returned 条 22 字节记录：

| 记录内偏移 | 类型 | 字段 |
|---|---|---|
| 0 | u8 | slot |
| 1 | u16 | w |
| 3 | u16 | h |
| 5 | u32 | len（整帧字节数） |
| 9 | u16 | crc16（整帧） |
| 11 | u8 | thumb_format：1 packed2bpp / 2 JPEG |
| 12 | u16 | thumb_w |
| 14 | u16 | thumb_h |
| 16 | u32 | thumb_len |
| 20 | u16 | thumb_crc16 |

每页最多 `floor((MTU-12)/22)` 条；还存在条目但装不下一条时返回 `UNSUPPORTED`，
因此非空列表最低 MTU=34。下一页 from 加上 returned；每页必须同一 revision。

### 6.21 `0x27` CAROUSEL_READ

请求：`slot u8, kind u8, offset u32, requested u16, revision u32`（12 字节）。
kind=0 读整帧，kind=1 读缩略图；requested>0；revision 使用刚获取的列表版本。
RSP 载荷：

| 偏移 | 类型 | 字段 |
|---|---|---|
| 0 | u8 | slot |
| 1 | u8 | kind |
| 2 | u32 | offset |
| 6 | u32 | total（所选载荷总长） |
| 10 | u16 | crc16（所选完整载荷） |
| 12 | u8 | format：1 packed2bpp / 2 JPEG（整帧固定为 1） |
| 13 | u8[] | data |

单片 data 最多 `MTU-19` 字节，由设备裁决（MTU=23 时 4 字节）。
**数据通过 EVT 的 RSP 返回，DATA 特征仍然只写入，不支持 Notify。**
客户端逐片检查 slot/kind/offset/total/CRC/format，直到 offset=total，再校验整个载荷 CRC16。
相同偏移可安全重读；版本变化返回 `STALE`，应丢弃部分下载，刷新列表后从头读取，
不可拼接新旧图库内容。READ 不持有跨命令锁。图库命令在上传/刷屏/存储/OTA 忙时返回 BUSY。

## 7. 状态码

| 值 | 名称 | 含义 |
|---|---|---|
| `0x00` | OK | |
| `0x01` | UNKNOWN_OP | 不支持的命令码 |
| `0x02` | BAD_ARG | 参数非法（长度/范围/引脚冲突） |
| `0x03` | BAD_STATE | 当前状态不接受该命令（如未 BEGIN 就发 DATA） |
| `0x04` | BUSY | 正在刷屏 / 正在 OTA / 正在收帧 |
| `0x05` | NO_MEM | 内存不足 |
| `0x06` | GEOMETRY | 几何不被当前屏画像接受（只接受 768×552） |
| `0x07` | LEN_MISMATCH | `len != w/4*h` |
| `0x08` | CRC | 载荷 CRC 不匹配 |
| `0x09` | INCOMPLETE | 数据未收满，附 `expected_seq` |
| `0x0A` | SEQ_GAP | DATA 分片乱序（同时会发 `TRANSFER_GAP` 事件） |
| `0x0B` | FLASH_ERROR | 分区读写失败 |
| `0x0C` | OTA_VERIFY | 镜像校验失败 |
| `0x0D` | NOT_READY | 缓存为空 / 升级未开始 |
| `0x0E` | UNSUPPORTED | 该操作不可用，或 MTU 不足以容纳完整响应 |
| `0x0F` | FULL | 图库已满，不覆盖旧图 |
| `0x10` | STALE | 图库版本变化，重新列举并重启下载 |

## 8. 事件（EVT，`[0x81][evt][arg][payload]`）

| evt | 名称 | arg | payload | 说明 |
|---|---|---|---|---|
| `0x01` | DISPLAY_DONE | result | — | 0 刷屏成功 / 1 刷屏失败 / 2 清理完成 / 3 缓存重刷成功 / 4 图库显示成功 |
| `0x02` | BUTTON | action | — | 1 单击 / 2 双击 / 3 长按预警（距恢复出厂 3 s） |
| `0x03` | LOW_BATTERY | 0 | `i16 mv` | 电量低于阈值（约 3400 mV，每次唤醒最多一次） |
| `0x04` | STATE | state | — | 状态变化（可与 `GET_STATUS` 交叉验证） |
| `0x05` | TRANSFER_GAP | 0 | `u16 expected_seq` | 收到乱序 DATA，需回退重发 |
| `0x06` | STORE_DONE | status | `slot u8, count u8, current u8` | 保存/删除/清空的结果；0 成功，失败用 §7 状态码；清空 slot=255，current=255 表示无当前项 |

- 设备**只在已订阅（CCCD=1）时**发通知，未订阅时静默丢弃，不影响状态机。
- 事件可能插在命令响应之间：解析时按首字节分流，不要假设「下一条一定是 RSP」。

## 9. 完整时序示例

### 9.1 传一张图（happy path）

```
APP                                          DEVICE
 │ PING                        ──────────────▶ │
 │ ◀────────────── RSP ok api=1 fw="B2.0.0"    │
 │ requestMtu(517)             ──────────────▶ │
 │                                             │
 │ CTRL: 10 05 00 0003 2802 009E0100 B129 0002 │  ← FRAME_BEGIN
 │ ◀──── RSP ok, chunk_used=512, chunk_max=512 │
 │ DATA: 0000 <512B>  ───────────────────────▶ │  seq=0
 │ DATA: 0100 <512B>  ───────────────────────▶ │  seq=1
 │ ...（共 207 片，最后一片 24 字节）...        │
 │ CTRL: 12 06                ──────────────▶ │  ← FRAME_END
 │ ◀──────────────────── RSP ok, queued=1      │
 │                                             │  （设备刷屏 2~4 s）
 │ ◀────────────── EVENT 01 arg=0 (DISPLAY_DONE) │
```

`FRAME_BEGIN` 那 15 字节拆解：

| 字节 | 含义 |
|---|---|
| `10` | op = FRAME_BEGIN |
| `05` | seq |
| `00` | flags（0 = 落盘缓存） |
| `00 03` | w = 768 |
| `28 02` | h = 552 |
| `00 9E 01 00` | len = 105984 |
| `B1 29` | crc16 |
| `00 02` | chunk = 512 |

### 9.2 丢包恢复

```
APP                                          DEVICE
 │ DATA seq=100 ...                            │ （seq=100 丢了）
 │ DATA: 6500 <512B>  ───────────────────────▶ │ 乱序！记录 expected=100
 │                                             │ 丢弃 101.. 并忽略后续
 │ ◀──────── EVENT 05 arg=0, expected_seq=100  │
 │ DATA: 6400 <512B>  ───────────────────────▶ │ 对齐，继续
 │ ...                                         │
```

若 APP 没处理该事件，`FRAME_END` 会回 `INCOMPLETE + expected_seq`，从该 seq 重发即可。

### 9.3 升级固件

```
 │ CTRL: 40 <seq> <total u32> <crc32 u32> <512 u16> │
 │ ◀─────────────── RSP ok, chunk_used=512         │
 │ DATA 分片 ...（同帧传输）                        │
 │ CTRL: 42 <seq>                                  │
 │ ◀─────────────── RSP ok（1.5 s 后设备重启）      │
 │            （连接断开，重连后 PING 看新版本）     │
```

## 10. 2bpp 图像打包契约（APP 侧必做）

设备**只收已打包的 2bpp 位图**，不做任何缩放/量化/旋转。规则（与 R 系列完全一致）：

1. 逻辑图 768×552，像素色码：**0 = 黑，1 = 白，2 = 黄，3 = 红**。
2. **180° 旋转**：缓冲区第 `r` 行 = 逻辑第 `H-1-r` 行；行内像素倒序。
3. 每字节 4 像素、**MSB 优先**：像素 `x` 占字节内位 `(6 - 2*(x % 4))`。
4. 缓冲长度 = `W/4 × H` = 105984 字节。

参考实现（Python，与固件源代码契约一致；实屏方向仍需上机验证）：

```python
def pack_2bpp(idx, w=768, h=552):
    """idx: 长度 w*h 的 0..3 数组，行优先、逻辑方向（左上起）。
    返回 105984 字节，直接作为 FRAME 载荷。"""
    rb = w >> 2
    out = bytearray(rb * h)
    for r in range(h):
        ro = (h - 1 - r) * w          # 缓冲第 r 行 = 逻辑第 H-1-r 行
        bo = r * rb
        for xb in range(rb):
            xa = w - 4 - (xb << 2)    # 行内倒序，4 像素一组
            out[bo + xb] = ((idx[ro + xa + 3] & 3) << 6 |
                            (idx[ro + xa + 2] & 3) << 4 |
                            (idx[ro + xa + 1] & 3) << 2 |
                            (idx[ro + xa]     & 3))
    return bytes(out)
```

纯色快速路径（省掉逐像素循环）：黑 `0x00`、白 `0x55`、黄 `0xAA`、红 `0xFF`。

**校验方法**：整帧填白（`0x55`）传上去，屏幕应全白；填一条黑带在最上面，
屏幕黑带应在**顶部**——如果跑到下面，说明 180° 方向搞反了。

### 10.1 CRC 算法

- **CRC-16/CCITT-FALSE**（帧载荷）：poly `0x1021`，init `0xFFFF`，无反射、无终异或。
  金标向量：`"123456789"` → `0x29B1`。
- **CRC-32/ISO-HDLC**（OTA 整包）：poly `0xEDB88320` 反射，init `0xFFFFFFFF`，
  末异或 `0xFFFFFFFF`（= Python `zlib.crc32`，= `esp_rom_crc32_le`）。

## 11. 参数与性能建议

| 项 | 建议值 | 说明 |
|---|---|---|
| MTU | 517 | 实际协商以链路为准；不足以放下状态包则回 UNSUPPORTED |
| DATA 载荷 | 512 | = MTU - 5（3 字节 ATT 头 + 2 字节 seq） |
| 连接间隔 | 15~30 ms，latency 0 | `requestConnectionPriority(CONNECTION_PRIORITY_HIGH)` |
| in-flight | 4~8 包 | WriteWithoutResponse 连续推，别等每包回执 |
| PHY | 依芯片与链路能力 | 本仓库没有 PHY 吞吐实测，不依赖 2M |
| 单帧耗时 | 需真机测量 | 105984 B / 512 B = 207 包 |
| 刷屏超时 | A1 驱动等待最长 180 s | 等 DISPLAY_DONE，客户端预留约 900 s（含驱动重试）；不是性能实测 |

Android 侧两个必做项：
- `BluetoothGatt.requestConnectionPriority(CONNECTION_PRIORITY_HIGH)`
- `writeCharacteristic` 的 `WRITE_TYPE_NO_RESPONSE` + 自己控制队列深度
  （Android 的 `WRITE_TYPE_NO_RESPONSE` 队列满会静默丢包 → 正是 `TRANSFER_GAP`
  存在的原因，务必实现回退重发）

## 12. 版本兼容规则

- `MOINK_API_VERSION` 只在**不兼容**变更时 +1；APP 用 `PING` 的 `api` 字段判断。
- 新增 opcode / 新增事件 / 新增 TLV id 属于**向后兼容**，api 不变：
  - APP 收到未知 op 的响应 = 设备不支持，应降级隐藏该功能；
  - 设备收到未知 op 返回 `UNKNOWN_OP`，不应断开。
- 可选新增字段缺省（如 wake_show）用默认值；必需字段不足则报解析错误，不把截断数据当有效状态。
- B1 对新增 opcode 返回 UNKNOWN_OP。新 APP 应探测能力；B1 上不要发送新增上传标志。
- B2 修改 Flash 分区，不能仅通过 B1 的 BLE OTA 安装 B2；首次完整串口迁移见 README。

## 13. 连接生命周期与电源策略（APP 必须知道）

- 设备空闲 `sleep_s`（默认 180 s）后进入**深睡**：广播停止、连接断开。
  此时 APP 扫描不到设备，**必须让用户按一下设备按键**才能重新连接。
- 有以下活动会续命：连接建立、任意 CTRL 命令、DATA 分片、刷屏进行中。
- 若设置了 `wake_s`（如 1 小时），设备会定时醒来广播 `AUTO_WAKE_WINDOW_S = 180 s`
  等人连；这个窗口内没连上就再睡。
- `timeout_action = 1` 时超时不是深睡而是**断电关机**（依赖板上的 MOS 自锁电路）；
  关机后只能靠物理按键重新上电。
- 断连不影响已经完成的画面（墨水屏断电保持）。
## 14. 验证范围

新增 Python/TypeScript mock tests 覆盖分片恢复、存储完成先于 RSP、分页、读回 CRC/偏移/版本、
wake_show 兼容解析和失败返回。这些是离线协议测试，不代表 BLE 链路、实屏、断电恢复或手机 UI 验收。
