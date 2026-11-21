/*
 * BLE 协议栈实现 —— GATT 服务 / 帧传输状态机 / 命令分发 / OTA。
 *
 * 协议权威定义：docs/API.md（改字节布局必须同步该文档与 sdk/ 两个客户端，
 * 并把 version.h 的 MOINK_API_VERSION +1）。
 *
 * 三条特征：
 *   CTRL 6D6F496E-4B00-4000-8000-000000000001  Write/WriteNR  命令
 *   DATA 6D6F496E-4B00-4000-8000-000000000002  WriteNR        大数据
 *   EVT  6D6F496E-4B00-4000-8000-000000000003  Notify         响应+事件
 *
 * 线程模型：
 *   GATT 写回调在 NimBLE host 任务里执行，只做「拷进缓冲 + 状态机推进」；
 *   真正刷屏投递给 main.c 的 display_task（队列 s_job_q），刷完回调
 *   ble_api_job_finish() 发 DISPLAY_DONE。这样 BLE 协程永远不会被 20s 的
 *   整屏刷新或 26 个扇区的 flash 擦除挡住太久（单次擦除 <40ms，可接受）。
 */
#include "ble_api.h"
#include "version.h"
#include "settings.h"
#include "power.h"
#include "epd_drv.h"
#include "cache.h"
#include "store.h"
#include "crc_util.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble";

/* ---------------- 协议常量（与 docs/API.md 严格一致） ---------------- */

/* opcode */
#define OP_PING         0x01
#define OP_GET_STATUS   0x02
#define OP_GET_SETTINGS 0x03
#define OP_SET_SETTINGS 0x04
#define OP_FRAME_BEGIN  0x10
#define OP_FRAME_END    0x12
#define OP_FRAME_ABORT  0x13
#define OP_CACHE_SHOW   0x20
#define OP_CACHE_INFO   0x21
#define OP_STORE_INFO   0x22
#define OP_STORE_SHOW   0x23
#define OP_STORE_DELETE 0x24
#define OP_STORE_CLEAR  0x25
#define OP_STORE_LIST   0x26
#define OP_STORE_READ   0x27
#define OP_GHOST_CLEAR  0x30
#define OP_OTA_BEGIN    0x40
#define OP_OTA_END      0x42
#define OP_OTA_ABORT    0x43
#define OP_REBOOT       0x50
#define OP_FACTORY      0x51
#define OP_SLEEP_NOW    0x52

/* status */
#define ST_OK           0x00
#define ST_UNKNOWN_OP   0x01
#define ST_BAD_ARG      0x02
#define ST_BAD_STATE    0x03
#define ST_BUSY         0x04
#define ST_NO_MEM       0x05
#define ST_GEOMETRY     0x06
#define ST_LEN_MISMATCH 0x07
#define ST_CRC          0x08
#define ST_INCOMPLETE   0x09
#define ST_SEQ_GAP      0x0A
#define ST_FLASH_ERROR  0x0B
#define ST_OTA_VERIFY   0x0C
#define ST_NOT_READY    0x0D
#define ST_UNSUPPORTED  0x0E
#define ST_STORE_FULL   0x0F
#define ST_STALE        0x10

/* event */
#define EVT_DISPLAY_DONE 0x01
#define EVT_BUTTON       0x02
#define EVT_LOW_BATTERY  0x03
#define EVT_STATE        0x04
#define EVT_TRANSFER_GAP 0x05

/* TLV id（SET_SETTINGS） */
#define TLV_PANEL   0x01
#define TLV_SLEEP   0x02
#define TLV_ACTION  0x03
#define TLV_WAKE    0x04
#define TLV_NAME    0x05
#define TLV_PRESET  0x06
#define TLV_PINS    0x07
#define TLV_CACHE   0x08
#define TLV_WAKE_SHOW 0x09

/* 时间参数 */
#define FRAME_IDLE_ABORT_MS   12000
#define OTA_IDLE_ABORT_MS     30000
#define MAX_CHUNK             512     /* = 517 MTU - 5 */
#define MIN_CHUNK             16
#define MAX_ATT_MTU           517

/* ---------------- UUID ---------------- */

/* 128-bit UUID 在 NimBLE 里按「小端字节序」声明：最后一个字节写在最前。 */
static ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
    0x00, 0x40, 0x00, 0x4b, 0x6e, 0x49, 0x6f, 0x6d);   /* 6D6F496E-4B00-4000-8000-...
                                                            规范序末 12 位全 0 */
static ble_uuid128_t s_ctl_uuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
    0x00, 0x40, 0x00, 0x4b, 0x6e, 0x49, 0x6f, 0x6d);
static ble_uuid128_t s_dat_uuid = BLE_UUID128_INIT(
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
    0x00, 0x40, 0x00, 0x4b, 0x6e, 0x49, 0x6f, 0x6d);
static ble_uuid128_t s_evt_uuid = BLE_UUID128_INIT(
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
    0x00, 0x40, 0x00, 0x4b, 0x6e, 0x49, 0x6f, 0x6d);

/* ---------------- 内部状态 ---------------- */

static uint8_t        *s_fb;
static uint8_t        *s_thumb;
/* Serializes host commands, button claims, completion and timeout resets. */
static SemaphoreHandle_t s_lock;
static volatile uint8_t s_state = BLE_ST_IDLE;
static uint16_t        s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t        s_ctl_handle, s_dat_handle, s_evt_handle;
static QueueHandle_t   s_job_q;
static volatile bool   s_running;
static volatile bool   s_adv_on;
static uint8_t         s_wake_reason;
static volatile int64_t s_last_rx_ms;

/* 收帧上下文 */
static uint16_t s_fw, s_fh;
static uint32_t s_framelen, s_recv;
static uint32_t s_thumb_len;
static uint16_t s_thumb_w, s_thumb_h, s_thumb_crc_want, s_thumb_crc_calc;
static uint8_t s_thumb_format;
static uint16_t s_expect;          /* 期望的下一个 DATA 分片序号 */
static uint16_t s_chunk;           /* 本传输协商的分片长度 */
static uint16_t s_crc_calc, s_crc_want;
static uint8_t  s_begin_flags;
static bool     s_gap_notified;

/* OTA 上下文 */
static const esp_partition_t *s_ota_part;
static uint32_t s_ota_total, s_ota_written, s_ota_crc_calc, s_ota_crc_want;
static uint32_t s_ota_erased;      /* 已连续擦除到的偏移（见 ota_flash_write） */
static uint8_t  s_ota_err;

static uint8_t s_rx[600];          /* GATT 写缓冲（最大 ATT 载荷 514 + 余量） */

/* ---------------- 小工具 ---------------- */

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}
static inline void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void notify_raw(const uint8_t *b, uint16_t n)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || s_evt_handle == 0) return;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(b, n);
    if (!om) {
        ESP_LOGW(TAG, "no mbuf for notify (%u B)", n);
        return;
    }
    /* ble_gatts_notify_custom() 无论成功失败都会释放 om，调用方不要再 free。 */
    int rc = ble_gatts_notify_custom(s_conn, s_evt_handle, om);
    if (rc != 0 && rc != BLE_HS_ENOTCONN) {
        ESP_LOGD(TAG, "notify rc=%d", rc);
    }
}

static uint16_t cur_mtu(void);

/* RSP: [0x01][seq][status][payload] */
static void respond(uint8_t seq, uint8_t status, const void *payload, uint16_t len)
{
    uint8_t b[MAX_ATT_MTU - 3];
    /* Never hand NimBLE an oversized notification: return an actionable short
     * error for fixed replies that require a negotiated MTU. */
    if (len > cur_mtu() - 6 || len > sizeof(b) - 3) {
        status = ST_UNSUPPORTED;
        len = 0;
    }
    b[0] = 0x01;
    b[1] = seq;
    b[2] = status;
    if (len && payload) memcpy(b + 3, payload, len);
    notify_raw(b, (uint16_t)(3 + len));
}

/* EVENT: [0x81][evt][arg][payload] */
void ble_api_notify_event(uint8_t evt, uint8_t arg, const void *payload, uint8_t len)
{
    uint8_t b[40];
    if (len > sizeof(b) - 3) len = sizeof(b) - 3;
    b[0] = 0x81;
    b[1] = evt;
    b[2] = arg;
    if (len && payload) memcpy(b + 3, payload, len);
    notify_raw(b, (uint16_t)(3 + len));
}

static uint16_t cur_mtu(void)
{
    uint16_t m = (s_conn != BLE_HS_CONN_HANDLE_NONE) ? ble_att_mtu(s_conn) : 23;
    if (m < 23) m = 23;
    return m;
}

static uint16_t max_chunk(void)
{
    int m = (int)cur_mtu() - 5;      /* 3 字节 ATT 头 + 2 字节 seq */
    if (m > MAX_CHUNK) m = MAX_CHUNK;
    return (uint16_t)m;
}

static void transfer_reset(void)
{
    free(s_thumb);
    s_thumb = NULL;
    s_thumb_len = 0;
    s_thumb_format = 0;
    s_thumb_w = s_thumb_h = 0;
    s_thumb_crc_calc = 0xFFFF;
    s_recv = 0;
    s_expect = 0;
    s_crc_calc = 0xFFFF;
    s_gap_notified = false;
    s_ota_part = NULL;
    s_ota_total = 0;
    s_ota_written = 0;
    s_ota_erased = 0;
    s_ota_crc_calc = crc32_begin();
    s_ota_err = ST_OK;
}

uint8_t *ble_api_frame_buf(void) { return s_fb; }
const uint8_t *ble_api_thumb_buf(void) { return s_thumb; }
uint8_t  ble_api_state(void) { return s_state; }
void ble_api_set_state(uint8_t st)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = st;
    if (s_lock) xSemaphoreGive(s_lock);
}
bool     ble_api_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }
void     ble_api_set_wake_reason(uint8_t reason) { s_wake_reason = reason; }

/* ---------------- OTA 落盘 ---------------- */

/*
 * 不用 esp_ota_begin() 整片预擦（BLE 传输期间长时间无数据流动会让链路显得很慢）。
 *
 * ★ 擦除必须按「已擦边界」推进，不能只看当前偏移是否扇区对齐：
 *   chunk=180 这类非 4096 约数时，某一片会从 3960 起跨到 3961..4140，
 *   后 44 字节落在**未擦除**的扇区 1 上，esp_partition_write 不会报错，
 *   但那些位从 0 变不成目标值 —— 镜像静默损坏，只在 OTA_END 的 CRC 上体现。
 *   所以这里先擦到本片末尾，再按扇区边界拆写。
 */
static uint8_t  s_ota_buf[4096];

static esp_err_t ota_flash_write(uint32_t off, const uint8_t *p, uint32_t n)
{
    const esp_partition_t *part = s_ota_part;
    if (!part) return ESP_ERR_INVALID_STATE;
    if (off + n > part->size) return ESP_ERR_INVALID_SIZE;

    uint32_t end = off + n;
    while (s_ota_erased < end) {
        uint32_t elen = 0x1000;
        if (s_ota_erased + elen > part->size) elen = part->size - s_ota_erased;
        if (elen == 0) return ESP_FAIL;
        if (esp_partition_erase_range(part, s_ota_erased, elen) != ESP_OK) return ESP_FAIL;
        s_ota_erased += elen;
    }

    uint32_t done = 0;
    while (done < n) {
        uint32_t at = off + done;
        uint32_t room = 0x1000 - (at & 0xFFF);
        uint32_t w = (n - done < room) ? (n - done) : room;
        memcpy(s_ota_buf, p + done, w);
        if (esp_partition_write(part, at, s_ota_buf, w) != ESP_OK) return ESP_FAIL;
        done += w;
    }
    return ESP_OK;
}

/* ---------------- 命令处理 ---------------- */

static void handle_frame_begin(uint8_t seq, const uint8_t *a, uint16_t n)
{
    if (n != 13 && n != 24) { respond(seq, ST_BAD_ARG, NULL, 0); return; }
    if (s_state != BLE_ST_IDLE) { respond(seq, ST_BUSY, NULL, 0); return; }
    uint8_t flags = a[0];
    uint16_t w = rd16(a + 1), h = rd16(a + 3);
    uint32_t len = rd32(a + 5);
    uint16_t crc = rd16(a + 9), chunk = rd16(a + 11);
    if ((flags & ~7u) || ((flags & 4u) && !(flags & 2u)) ||
        (n == 24 && !(flags & 2u))) {
        respond(seq, ST_BAD_ARG, NULL, 0); return;
    }
    const epd_profile_t *pr = epd_profile();
    if (w != pr->w || h != pr->h) { respond(seq, ST_GEOMETRY, NULL, 0); return; }
    if (len != pr->buf_len) { respond(seq, ST_LEN_MISMATCH, NULL, 0); return; }
    if (flags & 2u) {
        int count = 0;
        store_meta_get(&count, NULL);
        if (!store_slots()) { respond(seq, ST_NOT_READY, NULL, 0); return; }
        if (count >= store_slots()) { respond(seq, ST_STORE_FULL, NULL, 0); return; }
    }
    uint8_t tf = 0;
    uint16_t tw = 0, th = 0, tc = 0;
    uint32_t tl = 0;
    if (n == 24) {
        tf = a[13]; tw = rd16(a + 14); th = rd16(a + 16);
        tl = rd32(a + 18); tc = rd16(a + 22);
        if ((tf != STORE_THUMB_PACKED && tf != STORE_THUMB_JPEG) ||
            !tw || !th || !tl || tl > STORE_THUMB_JPEG_MAX ||
            len + tl + STORE_HEADER_SZ > STORE_SLOT_SZ ||
            (tf == STORE_THUMB_PACKED && (tw % 4u || tw > 200 || th > 150 ||
                                         tl != (uint32_t)tw * th / 4u))) {
            respond(seq, ST_BAD_ARG, NULL, 0); return;
        }
    }
    uint16_t mx = max_chunk();
    if (chunk < MIN_CHUNK) {
        uint8_t r[4]; wr16(r, MIN_CHUNK); wr16(r + 2, mx);
        respond(seq, ST_BAD_ARG, r, 4); return;
    }
    if (chunk > mx) chunk = mx;
    transfer_reset();
    if (tl) {
        s_thumb = malloc(tl);
        if (!s_thumb) { respond(seq, ST_NO_MEM, NULL, 0); return; }
    }
    s_begin_flags = flags;
    s_fw = w; s_fh = h; s_framelen = len;
    s_thumb_len = tl; s_thumb_format = tf; s_thumb_w = tw; s_thumb_h = th;
    s_thumb_crc_want = tc;
    s_crc_want = crc; s_chunk = chunk;
    s_state = BLE_ST_RECV_FRAME;
    s_last_rx_ms = esp_timer_get_time() / 1000;
    uint8_t r[4]; wr16(r, chunk); wr16(r + 2, mx);
    respond(seq, ST_OK, r, 4);
}

static void handle_frame_end(uint8_t seq)
{
    if (s_state != BLE_ST_RECV_FRAME) { respond(seq, ST_BAD_STATE, NULL, 0); return; }
    if (s_recv != s_framelen + s_thumb_len) {
        uint8_t r[2]; wr16(r, s_expect);
        respond(seq, ST_INCOMPLETE, r, 2); return;
    }
    if (s_crc_calc != s_crc_want ||
        (s_thumb_len && s_thumb_crc_calc != s_thumb_crc_want)) {
        s_recv = 0; s_expect = 0;
        s_crc_calc = s_thumb_crc_calc = 0xFFFF;
        s_gap_notified = false;
        uint8_t r[2] = { 0, 0 };
        respond(seq, ST_CRC, r, 2); return;
    }
    if (s_thumb_format == STORE_THUMB_JPEG &&
        (s_thumb_len < 4 || s_thumb[0] != 0xFF || s_thumb[1] != 0xD8 ||
         s_thumb[s_thumb_len - 2] != 0xFF || s_thumb[s_thumb_len - 1] != 0xD9)) {
        respond(seq, ST_BAD_ARG, NULL, 0); return;
    }
    ble_job_t job = {
        .type = BLE_JOB_FRAME,
        .save_cache = (s_begin_flags & 1u) ? 0 : 1,
        .save_store = (s_begin_flags & 2u) ? 1 : 0,
        .no_show = (s_begin_flags & 4u) ? 1 : 0,
        .thumb_format = s_thumb_format, .thumb_w = s_thumb_w,
        .thumb_h = s_thumb_h, .thumb_len = s_thumb_len,
        .w = s_fw, .h = s_fh,
    };
    s_state = BLE_ST_REFRESHING;
    if (xQueueSend(s_job_q, &job, 0) != pdTRUE) {
        s_state = BLE_ST_RECV_FRAME;
        respond(seq, ST_BUSY, NULL, 0); return;
    }
    uint8_t queued = 1;
    respond(seq, ST_OK, &queued, 1);
}

static void handle_ota_begin(uint8_t seq, const uint8_t *a, uint16_t n)
{
    if (n != 10) { respond(seq, ST_BAD_ARG, NULL, 0); return; }
    if (s_state != BLE_ST_IDLE) { respond(seq, ST_BUSY, NULL, 0); return; }

    uint32_t total = rd32(a);
    uint32_t crc = rd32(a + 4);
    uint16_t chunk = rd16(a + 8);

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        ESP_LOGE(TAG, "no update partition");
        respond(seq, ST_UNSUPPORTED, NULL, 0);
        return;
    }
    if (total < 1024 || total > part->size) {
        ESP_LOGW(TAG, "ota size %lu out of range (slot %lu)",
                 (unsigned long)total, (unsigned long)part->size);
        respond(seq, ST_BAD_ARG, NULL, 0);
        return;
    }
    /* 设备裁决分片长度：主机提的只是建议。Web Bluetooth 拿不到真实 MTU，
       猜大了原本会直接撞 BAD_ARG，这里降级更友好 —— 主机以 RSP 为准。 */
    uint16_t mx = max_chunk();
    if (chunk < MIN_CHUNK) {
        uint8_t r[4];
        wr16(r, MIN_CHUNK);
        wr16(r + 2, mx);
        respond(seq, ST_BAD_ARG, r, 4);
        return;
    }
    if (chunk > mx) chunk = mx;

    s_ota_part = part;
    s_ota_total = total;
    s_ota_crc_want = crc;
    s_ota_crc_calc = crc32_begin();
    s_ota_written = 0;
    s_ota_erased = 0;
    s_ota_err = ST_OK;
    s_expect = 0;
    s_chunk = chunk;
    s_gap_notified = false;
    s_state = BLE_ST_RECV_OTA;
    s_last_rx_ms = esp_timer_get_time() / 1000;

    uint8_t r[4];
    wr16(r, chunk);
    wr16(r + 2, mx);
    ESP_LOGI(TAG, "ota begin: %lu B crc32=0x%08lX chunk=%u slot=%s",
             (unsigned long)total, (unsigned long)crc, chunk, part->label);
    respond(seq, ST_OK, r, 4);
}

static void reboot_task(void *arg)
{
    uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    esp_restart();
}

static void handle_ota_end(uint8_t seq)
{
    if (s_state != BLE_ST_RECV_OTA) { respond(seq, ST_BAD_STATE, NULL, 0); return; }

    if (s_ota_err != ST_OK) {
        uint8_t e = s_ota_err;
        s_state = BLE_ST_IDLE;
        transfer_reset();
        respond(seq, e, NULL, 0);
        return;
    }
    if (s_ota_written != s_ota_total) {
        uint8_t r[2];
        wr16(r, s_expect);
        respond(seq, ST_INCOMPLETE, r, 2);
        return;
    }
    if (crc32_end(s_ota_crc_calc) != s_ota_crc_want) {
        ESP_LOGE(TAG, "ota crc32 0x%08lX != 0x%08lX",
                 (unsigned long)crc32_end(s_ota_crc_calc),
                 (unsigned long)s_ota_crc_want);
        s_state = BLE_ST_IDLE;
        transfer_reset();
        respond(seq, ST_OTA_VERIFY, NULL, 0);
        return;
    }

    /* 镜像头快速校验：ESP 镜像魔数 0xE9 + chip_id（ESP32-C3 = 5）。 */
    uint8_t hdr[32] = { 0 };
    if (esp_partition_read(s_ota_part, 0, hdr, sizeof(hdr)) != ESP_OK ||
        hdr[0] != 0xE9 || rd16(hdr + 12) != 5) {
        ESP_LOGE(TAG, "ota image header invalid (magic 0x%02X chip %u)",
                 hdr[0], rd16(hdr + 12));
        s_state = BLE_ST_IDLE;
        transfer_reset();
        respond(seq, ST_OTA_VERIFY, NULL, 0);
        return;
    }

    esp_err_t e = esp_ota_set_boot_partition(s_ota_part);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(e));
        s_state = BLE_ST_IDLE;
        transfer_reset();
        respond(seq, ST_FLASH_ERROR, NULL, 0);
        return;
    }

    ESP_LOGW(TAG, "ota ok, switching boot to %s, rebooting", s_ota_part->label);
    s_state = BLE_ST_SLEEPING;
    if (xTaskCreate(reboot_task, "ota_reboot", 2048, (void *)(uintptr_t)1500, 5, NULL) != pdPASS) {
        transfer_reset();
        s_state = BLE_ST_IDLE;
        respond(seq, ST_NO_MEM, NULL, 0); return;
    }
    respond(seq, ST_OK, NULL, 0);
}

static void handle_set_settings(uint8_t seq, const uint8_t *a, uint16_t n)
{
    if (s_state != BLE_ST_IDLE) { respond(seq, ST_BUSY, NULL, 0); return; }
    const moink_settings_t before = *settings_get();
    moink_settings_t cfg = before;
    uint8_t fields = 0, err_id = 0, status = ST_OK, flags = 0;
    bool has_pins = false, has_preset = false;
    uint16_t i = 0;
    while (i < n) {
        if (n - i < 2) { err_id = a[i]; status = ST_BAD_ARG; break; }
        uint8_t id = a[i++], len = a[i++];
        err_id = id;
        if (len > n - i) { status = ST_BAD_ARG; break; }
        const uint8_t *v = a + i;
        bool valid = true;
        switch (id) {
        case TLV_PANEL:
            valid = len == 1 && v[0] <= 1;
            if (valid) cfg.panel = v[0];
            break;
        case TLV_SLEEP:
            valid = len == 4;
            if (valid) cfg.sleep_s = rd32(v);
            break;
        case TLV_ACTION:
            valid = len == 1 && v[0] <= SETT_ACTION_OFF;
            if (valid) cfg.timeout_action = v[0];
            break;
        case TLV_WAKE:
            valid = len == 4;
            if (valid) cfg.wake_s = rd32(v);
            break;
        case TLV_NAME:
            valid = len < SETT_NAME_MAX && !memchr(v, 0, len);
            if (valid) { memcpy(cfg.name, v, len); cfg.name[len] = 0; }
            break;
        case TLV_PRESET:
            valid = len == 1 && v[0] < GPIO_PRESET_COUNT;
            if (valid) { cfg.gpio_preset = v[0]; has_preset = true; }
            break;
        case TLV_PINS:
            valid = len == PIN_IDX_COUNT;
            if (valid) { memcpy(cfg.pins.v, v, len); has_pins = true; }
            break;
        case TLV_CACHE:
            valid = len == 1 && v[0] <= 1;
            if (valid) cfg.cache_on = v[0];
            break;
        case TLV_WAKE_SHOW:
            valid = len == 1 && v[0] <= 1;
            if (valid) cfg.wake_show = v[0];
            break;
        default: valid = false; break;
        }
        if (!valid) { status = ST_BAD_ARG; break; }
        fields++;
        i += len;
    }
    if (status == ST_OK && fields == 0) status = ST_BAD_ARG;
    if (status == ST_OK && (has_preset || has_pins)) {
        if (cfg.gpio_preset < GPIO_PRESET_NPRESET) {
            if (has_pins) { status = ST_BAD_ARG; err_id = TLV_PINS; }
            else cfg.pins = PIN_PRESETS[cfg.gpio_preset];
        } else if (!pin_cfg_valid(&cfg.pins)) {
            status = ST_BAD_ARG; err_id = TLV_PINS;
        }
    }
    if (status == ST_OK) {
        esp_err_t err = settings_apply(&cfg);
        if (err != ESP_OK) {
            status = err == ESP_ERR_INVALID_ARG ? ST_BAD_ARG : ST_FLASH_ERROR;
        } else {
            err_id = 0;
            if (cfg.panel != before.panel) epd_set_panel((epd_panel_t)cfg.panel);
            if (strcmp(cfg.name, before.name) != 0) ble_api_restart_adv();
            if (cfg.gpio_preset != before.gpio_preset ||
                memcmp(&cfg.pins, &before.pins, sizeof(cfg.pins))) flags = 1;
        }
    }
    uint8_t r[4] = { status == ST_OK ? fields : 0, err_id,
                     status == ST_OK ? 0 : status, flags };
    respond(seq, status, r, sizeof(r));
}

static uint8_t store_status(esp_err_t err)
{
    if (err == ESP_OK) return ST_OK;
    if (err == STORE_ERR_FULL) return ST_STORE_FULL;
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_STATE) return ST_NOT_READY;
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE) return ST_BAD_ARG;
    if (err == ESP_ERR_NO_MEM) return ST_NO_MEM;
    if (err == ESP_ERR_INVALID_CRC) return ST_CRC;
    return ST_FLASH_ERROR;
}

static bool queue_job(const ble_job_t *job, uint8_t state)
{
    if (s_state != BLE_ST_IDLE || !s_job_q) return false;
    s_state = state;
    if (xQueueSend(s_job_q, job, 0) == pdTRUE) return true;
    s_state = BLE_ST_IDLE;
    return false;
}

/* Caller owns s_lock. Selection is repeated by display_task under the claimed
 * state; no membership mutation can occur between this check and execution. */
static bool queue_carousel(uint8_t slot)
{
    int count = 0;
    store_meta_get(&count, NULL);
    if (!count || (slot != 255 && slot >= count)) return false;
    ble_job_t job = { .type = BLE_JOB_STORE_SHOW, .slot = slot };
    return queue_job(&job, BLE_ST_REFRESHING);
}

static void handle_store_list(uint8_t seq, const uint8_t *a, uint16_t n)
{
    if (s_state != BLE_ST_IDLE) { respond(seq, ST_BUSY, NULL, 0); return; }
    if (n != 2 || !a[1]) { respond(seq, ST_BAD_ARG, NULL, 0); return; }
    if (!store_slots()) { respond(seq, ST_NOT_READY, NULL, 0); return; }
    int count = 0;
    store_meta_get(&count, NULL);
    uint8_t from = a[0], num = a[1];
    if (from > count) { respond(seq, ST_BAD_ARG, NULL, 0); return; }
    uint16_t room = cur_mtu() - 6; /* ATT + RSP headers */
    uint8_t max_num = (room - 6) / 22;
    if (!max_num && from < count) { respond(seq, ST_UNSUPPORTED, NULL, 0); return; }
    if (num > max_num) num = max_num;
    if (num > count - from) num = count - from;
    uint8_t r[MAX_ATT_MTU - 6];
    wr32(r, store_revision()); r[4] = from; r[5] = num;
    uint16_t at = 6;
    for (uint8_t k = 0; k < num; ++k) {
        store_info_t info;
        esp_err_t err = store_info(from + k, &info);
        if (err != ESP_OK) { respond(seq, store_status(err), NULL, 0); return; }
        r[at++] = from + k;
        wr16(r + at, info.w); at += 2;
        wr16(r + at, info.h); at += 2;
        wr32(r + at, info.len); at += 4;
        wr16(r + at, info.crc16); at += 2;
        r[at++] = info.thumb_format;
        wr16(r + at, info.thumb_w); at += 2;
        wr16(r + at, info.thumb_h); at += 2;
        wr32(r + at, info.thumb_len); at += 4;
        wr16(r + at, info.thumb_crc16); at += 2;
    }
    respond(seq, ST_OK, r, at);
}

static void handle_store_read(uint8_t seq, const uint8_t *a, uint16_t n)
{
    if (s_state != BLE_ST_IDLE) { respond(seq, ST_BUSY, NULL, 0); return; }
    if (n != 12 || a[1] > 1 || !rd16(a + 6)) {
        respond(seq, ST_BAD_ARG, NULL, 0); return;
    }
    if (rd32(a + 8) != store_revision()) { respond(seq, ST_STALE, NULL, 0); return; }
    uint8_t slot = a[0];
    bool thumb = a[1] != 0;
    uint32_t off = rd32(a + 2);
    uint16_t requested = rd16(a + 6);
    store_info_t info;
    esp_err_t err = store_info(slot, &info);
    if (err != ESP_OK) { respond(seq, store_status(err), NULL, 0); return; }
    uint32_t total = thumb ? info.thumb_len : info.len;
    if (!total) { respond(seq, ST_NOT_READY, NULL, 0); return; }
    if (off > total) { respond(seq, ST_BAD_ARG, NULL, 0); return; }
    uint16_t cap = cur_mtu() - 19; /* ATT 3 + RSP 3 + metadata 13 */
    if (requested > cap) requested = cap;
    if (requested > total - off) requested = total - off;
    uint8_t r[MAX_ATT_MTU - 6];
    r[0] = slot; r[1] = thumb;
    wr32(r + 2, off); wr32(r + 6, total);
    wr16(r + 10, thumb ? info.thumb_crc16 : info.crc16);
    r[12] = thumb ? info.thumb_format : STORE_THUMB_PACKED;
    err = store_read(slot, thumb, off, r + 13, requested);
    if (err != ESP_OK) { respond(seq, store_status(err), NULL, 0); return; }
    respond(seq, ST_OK, r, 13 + requested);
}

static void on_ctrl(const uint8_t *p, uint16_t n)
{
    if (n < 2) return;
    uint8_t op = p[0];
    uint8_t seq = p[1];
    const uint8_t *a = p + 2;
    uint16_t alen = (uint16_t)(n - 2);

    /* Read-only polling must not keep an abandoned transfer alive. */
    if (op == OP_FRAME_END || op == OP_OTA_END)
        s_last_rx_ms = esp_timer_get_time() / 1000;
    power_activity();

    switch (op) {
    case OP_PING: {
        const char *v = MOINK_VERSION;
        uint8_t vl = (uint8_t)strlen(v);
        uint8_t r[32];
        r[0] = MOINK_API_VERSION;
        r[1] = vl;
        memcpy(r + 2, v, vl);
        respond(seq, ST_OK, r, (uint8_t)(2 + vl));
        break;
    }

    case OP_GET_STATUS: {
        const moink_settings_t *cfg = settings_get();
        uint8_t r[48];
        int i = 0;
        int mv = power_battery_mv();
        const char *v = MOINK_VERSION;
        uint8_t vl = (uint8_t)strlen(v);

        r[i++] = MOINK_API_VERSION;
        r[i++] = s_state;
        r[i++] = cfg->panel;
        r[i++] = (uint8_t)(mv & 0xFF);
        r[i++] = (uint8_t)((mv >> 8) & 0xFF);
        wr32(r + i, (uint32_t)(esp_timer_get_time() / 1000000)); i += 4;
        wr32(r + i, esp_get_free_heap_size()); i += 4;
        wr16(r + i, cur_mtu()); i += 2;
        r[i++] = s_wake_reason;
        wr32(r + i, cfg->sleep_s); i += 4;
        r[i++] = cfg->timeout_action;
        wr32(r + i, cfg->wake_s); i += 4;
        r[i++] = cache_has() ? 1 : 0;
        r[i++] = vl;
        memcpy(r + i, v, vl); i += vl;
        respond(seq, ST_OK, r, (uint8_t)i);
        break;
    }

    case OP_GET_SETTINGS: {
        const moink_settings_t *cfg = settings_get();
        uint8_t r[48];
        int i = 0;
        uint8_t nl = (uint8_t)strlen(cfg->name);

        r[i++] = cfg->panel;
        wr32(r + i, cfg->sleep_s); i += 4;
        r[i++] = cfg->timeout_action;
        wr32(r + i, cfg->wake_s); i += 4;
        r[i++] = cfg->gpio_preset;
        for (int k = 0; k < PIN_IDX_COUNT; k++) r[i++] = cfg->pins.v[k];
        r[i++] = cfg->cache_on;
        r[i++] = nl;
        memcpy(r + i, cfg->name, nl); i += nl;
        r[i++] = cfg->wake_show;
        respond(seq, ST_OK, r, (uint8_t)i);
        break;
    }

    case OP_SET_SETTINGS:
        handle_set_settings(seq, a, alen);
        break;

    case OP_FRAME_BEGIN:
        handle_frame_begin(seq, a, alen);
        break;

    case OP_FRAME_END:
        handle_frame_end(seq);
        break;

    case OP_FRAME_ABORT:
        if (s_state == BLE_ST_RECV_FRAME) {
            s_state = BLE_ST_IDLE;
            transfer_reset();
            ESP_LOGI(TAG, "frame abort by host");
            respond(seq, ST_OK, NULL, 0);
        } else {
            respond(seq, ST_BAD_STATE, NULL, 0);
        }
        break;

    case OP_CACHE_SHOW: {
        if (s_state != BLE_ST_IDLE) { respond(seq, ST_BUSY, NULL, 0); break; }
        if (!cache_has()) { respond(seq, ST_NOT_READY, NULL, 0); break; }
        ble_job_t job = { .type = BLE_JOB_CACHE };
        respond(seq, queue_job(&job, BLE_ST_REFRESHING) ? ST_OK : ST_BUSY, NULL, 0);
        break;
    }

    case OP_STORE_INFO: {
        if (s_state != BLE_ST_IDLE) { respond(seq, ST_BUSY, NULL, 0); break; }
        int count = 0, cur = -1;
        store_meta_get(&count, &cur);
        uint8_t r[7] = { count, cur < 0 ? 255 : cur, store_slots() };
        wr32(r + 3, store_revision());
        respond(seq, ST_OK, r, sizeof(r));
        break;
    }
    case OP_STORE_SHOW: {
        if (s_state != BLE_ST_IDLE) { respond(seq, ST_BUSY, NULL, 0); break; }
        if (alen != 1) { respond(seq, ST_BAD_ARG, NULL, 0); break; }
        int count = 0; store_meta_get(&count, NULL);
        if (!count) { respond(seq, ST_NOT_READY, NULL, 0); break; }
        if (a[0] != 255 && a[0] >= count) { respond(seq, ST_BAD_ARG, NULL, 0); break; }
        respond(seq, queue_carousel(a[0]) ? ST_OK : ST_BUSY, NULL, 0);
        break;
    }
    case OP_STORE_DELETE:
    case OP_STORE_CLEAR: {
        if (s_state != BLE_ST_IDLE) { respond(seq, ST_BUSY, NULL, 0); break; }
        if ((op == OP_STORE_DELETE && alen != 1) || (op == OP_STORE_CLEAR && alen != 0)) {
            respond(seq, ST_BAD_ARG, NULL, 0); break;
        }
        if (!store_slots()) { respond(seq, ST_NOT_READY, NULL, 0); break; }
        int count = 0; store_meta_get(&count, NULL);
        if (op == OP_STORE_DELETE && a[0] >= count) { respond(seq, ST_BAD_ARG, NULL, 0); break; }
        ble_job_t job = { .type = op == OP_STORE_DELETE ? BLE_JOB_STORE_DELETE : BLE_JOB_STORE_CLEAR,
                          .slot = op == OP_STORE_DELETE ? a[0] : 255 };
        respond(seq, queue_job(&job, BLE_ST_REFRESHING) ? ST_OK : ST_BUSY, NULL, 0);
        break;
    }
    case OP_STORE_LIST: handle_store_list(seq, a, alen); break;
    case OP_STORE_READ: handle_store_read(seq, a, alen); break;

    case OP_CACHE_INFO: {
        uint8_t r[12];
        uint16_t w = 0, h = 0, crc = 0;
        uint32_t len = 0;
        bool ok = (cache_info(&w, &h, &crc, &len) == ESP_OK);
        r[0] = ok ? 1 : 0;
        wr16(r + 1, w);
        wr16(r + 3, h);
        wr16(r + 5, crc);
        wr32(r + 7, len);
        respond(seq, ST_OK, r, 11);
        break;
    }

    case OP_GHOST_CLEAR: {
        if (s_state != BLE_ST_IDLE) { respond(seq, ST_BUSY, NULL, 0); break; }
        uint8_t cycles = (alen >= 1) ? a[0] : 2;
        if (cycles < 1 || cycles > 4) { respond(seq, ST_BAD_ARG, NULL, 0); break; }
        ble_job_t job = { .type = BLE_JOB_CLEAR, .cycles = cycles };
        s_state = BLE_ST_CLEARING;
        if (xQueueSend(s_job_q, &job, 0) != pdTRUE) {
            s_state = BLE_ST_IDLE;
            respond(seq, ST_BUSY, NULL, 0);
            break;
        }
        respond(seq, ST_OK, NULL, 0);
        break;
    }

    case OP_OTA_BEGIN:
        handle_ota_begin(seq, a, alen);
        break;

    case OP_OTA_END:
        handle_ota_end(seq);
        break;

    case OP_OTA_ABORT:
        if (s_state == BLE_ST_RECV_OTA) {
            s_state = BLE_ST_IDLE;
            transfer_reset();
            ESP_LOGW(TAG, "ota abort by host (slot unchanged)");
            respond(seq, ST_OK, NULL, 0);
        } else {
            respond(seq, ST_BAD_STATE, NULL, 0);
        }
        break;

    case OP_REBOOT: {
        if (s_state != BLE_ST_IDLE) { respond(seq, ST_BUSY, NULL, 0); break; }
        uint8_t delay_s = alen >= 1 ? a[0] : 1;
        if (delay_s > 10) delay_s = 10;
        s_state = BLE_ST_SLEEPING;
        if (xTaskCreate(reboot_task, "reboot", 2048,
                        (void *)(uintptr_t)(delay_s ? delay_s * 1000u : 200u), 5, NULL) != pdPASS) {
            s_state = BLE_ST_IDLE;
            respond(seq, ST_NO_MEM, NULL, 0); break;
        }
        respond(seq, ST_OK, NULL, 0);
        break;
    }
    case OP_FACTORY: {
        if (s_state != BLE_ST_IDLE) { respond(seq, ST_BUSY, NULL, 0); break; }
        s_state = BLE_ST_SLEEPING;
        respond(seq, ST_OK, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(400));
        settings_factory_reset();
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
        break;
    }
    case OP_SLEEP_NOW: {
        esp_ota_img_states_t ota_state;
        const esp_partition_t *running = esp_ota_get_running_partition();
        if (running && esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
            ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            respond(seq, ST_BUSY, NULL, 0); break;
        }
        if (s_state != BLE_ST_IDLE) { respond(seq, ST_BUSY, NULL, 0); break; }
        s_state = BLE_ST_SLEEPING;
        respond(seq, ST_OK, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(300));
        ble_api_stop();
        power_enter_deep_sleep();
        break;
    }

    default:
        ESP_LOGW(TAG, "unknown op 0x%02X", op);
        respond(seq, ST_UNKNOWN_OP, NULL, 0);
        break;
    }
}

static void on_data(const uint8_t *p, uint16_t n)
{
    if (n < 3) return;

    uint16_t seq = rd16(p);
    const uint8_t *d = p + 2;
    uint16_t dlen = (uint16_t)(n - 2);

    s_last_rx_ms = esp_timer_get_time() / 1000;
    power_activity();

    if (s_state == BLE_ST_RECV_FRAME || s_state == BLE_ST_RECV_OTA) {
        bool frame = s_state == BLE_ST_RECV_FRAME;
        uint32_t received = frame ? s_recv : s_ota_written;
        uint32_t total = frame ? s_framelen + s_thumb_len : s_ota_total;
        if (!frame && s_ota_err != ST_OK) return; /* report the latched error at END */
        uint32_t off = (uint32_t)seq * s_chunk;
        uint32_t remaining = total - received;
        uint16_t expected_len = remaining < s_chunk ? remaining : s_chunk;
        if (seq != s_expect || off != received || !expected_len || dlen != expected_len ||
            (!frame && !s_ota_part)) {
            if (!s_gap_notified) {
                uint8_t r[2]; wr16(r, s_expect);
                ble_api_notify_event(EVT_TRANSFER_GAP, 0, r, 2);
                s_gap_notified = true;
            }
            return;
        }
        if (frame) {
            uint16_t image_bytes = 0;
            if (s_recv < s_framelen) {
                uint32_t image_remaining = s_framelen - s_recv;
                image_bytes = dlen < image_remaining ? dlen : image_remaining;
                memcpy(s_fb + s_recv, d, image_bytes);
                s_crc_calc = crc16_update(s_crc_calc, d, image_bytes);
            }
            if (dlen > image_bytes) {
                uint16_t thumb_bytes = dlen - image_bytes;
                uint32_t thumb_off = s_recv + image_bytes - s_framelen;
                memcpy(s_thumb + thumb_off, d + image_bytes, thumb_bytes);
                s_thumb_crc_calc = crc16_update(s_thumb_crc_calc, d + image_bytes, thumb_bytes);
            }
            s_recv += dlen;
        } else {
            if (ota_flash_write(s_ota_written, d, dlen) != ESP_OK) {
                ESP_LOGE(TAG, "ota flash write failed @%lu", (unsigned long)s_ota_written);
                s_ota_err = ST_FLASH_ERROR;
                return;
            }
            s_ota_crc_calc = crc32_update(s_ota_crc_calc, d, dlen);
            s_ota_written += dlen;
        }
        s_expect++;
        s_gap_notified = false;
        return;
    }

    /* 其它状态：丢弃（主机应等状态机回到 idle） */
}

/* ---------------- GATT ---------------- */

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    uint16_t n = OS_MBUF_PKTLEN(ctxt->om);
    if (n == 0) return 0;
    if (n > sizeof(s_rx)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (os_mbuf_copydata(ctxt->om, 0, n, s_rx) != 0) return BLE_ATT_ERR_UNLIKELY;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (attr_handle == s_ctl_handle)      on_ctrl(s_rx, n);
    else if (attr_handle == s_dat_handle) on_data(s_rx, n);
    xSemaphoreGive(s_lock);

    return 0;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_ctl_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &s_ctl_handle,
            },
            {
                .uuid = &s_dat_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &s_dat_handle,
            },
            {
                .uuid = &s_evt_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_evt_handle,
            },
            { 0 }
        },
    },
    { 0 }
};

/* ---------------- GAP ---------------- */

/* 前置声明：ble_api_restart_adv() 里要把它传给 ble_gap_adv_start()。 */
static int gap_event(struct ble_gap_event *event, void *arg);

static void make_name(char *out, size_t n)
{
    const moink_settings_t *cfg = settings_get();
    if (cfg->name[0]) {
        snprintf(out, n, "%s", cfg->name);
        return;
    }
    uint8_t mac[6] = { 0 };
    if (ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, mac, NULL) == 0) {
        snprintf(out, n, "MoInk-%02X%02X", mac[4], mac[5]);
    } else {
        snprintf(out, n, "MoInk");
    }
}

void ble_api_restart_adv(void)
{
    if (!s_running) return;

    char name[SETT_NAME_MAX];
    make_name(name, sizeof(name));
    ble_svc_gap_device_name_set(name);

    if (s_adv_on) {
        ble_gap_adv_stop();
        s_adv_on = false;
    }
    /* GAP name is updated immediately; advertise again after disconnect. */
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) return;

    /* 广播包只放 flags + 128-bit 服务 UUID（21 字节），名字放扫描响应里，
       否则 31 字节装不下 —— APP 必须用 active scan 才能拿到名字。 */
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&s_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (const uint8_t *)name;
    rsp.name_len = (uint8_t)strlen(name);
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = BLE_GAP_ADV_ITVL_MS(100);
    params.itvl_max = BLE_GAP_ADV_ITVL_MS(150);

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start rc=%d", rc);
        return;
    }
    s_adv_on = true;
    ESP_LOGI(TAG, "advertising as '%s'", name);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_adv_on = false;
            power_activity();
            ESP_LOGI(TAG, "connected (conn=%u)", s_conn);
            ble_api_notify_event(EVT_STATE, s_state, NULL, 0);
        } else {
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            ble_api_restart_adv();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnect reason=0x%02X", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        /* Serialize disconnect against worker and timer buffer ownership. */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_state == BLE_ST_RECV_FRAME || s_state == BLE_ST_RECV_OTA) {
            transfer_reset();
            s_state = BLE_ST_IDLE;
        }
        xSemaphoreGive(s_lock);
        ble_api_restart_adv();
        power_activity();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu updated to %u (chunk max %u)",
                 event->mtu.value, max_chunk());
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_adv_on = false;
        if (s_running && s_conn == BLE_HS_CONN_HANDLE_NONE) ble_api_restart_adv();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe attr=%u notify=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        return 0;

    default:
        return 0;
    }
}

/* ---------------- 生命周期 ---------------- */

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "host reset, reason=%d", reason);
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0) {
        ESP_LOGE(TAG, "no usable BLE address");
        return;
    }
    uint8_t addr_type = 0;
    ble_hs_id_infer_auto(0, &addr_type);
    ble_api_restart_adv();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void housekeeping(void *arg)
{
    (void)arg;
    /* Timer task must never reset/free a buffer while host DATA is using it.
     * A busy callback simply defers timeout inspection until the next tick. */
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) return;
    int64_t now = esp_timer_get_time() / 1000;
    int limit = s_state == BLE_ST_RECV_FRAME ? FRAME_IDLE_ABORT_MS :
                s_state == BLE_ST_RECV_OTA ? OTA_IDLE_ABORT_MS : 0;
    if (limit && now - s_last_rx_ms > limit) {
        ESP_LOGW(TAG, "transfer receive timeout, abort");
        transfer_reset();
        s_state = BLE_ST_IDLE;
    }
    xSemaphoreGive(s_lock);
}

void ble_api_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) abort();
    s_job_q = xQueueCreate(2, sizeof(ble_job_t));
    if (!s_job_q) {
        ESP_LOGE(TAG, "job queue alloc failed");
        abort();
    }

    s_fb = malloc(EPD_MAX_BUF_LEN);
    if (!s_fb) {
        ESP_LOGE(TAG, "frame buffer alloc failed (%d B)", EPD_MAX_BUF_LEN);
        abort();
    }

    nimble_port_init();

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb  = on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(s_svcs);
    ble_gatts_add_svcs(s_svcs);

    int rc = ble_att_set_preferred_mtu(MAX_ATT_MTU);
    if (rc != 0) ESP_LOGW(TAG, "preferred mtu set rc=%d", rc);

    s_running = true;
    transfer_reset();
    s_last_rx_ms = esp_timer_get_time() / 1000;

    const esp_timer_create_args_t targs = {
        .callback = housekeeping,
        .name = "ble_house",
    };
    esp_timer_handle_t th = NULL;
    if (esp_timer_create(&targs, &th) == ESP_OK) {
        esp_timer_start_periodic(th, 1000000ULL);
    }

    nimble_port_freertos_init(host_task);

    ESP_LOGI(TAG, "BLE stack up (%s, api %d)", MOINK_VERSION, MOINK_API_VERSION);
}

void ble_api_stop(void)
{
    s_running = false;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
    }
    if (s_adv_on) {
        ble_gap_adv_stop();
        s_adv_on = false;
    }
}

/* ---------------- 与 display_task 的接口 ---------------- */

bool ble_api_job_wait(ble_job_t *job, uint32_t timeout_ms)
{
    return xQueueReceive(s_job_q, job,
                         timeout_ms == 0 ? 0 : pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void ble_api_job_complete(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    transfer_reset();
    power_activity();
    s_state = BLE_ST_IDLE;
    xSemaphoreGive(s_lock);
}

void ble_api_job_finish(uint8_t result)
{
    ble_api_job_complete();
    ble_api_notify_event(EVT_DISPLAY_DONE, result, NULL, 0);
}

bool ble_api_request_cache_show(void)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ble_job_t job = { .type = BLE_JOB_CACHE };
    bool ok = cache_has() && queue_job(&job, BLE_ST_REFRESHING);
    xSemaphoreGive(s_lock);
    return ok;
}

bool ble_api_request_carousel_show(uint8_t slot)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = queue_carousel(slot);
    xSemaphoreGive(s_lock);
    return ok;
}

bool ble_api_prepare_sleep(void)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_state == BLE_ST_IDLE;
    if (ok) s_state = BLE_ST_SLEEPING;
    xSemaphoreGive(s_lock);
    return ok;
}

/* A read-only command can refresh activity without changing IDLE. Recheck the
 * timeout while excluding host commands, rather than using an earlier sample
 * from idle_task before it acquired the state claim. */
bool ble_api_prepare_idle_sleep(void)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_state == BLE_ST_IDLE && power_should_sleep();
    if (ok) s_state = BLE_ST_SLEEPING;
    xSemaphoreGive(s_lock);
    return ok;
}
