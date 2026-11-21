"""MoInk 帧编码 / 校验和（纯 Python，不依赖 bleak）。

单独成文件的原因：
  1. 不装 bleak 也能用 —— 离线生图、单元测试、CI；
  2. APP 侧（Kotlin/Swift）可以逐行照着移植，不用读 BLE 那一坨；
  3. 打包规则只有这一处定义，Python SDK 与测试共用。

帧缓冲契约（与固件 epd_drv.c 一致）：
  768x552、2bpp、每字节 4 像素 MSB 优先（像素 x 在位 6-2*(x%4)）、
  缓冲自下而上存储 + 逐行整行水平反转 —— 合起来就是「正向图像旋转 180°」。
  上层只画一张正常的图，交给 pack_2bpp() 一次做完。
"""

from __future__ import annotations

import zlib
from typing import Iterable

# 色码（与固件 epd_drv.h 一致）
BLACK, WHITE, YELLOW, RED = 0, 1, 2, 3

FRAME_W, FRAME_H = 768, 552
FRAME_PIXELS = FRAME_W * FRAME_H          # 423936
FRAME_LEN = FRAME_W // 4 * FRAME_H        # 105984 字节

# 单色整屏的字节值（黑/白/黄/红），可用于极速自测
SOLID_BYTE = {BLACK: 0x00, WHITE: 0x55, YELLOW: 0xAA, RED: 0xFF}


# ---------------------------------------------------------------- 校验和

def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE：poly 0x1021、init 0xFFFF、无反射、无终异或。

    金标向量: crc16(b"123456789") == 0x29B1
    固件用同一套（crc_util.h），帧传输与帧缓存都靠它校验。
    """
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def crc32(data: bytes) -> int:
    """CRC-32/ISO-HDLC，与固件 esp_rom_crc32_le() 完全一致（即 zlib.crc32）。"""
    return zlib.crc32(data) & 0xFFFFFFFF


# ---------------------------------------------------------------- 2bpp 打包

def pack_2bpp(codes: Iterable[int], w: int = FRAME_W, h: int = FRAME_H) -> bytes:
    """把「正向、自上而下、左到右」的 2bit 色码序列打成固件要的帧缓冲。

    codes 长度必须 w*h，元素 0=黑 1=白 2=黄 3=红（也可以直接传 bytes）。
    返回长度 w//4*h 的 bytes。
    """
    codes = bytes(codes) if not isinstance(codes, (bytes, bytearray)) else codes
    if len(codes) != w * h:
        raise ValueError(f"codes 长度应为 {w*h}，实际 {len(codes)}")
    if w % 4:
        raise ValueError("宽度必须是 4 的倍数")

    row_bytes = w // 4
    out = bytearray(row_bytes * h)
    for y in range(h):
        row = codes[y * w:(y + 1) * w][::-1]      # 行内水平反转
        packed = bytes(
            (row[i] << 6) | (row[i + 1] << 4) | (row[i + 2] << 2) | row[i + 3]
            for i in range(0, w, 4)
        )
        dst = (h - 1 - y) * row_bytes             # 缓冲自下而上
        out[dst:dst + row_bytes] = packed
    return bytes(out)


def unpack_2bpp(buf: bytes, w: int = FRAME_W, h: int = FRAME_H) -> bytes:
    """pack_2bpp 的逆运算（调试、回读校验、单元测试用）。"""
    if len(buf) != w // 4 * h:
        raise ValueError(f"buf 长度应为 {w//4*h}，实际 {len(buf)}")

    row_bytes = w // 4
    out = bytearray(w * h)
    for y in range(h):
        src = (h - 1 - y) * row_bytes
        row = bytearray(w)
        for i in range(row_bytes):
            b = buf[src + i]
            row[i * 4] = (b >> 6) & 3
            row[i * 4 + 1] = (b >> 4) & 3
            row[i * 4 + 2] = (b >> 2) & 3
            row[i * 4 + 3] = b & 3
        out[y * w:(y + 1) * w] = bytes(row[::-1])
    return bytes(out)


def solid_frame(color: int = WHITE, w: int = FRAME_W, h: int = FRAME_H) -> bytes:
    """整屏单色帧缓冲（最快的连通性自测，一次写满整屏）。"""
    if color not in SOLID_BYTE:
        raise ValueError(f"色码应为 0..3，实际 {color}")
    # 单色时每个字节都是同一值，不必逐像素走一遍
    return bytes([SOLID_BYTE[color]]) * (w // 4 * h)


def solid_pixels(color: int, w: int = FRAME_W, h: int = FRAME_H) -> bytes:
    """整屏单色的「正向色码」序列（给需要改像素的场景做底图）。"""
    if color not in SOLID_BYTE:
        raise ValueError(f"色码应为 0..3，实际 {color}")
    return bytes([color]) * (w * h)