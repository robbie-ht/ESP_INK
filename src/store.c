#include "store.h"

#include <string.h>
#include "crc_util.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

/* All persistent integers are explicitly little-endian. Neither structure
 * padding nor the compiler ABI becomes part of the flash format. */
#define HEADER_SIZE STORE_HEADER_SZ
#define META_SIZE   24u
#define META_NS     "app_gallery"
#define META_KEY    "meta_v1"
#define FORMAT_VERSION 1u

typedef struct {
    uint8_t count;
    int8_t current;
    uint8_t order[STORE_SLOT_MAX];
    uint32_t revision;
} meta_t;

static const esp_partition_t *s_part;
static SemaphoreHandle_t s_lock;
static unsigned s_slots;
static bool s_ready;
static bool s_writes_blocked;
static meta_t s_meta;

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static esp_err_t lock_store(void)
{
    if (!s_lock || !s_ready) return ESP_ERR_INVALID_STATE;
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(5000)) == pdTRUE
        ? ESP_OK : ESP_ERR_TIMEOUT;
}

static uint32_t next_revision(uint32_t revision)
{
    /* Zero is reserved for an unavailable/uninitialized gallery. */
    return revision == UINT32_MAX ? 1 : revision + 1;
}

static esp_err_t lock_writable(void)
{
    esp_err_t err = lock_store();
    if (err == ESP_OK && s_writes_blocked) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    return err;
}

static esp_err_t persist_meta(const meta_t *meta)
{
    uint8_t blob[META_SIZE] = { 'M', 'G', 'M', '1', FORMAT_VERSION };
    blob[5] = meta->count;
    blob[6] = meta->current < 0 ? 0xff : (uint8_t)meta->current;
    memcpy(blob + 8, meta->order, STORE_SLOT_MAX);
    put32(blob + 16, meta->revision);
    put32(blob + 20, crc32_of(blob, 20));
    nvs_handle_t handle;
    esp_err_t err = nvs_open(META_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, META_KEY, blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    /* Publish the complete new metadata only after persistence succeeds. */
    if (err == ESP_OK) s_meta = *meta;
    /* An error is not proof the new blob was never made durable. Do not reuse
     * slots using an older RAM order until startup reloads authoritative NVS;
     * otherwise a retry could erase a now-referenced slot before a power cut. */
    else s_writes_blocked = true;
    return err;
}

static esp_err_t decode_meta(const uint8_t *blob, meta_t *meta)
{
    if (memcmp(blob, "MGM1", 4) || blob[4] != FORMAT_VERSION ||
        get32(blob + 20) != crc32_of(blob, 20)) return ESP_ERR_INVALID_CRC;
    if (blob[5] > s_slots ||
        (blob[6] != 0xff && blob[6] >= blob[5]) || !get32(blob + 16))
        return ESP_ERR_INVALID_SIZE;
    uint8_t used = 0;
    for (unsigned i = 0; i < blob[5]; ++i) {
        unsigned physical = blob[8 + i];
        if (physical >= s_slots || (used & (1u << physical)))
            return ESP_ERR_INVALID_SIZE;
        used |= (uint8_t)(1u << physical);
    }
    meta->count = blob[5];
    meta->current = blob[6] == 0xff ? -1 : (int8_t)blob[6];
    memcpy(meta->order, blob + 8, STORE_SLOT_MAX);
    meta->revision = get32(blob + 16);
    return ESP_OK;
}

esp_err_t store_init(void)
{
    /* Startup-only API; repeated calls are also useful after a controlled NVS
     * reinitialization. Existing data is never inferred from orphan headers. */
    s_ready = false;
    s_writes_blocked = false;
    s_slots = 0;
    memset(&s_meta, 0, sizeof(s_meta));
    s_meta.current = -1;
    s_meta.revision = 1;
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      ESP_PARTITION_SUBTYPE_ANY, "store");
    if (!s_part) return ESP_ERR_NOT_FOUND;
    s_slots = s_part->size / STORE_SLOT_SZ;
    if (s_slots > STORE_SLOT_MAX) s_slots = STORE_SLOT_MAX;
    if (!s_slots || s_part->encrypted) {
        s_slots = 0;
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) { s_slots = 0; return ESP_ERR_NO_MEM; }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(META_NS, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) { s_ready = true; return ESP_OK; }
    if (err != ESP_OK) { s_slots = 0; return err; }
    uint8_t blob[META_SIZE];
    size_t len = sizeof(blob);
    err = nvs_get_blob(handle, META_KEY, blob, &len);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    else if (err == ESP_OK) {
        err = len == META_SIZE ? decode_meta(blob, &s_meta) : ESP_ERR_INVALID_SIZE;
    }
    if (err != ESP_OK) { s_slots = 0; return err; }
    s_ready = true;
    return ESP_OK;
}

int store_slots(void)
{
    return s_ready ? (int)s_slots : 0;
}

void store_meta_get(int *count, int *cur)
{
    if (count) *count = 0;
    if (cur) *cur = -1;
    if (lock_store() != ESP_OK) return;
    if (count) *count = s_meta.count;
    if (cur) *cur = s_meta.current;
    xSemaphoreGive(s_lock);
}

uint32_t store_revision(void)
{
    if (lock_store() != ESP_OK) return 0;
    uint32_t revision = s_meta.revision;
    xSemaphoreGive(s_lock);
    return revision;
}

static bool valid_frame(uint16_t w, uint16_t h)
{
    return w && h && w <= 800 && h <= 600 && !(w & 15u) && !(h & 3u);
}

static bool valid_thumb(const store_info_t *info)
{
    if (!info->thumb_w || !info->thumb_h || !info->thumb_len) return false;
    if (info->thumb_format == STORE_THUMB_JPEG)
        return info->thumb_len <= STORE_THUMB_JPEG_MAX;
    return info->thumb_format == STORE_THUMB_PACKED &&
        !(info->thumb_w & 3u) && info->thumb_w <= 200 && info->thumb_h <= 150 &&
        info->thumb_len == (uint32_t)(info->thumb_w / 4u) * info->thumb_h;
}

static void make_header(const store_info_t *info, uint8_t *header)
{
    memset(header, 0, HEADER_SIZE);
    memcpy(header, "MGL1", 4);
    header[4] = FORMAT_VERSION;
    header[5] = info->thumb_format;
    put16(header + 6, info->w);
    put16(header + 8, info->h);
    put16(header + 10, info->crc16);
    put32(header + 12, info->len);
    put16(header + 16, info->thumb_w);
    put16(header + 18, info->thumb_h);
    put32(header + 20, info->thumb_len);
    put16(header + 24, info->thumb_crc16);
    put32(header + 60, crc32_of(header, 60));
}

/* Caller holds the mutex; the NVS order is the only authority for membership. */
static esp_err_t read_header(int slot, store_info_t *info, uint32_t *base)
{
    if (slot < 0 || slot >= s_meta.count) return ESP_ERR_NOT_FOUND;
    *base = s_meta.order[slot] * STORE_SLOT_SZ;
    uint8_t header[HEADER_SIZE];
    esp_err_t err = esp_partition_read(s_part, *base, header, sizeof(header));
    if (err != ESP_OK) return err;
    if (memcmp(header, "MGL1", 4) || header[4] != FORMAT_VERSION ||
        get32(header + 60) != crc32_of(header, 60)) return ESP_ERR_INVALID_CRC;
    memset(info, 0, sizeof(*info));
    info->thumb_format = header[5];
    info->w = get16(header + 6);
    info->h = get16(header + 8);
    info->crc16 = get16(header + 10);
    info->len = get32(header + 12);
    info->thumb_w = get16(header + 16);
    info->thumb_h = get16(header + 18);
    info->thumb_len = get32(header + 20);
    info->thumb_crc16 = get16(header + 24);
    if (!valid_frame(info->w, info->h) ||
        info->len != (uint32_t)(info->w / 4u) * info->h ||
        info->len > STORE_FRAME_MAX || !valid_thumb(info) ||
        info->thumb_len > STORE_SLOT_SZ - HEADER_SIZE - info->len)
        return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

esp_err_t store_info(int slot, store_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    esp_err_t err = lock_store();
    if (err != ESP_OK) return err;
    uint32_t base;
    err = read_header(slot, info, &base);
    xSemaphoreGive(s_lock);
    return err;
}

static esp_err_t verify_payload(uint32_t offset, uint32_t len, uint16_t expected)
{
    uint8_t chunk[512];
    uint16_t crc = 0xffff;
    while (len) {
        size_t n = len < sizeof(chunk) ? len : sizeof(chunk);
        esp_err_t err = esp_partition_read(s_part, offset, chunk, n);
        if (err != ESP_OK) return err;
        crc = crc16_update(crc, chunk, n);
        offset += n;
        len -= n;
    }
    return crc == expected ? ESP_OK : ESP_ERR_INVALID_CRC;
}

/* The packed stream itself is rotated 180 degrees. Taking successive 4x4
 * groups in that stream preserves the same orientation in the output; no
 * unpacked image and no second full-frame buffer are required. */
static esp_err_t write_generated_thumb(uint32_t offset, const uint8_t *data,
                                      store_info_t *info)
{
    uint8_t row[50];
    unsigned source_stride = info->w / 4u;
    unsigned thumb_stride = info->thumb_w / 4u;
    uint16_t crc = 0xffff;
    for (unsigned ty = 0; ty < info->thumb_h; ++ty) {
        memset(row, 0, thumb_stride);
        for (unsigned tx = 0; tx < info->thumb_w; ++tx) {
            unsigned votes[4] = {0};
            for (unsigned dy = 0; dy < 4; ++dy) {
                uint8_t pixels = data[(ty * 4u + dy) * source_stride + tx];
                for (unsigned dx = 0; dx < 4; ++dx)
                    ++votes[(pixels >> (6u - 2u * dx)) & 3u];
            }
            unsigned best = 0;
            for (unsigned color = 1; color < 4; ++color)
                if (votes[color] > votes[best]) best = color;
            row[tx / 4u] |= (uint8_t)(best << (6u - 2u * (tx & 3u)));
        }
        esp_err_t err = esp_partition_write(s_part, offset, row, thumb_stride);
        if (err != ESP_OK) return err;
        crc = crc16_update(crc, row, thumb_stride);
        offset += thumb_stride;
    }
    info->thumb_crc16 = crc;
    return ESP_OK;
}

esp_err_t store_save_frame(const uint8_t *data, uint16_t w, uint16_t h,
                           uint16_t crc16, const uint8_t *thumb,
                           uint32_t thumb_len, uint8_t thumb_format,
                           uint16_t thumb_w, uint16_t thumb_h, int *slot)
{
    if (slot) *slot = -1;
    if (!data || !valid_frame(w, h) || (!thumb && thumb_len) ||
        (thumb && !thumb_len)) return ESP_ERR_INVALID_ARG;
    store_info_t info = {
        .w = w, .h = h, .crc16 = crc16, .len = (uint32_t)(w / 4u) * h,
        .thumb_format = thumb ? thumb_format : STORE_THUMB_PACKED,
        .thumb_w = thumb ? thumb_w : w / 4u,
        .thumb_h = thumb ? thumb_h : h / 4u,
        .thumb_len = thumb ? thumb_len : (uint32_t)(w / 16u) * (h / 4u),
    };
    if (info.len > STORE_FRAME_MAX || !valid_thumb(&info) ||
        info.thumb_len > STORE_SLOT_SZ - HEADER_SIZE - info.len)
        return ESP_ERR_INVALID_SIZE;
    if (crc16_of(data, info.len) != crc16) return ESP_ERR_INVALID_CRC;
    if (thumb) info.thumb_crc16 = crc16_of(thumb, thumb_len);
    esp_err_t err = lock_writable();
    if (err != ESP_OK) return err;
    if (s_meta.count >= s_slots) { err = STORE_ERR_FULL; goto done; }

    uint8_t used = 0;
    for (unsigned i = 0; i < s_meta.count; ++i)
        used |= (uint8_t)(1u << s_meta.order[i]);
    unsigned physical = 0;
    while (used & (1u << physical)) ++physical;
    uint32_t base = physical * STORE_SLOT_SZ;
    err = esp_partition_erase_range(s_part, base, STORE_SLOT_SZ);
    if (err != ESP_OK) goto done;
    err = esp_partition_write(s_part, base + HEADER_SIZE, data, info.len);
    if (err != ESP_OK) goto done;
    uint32_t thumb_offset = base + HEADER_SIZE + info.len;
    if (thumb) err = esp_partition_write(s_part, thumb_offset, thumb, thumb_len);
    else err = write_generated_thumb(thumb_offset, data, &info);
    if (err != ESP_OK) goto done;
    err = verify_payload(base + HEADER_SIZE, info.len, info.crc16);
    if (err != ESP_OK) goto done;
    err = verify_payload(thumb_offset, info.thumb_len, info.thumb_crc16);
    if (err != ESP_OK) goto done;

    /* A partial body never has a valid header. Membership is published only
     * after a complete, verified body and header exist. Startup ignores orphan
     * headers that are not referenced by the atomic NVS metadata blob. */
    uint8_t header[HEADER_SIZE];
    make_header(&info, header);
    err = esp_partition_write(s_part, base, header, sizeof(header));
    if (err != ESP_OK) goto done;
    uint8_t check[HEADER_SIZE];
    err = esp_partition_read(s_part, base, check, sizeof(check));
    if (err != ESP_OK) goto done;
    if (memcmp(header, check, sizeof(check))) { err = ESP_ERR_INVALID_CRC; goto done; }
    meta_t next = s_meta;
    next.order[next.count++] = (uint8_t)physical;
    next.revision = next_revision(next.revision);
    err = persist_meta(&next);
    if (err == ESP_OK && slot) *slot = next.count - 1;
done:
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t store_load_frame(int slot, uint8_t *out, size_t cap,
                           uint16_t *w, uint16_t *h)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    esp_err_t err = lock_store();
    if (err != ESP_OK) return err;
    store_info_t info;
    uint32_t base;
    err = read_header(slot, &info, &base);
    if (err == ESP_OK && cap < info.len) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) err = esp_partition_read(s_part, base + HEADER_SIZE, out, info.len);
    if (err == ESP_OK && crc16_of(out, info.len) != info.crc16) err = ESP_ERR_INVALID_CRC;
    if (err == ESP_OK) {
        if (w) *w = info.w;
        if (h) *h = info.h;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t store_read(int slot, bool thumbnail, uint32_t offset,
                     void *out, size_t len)
{
    if (!out && len) return ESP_ERR_INVALID_ARG;
    esp_err_t err = lock_store();
    if (err != ESP_OK) return err;
    store_info_t info;
    uint32_t base;
    err = read_header(slot, &info, &base);
    if (err == ESP_OK) {
        uint32_t payload_len = thumbnail ? info.thumb_len : info.len;
        uint32_t payload_start = base + HEADER_SIZE + (thumbnail ? info.len : 0);
        if (offset > payload_len || len > payload_len - offset) err = ESP_ERR_INVALID_SIZE;
        else {
            if (!offset) err = verify_payload(payload_start, payload_len,
                                              thumbnail ? info.thumb_crc16 : info.crc16);
            if (err == ESP_OK && len) err = esp_partition_read(s_part, payload_start + offset, out, len);
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t store_set_current(int slot)
{
    esp_err_t err = lock_writable();
    if (err != ESP_OK) return err;
    if (slot < -1 || slot >= s_meta.count) err = ESP_ERR_NOT_FOUND;
    else if (slot != s_meta.current) {
        meta_t next = s_meta;
        next.current = (int8_t)slot;
        err = persist_meta(&next);
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t store_delete(int slot)
{
    esp_err_t err = lock_writable();
    if (err != ESP_OK) return err;
    if (slot < 0 || slot >= s_meta.count) err = ESP_ERR_NOT_FOUND;
    else {
        meta_t next = s_meta;
        memmove(next.order + slot, next.order + slot + 1, next.count - slot - 1);
        next.order[--next.count] = 0;
        if (next.current == slot) next.current = -1;
        else if (next.current > slot) --next.current;
        next.revision = next_revision(next.revision);
        err = persist_meta(&next);
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t store_clear(void)
{
    esp_err_t err = lock_writable();
    if (err != ESP_OK) return err;
    if (s_meta.count) {
        meta_t next = { .current = -1, .revision = next_revision(s_meta.revision) };
        err = persist_meta(&next);
    }
    xSemaphoreGive(s_lock);
    return err;
}
