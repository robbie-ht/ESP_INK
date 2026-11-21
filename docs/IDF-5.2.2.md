# 本工程 vs ESP-IDF 5.2.2：逐条核对记录

> 这份文件是"我没有凭记忆写代码"的证据清单。每条都在本机 `esp-idf-v5.2.2`
> 源码里点开确认过，并且整工程用 `idf.py build` 真编译过。
>
> IDF 路径（取自 `epd/gdey037z03_idf/build/project_description.json`）：
> `D:/APPS/ESP-IDF/ESP-IDF_5.2.2/Espressif/frameworks/esp-idf-v5.2.2`，target `esp32c3`。

## 1. 用到的 API / 符号

| 用法 | 证据（相对 IDF 根目录） |
|---|---|
| `esp_deep_sleep_enable_gpio_wakeup()` + `ESP_GPIO_WAKEUP_GPIO_LOW` | `components/esp_hw_support/include/esp_sleep.h:435` / `:47` |
| C3 支持 GPIO 深睡唤醒 | `components/soc/esp32c3/include/soc/soc_caps.h:168` → `SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP (1)` |
| **唤醒原因只有单因接口** | `esp_sleep.h:653 esp_sleep_get_wakeup_cause()`；位图版 `esp_sleep_get_wakeup_causes()` 在 5.2.2 **不存在** |
| `nimble_port_init()` 同时初始化 controller | `components/bt/porting/nimble/include/nimble/nimble_port.h:39-44`（注释明写 "Initialize controller and NimBLE host stack"），所以不要再调 `esp_nimble_hci_and_controller_init()` |
| `ble_gatts_notify_custom()` **自己释放 mbuf** | `components/bt/host/nimble/nimble/nimble/host/src/ble_gattc.c:4458`（成功路径先 `txom = NULL`）→ `:4471 os_mbuf_free_chain(txom)`；**调用方不能再 free** |
| notify 功能默认开启 | `components/bt/host/nimble/port/include/esp_nimble_cfg.h:618-619` → `MYNEWT_VAL_BLE_GATT_NOTIFY (1)` |
| `ble_gatts_*` 声明在 `host/ble_gatt.h`，**没有** `ble_gatts.h`；由 `ble_hs.h:34` 引入 | `components/bt/host/nimble/nimble/nimble/host/include/host/ble_gatt.h:705`（`ble_gatts_notify_custom`）、`:1058`（`add_svcs`）、`:1114`（`count_cfg`） |
| 服务表用文件作用域复合字面量 `(struct ble_gatt_chr_def[]){...}` | 5.2.2 自带 `bleprph`/`bleuart`/`blufi`/各 GATT service 全是这个写法 |
| `BLE_GAP_EVENT_CONNECT/DISCONNECT/CONN_UPDATE/ADV_COMPLETE/SUBSCRIBE/MTU/PHY_UPDATE_COMPLETE` | `.../host/include/host/ble_gap.h:129,130,132,138,143,144,147` |
| `BLE_GAP_ADV_ITVL_MS()` / `ble_gap_terminate()` | `ble_gap.h:47` / `:2237` |
| `ble_att_mtu()` / `ble_att_set_preferred_mtu()` | `.../host/include/host/ble_att.h:177` / `:203` |
| `struct ble_gatt_chr_def.access_cb` / `.val_handle` | `ble_gatt.h:795` / `:817` |
| `struct ble_hs_adv_fields.num_uuids128` / `uuids128_is_complete` / `name_is_complete` | `.../host/include/host/ble_hs_adv.h:60,61,66` |
| `gpio_hold_dis()` / `gpio_deep_sleep_hold_dis()`（自锁修复依赖） | `components/driver/gpio/include/driver/gpio.h:424,429` |
| `ESP_ERR_INVALID_CRC` / `_SIZE` / `_NOT_FOUND` | `components/esp_common/include/esp_err.h:33,28,29` |
| `ADC_ATTEN_DB_12` / `ADC_BITWIDTH_12` / `ADC_ULP_MODE_DISABLE` | `components/hal/include/hal/adc_types.h:50,59,64` |
| `esp_ota_set_boot_partition` / `get_next_update_partition` / `mark_app_valid_cancel_rollback` / `get_state_partition` | `components/app_update/include/esp_ota_ops.h:187,237,286,318` |
| `ESP_SLEEP_WAKEUP_GPIO` / `ESP_SLEEP_WAKEUP_UNDEFINED` / `ESP_SLEEP_WAKEUP_TIMER` | `esp_sleep.h:112,105,109` |

## 2. 用到的 Kconfig

| 符号 | 证据 |
|---|---|
| `BT_NIMBLE_ROLE_PERIPHERAL` / `_BROADCASTER` | `components/bt/host/nimble/Kconfig.in:139` / `:146` |
| `BT_NIMBLE_MAX_CONNECTIONS` | `Kconfig.in:65` |
| `BT_NIMBLE_NVS_PERSIST` | `Kconfig.in:160` |
| `BT_NIMBLE_SECURITY_ENABLE` | `Kconfig.in:167` |
| `BT_NIMBLE_ATT_PREFERRED_MTU` | `Kconfig.in:244` |
| `BT_NIMBLE_EXT_ADV` | `Kconfig.in:552` |
| `BT_NIMBLE_HOST_TASK_STACK_SIZE` | `Kconfig.in:124` |
| `BT_NIMBLE_L2CAP_COC_MAX_NUM` | `Kconfig.in:95` |
| `RTC_CLK_SRC_INT_RC` / `RTC_CLK_CAL_CYCLES`（C3 专用） | `components/esp_hw_support/port/esp32c3/Kconfig.rtc:7,19` |
| `ESP_CONSOLE_USB_SERIAL_JTAG` / `ESP_CONSOLE_SECONDARY_NONE` | `components/esp_system/Kconfig:236,256` |
| `BOOTLOADER_APP_ROLLBACK_ENABLE` / `BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP` | `components/bootloader/Kconfig.projbuild:311,376` |

## 3. 核对时抓出来的真问题

### 3.1 会直接编译失败的（已改）

1. **`esp_sleep_get_wakeup_causes()` 在 5.2.2 不存在**（复数、返回位图的版本是 5.3+ 才有）。
   改成 `power_woke_by_button()` / `power_woke_by_timer()` 两个谓词，内部走单因接口。
2. `ble_api_notify_event()` 原先只在 `.c` 里定义、头文件没声明 → 调用处是隐式声明，
   在 `-Werror=all` 下直接失败。已在 `ble_api.h` 补声明。
3. `s_ota_erased` 声明位置在用它的 `transfer_reset()` 之后 → 未声明标识符。已上移。

### 3.2 静默失效的（已改）

4. **`sdkconfig.defaults` 里 5 个 Kconfig 名在 5.2.2 根本不存在**：
   `NIMBLE_ACL_BUF_COUNT`、`NIMBLE_ACL_BUF_SIZE`、`HCI_EVT_HI_BUF_COUNT`、
   `HCI_EVT_LO_BUF_COUNT`、`MSYS1_BLOCK_COUNT`。写进 defaults 只会被忽略。
   真实名字见 `components/bt/sdkconfig.rename:281-286`：
   - `ACL_BUF_COUNT` → `BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT`
   - `ACL_BUF_SIZE` → `BT_NIMBLE_TRANSPORT_ACL_SIZE`
   - `MSYS1_BLOCK_COUNT` → `BT_NIMBLE_MSYS_1_BLOCK_COUNT`
   - `HCI_EVT_HI_BUF_COUNT` → `BT_NIMBLE_TRANSPORT_EVT_COUNT`
   - `HCI_EVT_LO_BUF_COUNT` → `BT_NIMBLE_TRANSPORT_EVT_DISCARD_COUNT`

   已全部删掉——实测生成的 `sdkconfig` 是 MSYS 12 块 / ACL 24×255 / 事件 30，够用。
   （我原先按 "SOC_ESP_NIMBLE_CONTROLLER 下默认 24 块" 写注释，也是错的：C3 跑的是
   ESP32 BT 控制器，不是 NimBLE 控制器，所以是 12 块。已按实测更正。）

5. **`sdkconfig` 里会出现上面那些"旧名字"，属于正常现象**：kconfgen 按
   `sdkconfig.rename` 把它们作为废弃别名回写。不是本工程写的，也不会生效。

### 3.3 与构建无关但必须记住的

6. **构建输入必须纯 ASCII**。中文 Windows 下 kconfgen 用 GBK 读
   `sdkconfig.defaults` / `partitions.csv` / `CMakeLists.txt`，UTF-8 中文会直接
   `UnicodeDecodeError: 'gbk' codec can't decode byte 0x88` 中断配置阶段。
   `.c/.h`、`.md` 里的中文没这个问题（编译器/编辑器按 UTF-8 读）。

## 4. 怎么复现这份核对

```powershell
# 1. 让 IDF 用自带的 Python 3.11（否则会去找 py3.14 的 venv）
$env:PATH = 'D:\APPS\ESP-IDF\ESP-IDF_5.2.2\Espressif\tools\idf-python\3.11.2;' + $env:PATH
. 'D:\APPS\ESP-IDF\ESP-IDF_5.2.2\Espressif\frameworks\esp-idf-v5.2.2\export.ps1'

# 2. 编译
cd D:\Projects\VSCode\ESP32\Moink-APP
idf.py build

# 3. 看尺寸
idf.py size
```