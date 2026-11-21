#ifndef MOINK_STORE_H
#define MOINK_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define STORE_SLOT_SZ        0x20000u
#define STORE_HEADER_SZ      64u
#define STORE_SLOT_MAX       8u
#define STORE_FRAME_MAX      120000u
#define STORE_THUMB_PACKED   1u
#define STORE_THUMB_JPEG     2u
#define STORE_THUMB_JPEG_MAX 20000u
#define STORE_ERR_FULL       ((esp_err_t)0x7101)

typedef struct {
    uint16_t w, h, crc16;
    uint32_t len;
    uint8_t thumb_format;
    uint16_t thumb_w, thumb_h;
    uint32_t thumb_len;
    uint16_t thumb_crc16;
} store_info_t;

/* Call after nvs_flash_init(). Missing store partition returns ESP_ERR_NOT_FOUND
 * and zero slots. Corrupt metadata fails closed; existing flash is preserved.
 * This APP gallery format is separate from the Wi-Fi reference's store format.
 * After an ambiguous NVS write error, mutations return ESP_ERR_INVALID_STATE
 * until reboot/store_init reconciles persistent metadata; reads remain usable.
 */
esp_err_t store_init(void);
int store_slots(void);

/* Slots are logical indices in append order, including after hole reuse.
 * current == -1 means no gallery item is known to be displayed. Saving does not
 * change current; call store_set_current only after a successful screen refresh.
 */
void store_meta_get(int *count, int *cur);
uint32_t store_revision(void); /* membership/order revision, stable during display */
esp_err_t store_info(int slot, store_info_t *info);

/* Raw packed frame payload, without a protocol frame header. CRC is CCITT-FALSE.
 * With thumb == NULL and thumb_len == 0, generate a 4x4 majority packed thumbnail
 * in the same 180-degree packed orientation (768x552 -> 192x138). Provided packed
 * or JPEG thumbnail payloads are stored without a protocol header. All data plus
 * storage header must fit one slot. An existing image is never overwritten.
 */
esp_err_t store_save_frame(const uint8_t *data, uint16_t w, uint16_t h,
                           uint16_t crc16, const uint8_t *thumb,
                           uint32_t thumb_len, uint8_t thumb_format,
                           uint16_t thumb_w, uint16_t thumb_h, int *slot);
esp_err_t store_load_frame(int slot, uint8_t *out, size_t cap,
                           uint16_t *w, uint16_t *h);

/* Read raw payload bytes. The first read at offset zero validates the complete
 * selected payload CRC before returning data. A streaming caller must snapshot
 * store_revision() before info/read, reject changes, and verify the advertised
 * CRC when reassembling a complete/resumed download.
 */
esp_err_t store_read(int slot, bool thumbnail, uint32_t offset,
                     void *out, size_t len);
esp_err_t store_set_current(int slot); /* accepts -1 to clear the displayed index */
esp_err_t store_delete(int slot);      /* metadata only, no frame erase/copy */
esp_err_t store_clear(void);           /* metadata only, no frame erase/copy */

#endif
