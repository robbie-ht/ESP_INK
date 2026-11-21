#include "cache.h"
#include "crc_util.h"

#include "esp_partition.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "cache";

#define CACHE_LABEL      "fcache"
#define CACHE_HDR_MAGIC  0xC53A
#define CACHE_VER        1
#define CACHE_HDR_OFF    0x0000
#define CACHE_PAYLOAD_OFF 0x1000
#define SECTOR           0x1000

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  ver;
    uint8_t  flags;
    uint16_t w;
    uint16_t h;
    uint32_t len;
    uint16_t crc16;
    uint16_t rsvd;
} cache_hdr_t;   /* 16 字节 */

static const esp_partition_t *s_part;
static cache_hdr_t s_hdr;       /* RAM 里的头镜像 */
static bool s_hdr_ok;

esp_err_t cache_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                      CACHE_LABEL);
    if (!s_part) {
        ESP_LOGW(TAG, "partition '%s' not found (old table?), cache disabled",
                 CACHE_LABEL);
        s_hdr_ok = false;
        return ESP_ERR_NOT_FOUND;
    }
    if (s_part->size < CACHE_PAYLOAD_OFF + EPD_CACHE_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "partition too small (%lu B), cache disabled",
                 (unsigned long)s_part->size);
        s_part = NULL;
        s_hdr_ok = false;
        return ESP_ERR_INVALID_SIZE;
    }

    cache_hdr_t h;
    if (esp_partition_read(s_part, CACHE_HDR_OFF, &h, sizeof(h)) != ESP_OK) {
        s_hdr_ok = false;
        return ESP_FAIL;
    }
    if (h.magic == CACHE_HDR_MAGIC && h.ver == CACHE_VER &&
        h.len > 0 && h.len <= EPD_CACHE_MAX_PAYLOAD &&
        h.w == 768 && h.h == 552 && h.len == (uint32_t)(h.w / 4) * h.h) {
        s_hdr = h;
        s_hdr_ok = true;
        ESP_LOGI(TAG, "cache valid: %ux%u %lu B crc16=0x%04X",
                 h.w, h.h, (unsigned long)h.len, h.crc16);
        return ESP_OK;
    }

    s_hdr_ok = false;
    ESP_LOGI(TAG, "cache empty");
    return ESP_OK;
}

bool cache_has(void) { return s_part && s_hdr_ok; }

esp_err_t cache_info(uint16_t *w, uint16_t *h, uint16_t *crc16, uint32_t *len)
{
    if (!cache_has()) return ESP_ERR_NOT_FOUND;
    if (w) *w = s_hdr.w;
    if (h) *h = s_hdr.h;
    if (crc16) *crc16 = s_hdr.crc16;
    if (len) *len = s_hdr.len;
    return ESP_OK;
}

/* 分段擦写，避免大块栈占用；src 必须在 RAM（帧缓冲）。 */
static esp_err_t write_region(const esp_partition_t *p, uint32_t off,
                              const uint8_t *src, uint32_t len)
{
    static uint8_t buf[SECTOR];
    uint32_t done = 0;

    while (done < len) {
        uint32_t at = off + done;
        if ((at & (SECTOR - 1)) == 0) {
            if (esp_partition_erase_range(p, at, SECTOR) != ESP_OK) {
                ESP_LOGE(TAG, "erase @0x%lx failed", (unsigned long)at);
                return ESP_FAIL;
            }
        }
        uint32_t room = SECTOR - (at & (SECTOR - 1));
        uint32_t n = (len - done < room) ? (len - done) : room;
        memcpy(buf, src + done, n);
        if (esp_partition_write(p, at, buf, n) != ESP_OK) {
            ESP_LOGE(TAG, "write @0x%lx failed", (unsigned long)at);
            return ESP_FAIL;
        }
        done += n;
    }
    return ESP_OK;
}

static esp_err_t header_erase(void)
{
    return esp_partition_erase_range(s_part, CACHE_HDR_OFF, SECTOR) == ESP_OK
           ? ESP_OK : ESP_FAIL;
}

static esp_err_t header_write(cache_hdr_t *h)
{
    h->magic = CACHE_HDR_MAGIC;
    h->ver = CACHE_VER;
    h->flags = 0;
    h->rsvd = 0;
    esp_err_t e = esp_partition_write(s_part, CACHE_HDR_OFF, h, sizeof(*h));
    return e == ESP_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t cache_save(const uint8_t *frame, uint16_t w, uint16_t h, uint16_t crc16)
{
    if (!s_part) return ESP_ERR_NOT_FOUND;
    if (!frame || w != 768 || h != 552) return ESP_ERR_INVALID_ARG;

    uint32_t len = (uint32_t)(w / 4) * h;
    if (len > EPD_CACHE_MAX_PAYLOAD) return ESP_ERR_INVALID_SIZE;

    /* 内容一样就不写：避免重复推同一张图时白擦 26 个扇区。 */
    if (s_hdr_ok && s_hdr.crc16 == crc16 && s_hdr.len == len &&
        s_hdr.w == w && s_hdr.h == h) {
        /* CRC16 alone can collide: compare persisted bytes before deduping. */
        uint8_t check[256];
        bool identical = true;
        for (uint32_t off = 0; off < len; off += sizeof(check)) {
            uint32_t n = len - off < sizeof(check) ? len - off : sizeof(check);
            if (esp_partition_read(s_part, CACHE_PAYLOAD_OFF + off, check, n) != ESP_OK ||
                memcmp(check, frame + off, n)) { identical = false; break; }
        }
        if (identical) {
            ESP_LOGI(TAG, "same frame, skip flash write");
            return ESP_OK;
        }
    }

    s_hdr_ok = false;                       /* 先让缓存失效（掉电安全） */
    if (header_erase() != ESP_OK) return ESP_FAIL;

    esp_err_t e = write_region(s_part, CACHE_PAYLOAD_OFF, frame, len);
    if (e != ESP_OK) return e;

    cache_hdr_t nh = {
        .w = w, .h = h, .len = len, .crc16 = crc16,
    };
    e = header_write(&nh);
    if (e != ESP_OK) return e;

    s_hdr = nh;
    s_hdr_ok = true;
    ESP_LOGI(TAG, "cache saved: %lu B crc16=0x%04X", (unsigned long)len, crc16);
    return ESP_OK;
}

esp_err_t cache_load(uint8_t *out, uint32_t cap,
                     uint16_t *w, uint16_t *h, uint16_t *crc16)
{
    if (!cache_has()) return ESP_ERR_NOT_FOUND;
    if (!out || cap < s_hdr.len) return ESP_ERR_INVALID_SIZE;

    if (esp_partition_read(s_part, CACHE_PAYLOAD_OFF, out, s_hdr.len) != ESP_OK)
        return ESP_FAIL;

    uint16_t got = crc16_of(out, s_hdr.len);
    if (got != s_hdr.crc16) {
        ESP_LOGE(TAG, "cache CRC mismatch (0x%04X != 0x%04X), drop",
                 got, s_hdr.crc16);
        s_hdr_ok = false;
        return ESP_ERR_INVALID_CRC;
    }

    if (w) *w = s_hdr.w;
    if (h) *h = s_hdr.h;
    if (crc16) *crc16 = s_hdr.crc16;
    return ESP_OK;
}

esp_err_t cache_clear(void)
{
    if (!s_part) return ESP_ERR_NOT_FOUND;
    s_hdr_ok = false;
    return header_erase();
}