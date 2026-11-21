#ifndef MOINK_CACHE_H
#define MOINK_CACHE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/*
 * 「最近一帧」缓存（fcache 分区，192 KB）。
 *
 * 用途：
 *   - 双击设备按键：不连手机也能把当前画面重刷一遍（面板被静电/杂散干扰打花时救场）
 *   - CACHE_SHOW 命令（docs/API.md §6.8）
 *
 * 布局（扇区 4 KB）：
 *   0x0000  16 字节头（magic / ver / w / h / len / crc16）
 *   0x1000  2bpp 整帧载荷（105984 字节 = 26 个扇区）
 *
 * 写序：先擦头 -> 写载荷 -> 最后写头。
 * 中途掉电会留下「头无效」的缓存，而不是一个 CRC 对不上的错帧。
 */

/* 载荷上限 = 帧缓冲上限（120000），762x552 实际只用 105984。 */
#define EPD_CACHE_MAX_PAYLOAD 120000

esp_err_t cache_init(void);          /* 分区不存在时返回 ESP_ERR_NOT_FOUND，其它接口降级 */
bool      cache_has(void);           /* 头有效（不校验载荷 CRC） */

/* 返回缓存几何与 CRC；无缓存返回 ESP_ERR_NOT_FOUND。 */
esp_err_t cache_info(uint16_t *w, uint16_t *h, uint16_t *crc16, uint32_t *len);

/* 保存一帧（frame 为已打包的 2bpp，长度 = w/4*h）。内容未变时直接返回，不磨损 flash。 */
esp_err_t cache_save(const uint8_t *frame, uint16_t w, uint16_t h, uint16_t crc16);

/* 读回并校验 CRC。out 容量不足或 CRC 不符返回错误。 */
esp_err_t cache_load(uint8_t *out, uint32_t cap,
                     uint16_t *w, uint16_t *h, uint16_t *crc16);

esp_err_t cache_clear(void);

#endif /* MOINK_CACHE_H */