#ifndef MOINK_CRC_UTIL_H
#define MOINK_CRC_UTIL_H

#include <stdint.h>
#include <stddef.h>

/*
 * 两个 CRC 都按 docs/API.md §10.1 的约定实现，与 APP 侧
 * (Python zlib.crc32 / JS crc32) 逐位一致。
 *
 *   CRC-16/CCITT-FALSE：poly 0x1021，init 0xFFFF，无反射、无终异或。
 *       金标向量："123456789" -> 0x29B1
 *   CRC-32/ISO-HDLC  ：poly 0xEDB88320 反射，init 0xFFFFFFFF，末异或 0xFFFFFFFF。
 *       = Python zlib.crc32()
 */

static inline uint16_t crc16_update(uint16_t crc, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static inline uint16_t crc16_of(const uint8_t *d, size_t n)
{
    return crc16_update(0xFFFF, d, n);
}

static inline uint32_t crc32_begin(void) { return 0xFFFFFFFFu; }

static inline uint32_t crc32_update(uint32_t crc, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

static inline uint32_t crc32_end(uint32_t crc) { return crc ^ 0xFFFFFFFFu; }

static inline uint32_t crc32_of(const uint8_t *d, size_t n)
{
    return crc32_end(crc32_update(crc32_begin(), d, n));
}

#endif /* MOINK_CRC_UTIL_H */