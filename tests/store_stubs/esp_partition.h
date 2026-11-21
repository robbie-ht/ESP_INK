#ifndef TEST_ESP_PARTITION_H
#define TEST_ESP_PARTITION_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_ANY 0xff
typedef struct { uint32_t size; bool encrypted; } esp_partition_t;
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label);
esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *out, size_t len);
esp_err_t esp_partition_write(const esp_partition_t *p, size_t off, const void *data, size_t len);
esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t len);
#endif
