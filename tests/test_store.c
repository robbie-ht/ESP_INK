#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "store.h"
#include "crc_util.h"
#include "esp_partition.h"
#include "nvs.h"
#include "freertos/semphr.h"

static uint8_t flash[STORE_SLOT_SZ * STORE_SLOT_MAX];
static uint8_t nvs_data[64], pending[64];
static size_t nvs_len, pending_len;
static bool have_partition = true, locked;
static int writes, erases, reads, commits;
static int fail_write, fail_erase, fail_read, fail_set, fail_commit;
static bool commit_error_is_durable;
static esp_partition_t partition = { .size = sizeof(flash), .encrypted = false };

const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label)
{
    assert(type == ESP_PARTITION_TYPE_DATA && subtype == ESP_PARTITION_SUBTYPE_ANY);
    assert(!strcmp(label, "store"));
    return have_partition ? &partition : NULL;
}

esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *out, size_t len)
{
    assert(p == &partition && off <= sizeof(flash) && len <= sizeof(flash) - off);
    ++reads;
    if (fail_read && reads == fail_read) return ESP_FAIL;
    memcpy(out, flash + off, len);
    return ESP_OK;
}

esp_err_t esp_partition_write(const esp_partition_t *p, size_t off, const void *data, size_t len)
{
    assert(p == &partition && off <= sizeof(flash) && len <= sizeof(flash) - off);
    ++writes;
    const uint8_t *source = data;
    size_t n = fail_write && writes == fail_write ? len / 2 : len;
    for (size_t i = 0; i < n; ++i) {
        assert((flash[off + i] & source[i]) == source[i]);
        flash[off + i] &= source[i];
    }
    return n == len ? ESP_OK : ESP_FAIL;
}

esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t len)
{
    assert(p == &partition && !(off % 4096) && !(len % 4096));
    assert(off <= sizeof(flash) && len <= sizeof(flash) - off);
    ++erases;
    if (fail_erase && erases == fail_erase) return ESP_FAIL;
    memset(flash + off, 0xff, len);
    return ESP_OK;
}

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *handle)
{
    assert(!strcmp(ns, "app_gallery"));
    if (mode == NVS_READONLY && !nvs_len) return ESP_ERR_NVS_NOT_FOUND;
    *handle = 1;
    pending_len = 0;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *len)
{
    assert(handle == 1 && !strcmp(key, "meta_v1"));
    if (!nvs_len) return ESP_ERR_NVS_NOT_FOUND;
    if (*len < nvs_len) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, nvs_data, nvs_len);
    *len = nvs_len;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t len)
{
    assert(handle == 1 && !strcmp(key, "meta_v1") && len <= sizeof(pending));
    if (fail_set) return ESP_FAIL;
    memcpy(pending, data, len);
    pending_len = len;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1);
    ++commits;
    if (fail_commit && !commit_error_is_durable) return ESP_FAIL;
    memcpy(nvs_data, pending, pending_len);
    nvs_len = pending_len;
    return fail_commit ? ESP_FAIL : ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle == 1);
    pending_len = 0;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &locked; }
int xSemaphoreTake(SemaphoreHandle_t lock, unsigned ticks)
{
    assert(lock == &locked && ticks == 5000);
    if (locked) return 0;
    locked = true;
    return 1;
}
int xSemaphoreGive(SemaphoreHandle_t lock)
{
    assert(lock == &locked && locked);
    locked = false;
    return 1;
}

static void fresh(void)
{
    memset(flash, 0xff, sizeof(flash));
    memset(nvs_data, 0, sizeof(nvs_data));
    nvs_len = pending_len = 0;
    writes = reads = erases = commits = 0;
    fail_write = fail_read = fail_erase = fail_set = fail_commit = 0;
    commit_error_is_durable = false;
    have_partition = true;
    partition.size = sizeof(flash);
    partition.encrypted = false;
    locked = false;
    assert(store_init() == ESP_OK);
}

static void meta(int expected_count, int expected_current)
{
    int count, current;
    store_meta_get(&count, &current);
    assert(count == expected_count && current == expected_current);
}

static esp_err_t save_small(uint8_t value, int *slot)
{
    uint8_t data[16];
    memset(data, value, sizeof(data));
    return store_save_frame(data, 16, 4, crc16_of(data, sizeof(data)), NULL, 0, 0, 0, 0, slot);
}

static void expect_small(int slot, uint8_t value)
{
    uint8_t frame[16];
    uint16_t w = 0, h = 0;
    assert(store_load_frame(slot, frame, sizeof(frame), &w, &h) == ESP_OK);
    assert(w == 16 && h == 4);
    for (unsigned i = 0; i < sizeof(frame); ++i) assert(frame[i] == value);
}

static void test_full_delete_append_reboot(void)
{
    fresh();
    assert(store_slots() == 8);
    meta(0, -1);
    for (int i = 0; i < 8; ++i) {
        int slot = -1;
        assert(save_small((uint8_t)i, &slot) == ESP_OK && slot == i);
    }
    meta(8, -1);
    int before_writes = writes, before_erases = erases;
    assert(save_small(9, NULL) == STORE_ERR_FULL);
    assert(writes == before_writes && erases == before_erases);
    uint32_t revision = store_revision();
    assert(store_set_current(6) == ESP_OK);
    assert(store_revision() == revision);
    assert(store_delete(2) == ESP_OK);
    meta(7, 5);
    assert(writes == before_writes && erases == before_erases);
    int slot;
    assert(save_small(99, &slot) == ESP_OK && slot == 7);
    for (int i = 0; i < 7; ++i) expect_small(i, (uint8_t)(i < 2 ? i : i + 1));
    expect_small(7, 99);
    meta(8, 5);
    assert(store_init() == ESP_OK);
    meta(8, 5);
    expect_small(7, 99);
    assert(store_delete(5) == ESP_OK);
    meta(7, -1);
    before_writes = writes; before_erases = erases;
    assert(store_clear() == ESP_OK);
    assert(writes == before_writes && erases == before_erases);
    meta(0, -1);
    assert(store_init() == ESP_OK);
    meta(0, -1);
    assert(store_load_frame(0, flash, 16, NULL, NULL) == ESP_ERR_NOT_FOUND);
    assert(save_small(50, &slot) == ESP_OK && slot == 0);
    expect_small(0, 50);
}

static void pack_pixel(uint8_t *data, unsigned w, unsigned h,
                       unsigned x, unsigned y, uint8_t color)
{
    unsigned index = (h - 1u - y) * w + (w - 1u - x);
    unsigned shift = 6u - 2u * (index & 3u);
    data[index / 4u] = (uint8_t)((data[index / 4u] & ~(3u << shift)) | (color << shift));
}

static void test_real_thumbnail_orientation(void)
{
    static uint8_t input[768 * 552 / 4], output[768 * 552 / 4];
    static uint8_t expected_thumb[192 * 138 / 4], actual_thumb[192 * 138 / 4];
    fresh();
    memset(input, 0, sizeof(input));
    memset(expected_thumb, 0, sizeof(expected_thumb));
    for (unsigned ty = 0; ty < 138; ++ty) for (unsigned tx = 0; tx < 192; ++tx) {
        uint8_t color = (uint8_t)((tx + 3 * ty) & 3);
        pack_pixel(expected_thumb, 192, 138, tx, ty, color);
        for (unsigned dy = 0; dy < 4; ++dy) for (unsigned dx = 0; dx < 4; ++dx) {
            /* Eleven majority pixels, five different minority pixels. */
            uint8_t source = dy * 4 + dx < 11 ? color : (uint8_t)((color + 1) & 3);
            pack_pixel(input, 768, 552, tx * 4 + dx, ty * 4 + dy, source);
        }
    }
    int slot;
    assert(store_save_frame(input, 768, 552, crc16_of(input, sizeof(input)),
                            NULL, 0, 0, 0, 0, &slot) == ESP_OK);
    assert(slot == 0);
    store_info_t info;
    assert(store_info(0, &info) == ESP_OK);
    assert(info.w == 768 && info.h == 552 && info.len == sizeof(input));
    assert(info.thumb_w == 192 && info.thumb_h == 138 && info.thumb_format == 1);
    assert(info.thumb_len == sizeof(expected_thumb));
    assert(store_read(0, true, 0, actual_thumb, sizeof(actual_thumb)) == ESP_OK);
    assert(!memcmp(expected_thumb, actual_thumb, sizeof(actual_thumb)));
    assert(info.thumb_crc16 == crc16_of(actual_thumb, sizeof(actual_thumb)));
    assert(store_load_frame(0, output, sizeof(output), NULL, NULL) == ESP_OK);
    assert(!memcmp(input, output, sizeof(input)));
    assert(store_read(0, true, info.thumb_len - 1, actual_thumb, 2) == ESP_ERR_INVALID_SIZE);
    assert(store_read(0, false, UINT32_MAX, actual_thumb, 1) == ESP_ERR_INVALID_SIZE);
    assert(store_load_frame(0, output, sizeof(output) - 1, NULL, NULL) == ESP_ERR_INVALID_SIZE);
}

static void test_corruption_and_input_checks(void)
{
    fresh();
    uint8_t input[16] = {0};
    uint8_t output[16];
    assert(store_save_frame(input, 16, 4, 0, NULL, 0, 0, 0, 0, NULL) == ESP_ERR_INVALID_CRC);
    assert(!writes && !erases);
    assert(store_save_frame(input, 15, 4, 0, NULL, 0, 0, 0, 0, NULL) == ESP_ERR_INVALID_ARG);
    assert(save_small(0x55, NULL) == ESP_OK);
    flash[STORE_HEADER_SZ + 5] ^= 1;
    assert(store_load_frame(0, output, sizeof(output), NULL, NULL) == ESP_ERR_INVALID_CRC);
    assert(store_read(0, false, 0, output, sizeof(output)) == ESP_ERR_INVALID_CRC);
    flash[STORE_HEADER_SZ + 5] ^= 1;
    flash[STORE_HEADER_SZ + 16] ^= 1;
    assert(store_read(0, true, 0, output, 1) == ESP_ERR_INVALID_CRC);
    flash[STORE_HEADER_SZ + 16] ^= 1;
    flash[6] ^= 1;
    store_info_t info;
    assert(store_info(0, &info) == ESP_ERR_INVALID_CRC);
    flash[6] ^= 1;
    assert(store_init() == ESP_OK);
    expect_small(0, 0x55);
    nvs_data[8] ^= 1;
    assert(store_init() == ESP_ERR_INVALID_CRC);
    assert(store_slots() == 0);
    assert(save_small(0, NULL) == ESP_ERR_INVALID_STATE);
}

static void test_failures_keep_old_gallery(void)
{
    for (int stage = 0; stage < 7; ++stage) {
        fresh();
        assert(save_small(7, NULL) == ESP_OK);
        assert(store_set_current(0) == ESP_OK);
        uint32_t revision = store_revision();
        if (stage == 0) fail_erase = erases + 1;
        if (stage == 1) fail_write = writes + 1; /* partial body */
        if (stage == 2) fail_write = writes + 2; /* partial thumbnail */
        if (stage == 3) fail_write = writes + 3; /* partial header */
        if (stage == 4) fail_read = reads + 1;   /* verify body */
        if (stage == 5) fail_set = 1;
        if (stage == 6) fail_commit = 1;
        int slot = 123;
        assert(save_small(8, &slot) != ESP_OK && slot == -1);
        assert(store_revision() == revision);
        meta(1, 0);
        if (stage >= 5) {
            int prior_writes = writes, prior_erases = erases;
            assert(save_small(10, NULL) == ESP_ERR_INVALID_STATE);
            assert(writes == prior_writes && erases == prior_erases);
        }
        fail_erase = fail_write = fail_read = fail_set = fail_commit = 0;
        assert(store_init() == ESP_OK);
        meta(1, 0);
        expect_small(0, 7);
        assert(save_small(9, &slot) == ESP_OK && slot == 1);
        expect_small(1, 9);
    }
    fresh();
    assert(save_small(1, NULL) == ESP_OK);
    assert(save_small(2, NULL) == ESP_OK);
    assert(store_set_current(1) == ESP_OK);
    fail_commit = 1;
    assert(store_delete(0) != ESP_OK);
    assert(store_clear() != ESP_OK);
    assert(store_set_current(0) != ESP_OK);
    meta(2, 1);
    fail_commit = 0;
    assert(store_init() == ESP_OK);
    meta(2, 1);
    expect_small(0, 1);
    expect_small(1, 2);

    /* A reported commit failure can still have published the complete blob.
     * Refuse reuse until reboot, then honor the durable order and keep both. */
    fresh();
    assert(save_small(1, NULL) == ESP_OK);
    fail_commit = 1;
    commit_error_is_durable = true;
    assert(save_small(2, NULL) != ESP_OK);
    meta(1, -1);
    fail_commit = 0;
    assert(save_small(3, NULL) == ESP_ERR_INVALID_STATE);
    assert(store_init() == ESP_OK);
    meta(2, -1);
    expect_small(0, 1);
    expect_small(1, 2);
    int slot;
    assert(save_small(3, &slot) == ESP_OK && slot == 2);
    expect_small(1, 2);
}

static void test_supplied_thumbnails(void)
{
    fresh();
    uint8_t input[16] = {0}, packed = 0x1b, jpeg[] = {0xff, 0xd8, 0x12, 0x34, 0xff, 0xd9};
    uint8_t output[sizeof(jpeg)];
    uint16_t crc = crc16_of(input, sizeof(input));
    assert(store_save_frame(input, 16, 4, crc, &packed, 1, 1, 4, 1, NULL) == ESP_OK);
    assert(store_save_frame(input, 16, 4, crc, jpeg, sizeof(jpeg), 2, 138, 192, NULL) == ESP_OK);
    assert(store_read(0, true, 0, output, 1) == ESP_OK && output[0] == packed);
    assert(store_read(1, true, 0, output, sizeof(output)) == ESP_OK);
    assert(!memcmp(output, jpeg, sizeof(jpeg)));
    assert(store_save_frame(input, 16, 4, crc, &packed, 1, 1, 3, 1, NULL) == ESP_ERR_INVALID_SIZE);
    assert(store_save_frame(input, 16, 4, crc, jpeg, 20001, 2, 1, 1, NULL) == ESP_ERR_INVALID_SIZE);
    static uint8_t large[120000], large_thumb[20000];
    assert(store_save_frame(large, 800, 600, crc16_of(large, sizeof(large)),
                            large_thumb, sizeof(large_thumb), 2, 138, 192, NULL) == ESP_ERR_INVALID_SIZE);
}

static void test_missing_partition_and_lock(void)
{
    fresh();
    have_partition = false;
    assert(store_init() == ESP_ERR_NOT_FOUND);
    assert(store_slots() == 0);
    meta(0, -1);
    assert(save_small(1, NULL) == ESP_ERR_INVALID_STATE);
    assert(store_clear() == ESP_ERR_INVALID_STATE);
    fresh();
    locked = true;
    assert(save_small(1, NULL) == ESP_ERR_TIMEOUT);
    locked = false;
    assert(save_small(1, NULL) == ESP_OK);
    assert(store_delete(-1) == ESP_ERR_NOT_FOUND);
    assert(store_set_current(8) == ESP_ERR_NOT_FOUND);
    partition.encrypted = true;
    assert(store_init() == ESP_ERR_NOT_SUPPORTED);
    assert(store_slots() == 0);
}

int main(void)
{
    test_full_delete_append_reboot();
    test_real_thumbnail_orientation();
    test_corruption_and_input_checks();
    test_failures_keep_old_gallery();
    test_supplied_thumbnails();
    test_missing_partition_and_lock();
    puts("store: gallery order, capacity, persistence, CRC, thumbnail orientation, failures and missing partition PASS");
    return 0;
}
