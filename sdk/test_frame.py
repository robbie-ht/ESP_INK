"""moink_frame.py 离线自检 —— 不需要硬件、不需要 bleak。

    python sdk/test_frame.py        # 从工程根目录
    python test_frame.py            # 或从 sdk/ 目录

验的是「交给固件的那一坨字节」到底对不对，这是整条链路里最不能错的地方：
  * CRC 金标向量（固件用同一套算法校验）
  * 单色整屏的字节值（黑 0x00 / 白 0x55 / 黄 0xAA / 红 0xFF）
  * 打包 ↔ 解包往返一致
  * 180° 契约的四个锚点（左上角像素落在最后一个字节的最低 2 位……）
  * 逐行切片实现与「逐像素」参考实现在小图上完全等价
"""

from __future__ import annotations

import random
import sys

from moink_frame import (
    BLACK, WHITE, YELLOW, RED,
    FRAME_W, FRAME_H, FRAME_LEN, FRAME_PIXELS,
    crc16, crc32, pack_2bpp, unpack_2bpp, solid_frame, solid_pixels,
)

_failed = 0


def check(name: str, cond: bool, detail: str = "") -> None:
    global _failed
    if cond:
        print(f"  PASS  {name}")
    else:
        _failed += 1
        print(f"  FAIL  {name}   {detail}")


def pack_ref(codes: bytes, w: int, h: int) -> bytes:
    """逐像素参考实现（按固件契约直译），用来交叉验证打包函数。"""
    row_bytes = w // 4
    out = bytearray(row_bytes * h)
    for y in range(h):
        dst_row = (h - 1 - y) * row_bytes          # 缓冲自下而上
        for x in range(w):
            sx = w - 1 - x                         # 行内水平反转
            out[dst_row + (sx >> 2)] |= (codes[y * w + x] & 3) << (6 - 2 * (sx & 3))
    return bytes(out)


def main() -> int:
    print("CRC 金标")
    check('crc16(b"123456789") == 0x29B1', crc16(b"123456789") == 0x29B1,
          hex(crc16(b"123456789")))
    check('crc32(b"123456789") == 0xCBF43926', crc32(b"123456789") == 0xCBF43926,
          hex(crc32(b"123456789")))
    check("crc16(b'') == 0xFFFF（init 值）", crc16(b"") == 0xFFFF)

    print("尺寸与单色整屏")
    check(f"FRAME_LEN == 105984", FRAME_LEN == 105984, str(FRAME_LEN))
    check(f"FRAME_PIXELS == 423936", FRAME_PIXELS == 423936, str(FRAME_PIXELS))
    for color, byte, label in ((BLACK, 0x00, "黑"), (WHITE, 0x55, "白"),
                               (YELLOW, 0xAA, "黄"), (RED, 0xFF, "红")):
        f = solid_frame(color)
        check(f"solid_frame({label}) 全 0x{byte:02X}", f == bytes([byte]) * FRAME_LEN)
        check(f"solid_frame({label}) 长度", len(f) == FRAME_LEN)

    print("打包/解包往返")
    random.seed(20261117)
    img = bytes(random.randrange(4) for _ in range(FRAME_PIXELS))
    buf = pack_2bpp(img)
    check("长度 == 105984", len(buf) == FRAME_LEN, str(len(buf)))
    check("unpack(pack(x)) == x", unpack_2bpp(buf) == img)
    check("solid_pixels 往返", unpack_2bpp(solid_frame(YELLOW)) == solid_pixels(YELLOW))

    print("180° 契约锚点")
    # 图像左上角 (0,0) -> 旋转后到右下角 -> 缓冲最后一行最后一个字节的最低 2 位
    img = bytearray(solid_pixels(BLACK))
    img[0] = RED
    check("图像(0,0) 落在 buf[-1] 的低 2 位", pack_2bpp(bytes(img))[-1] & 0x03 == RED)

    # 图像右下角 (w-1,h-1) -> 旋转后到左上角 -> 缓冲第一个字节的最高 2 位
    img = bytearray(solid_pixels(BLACK))
    img[-1] = YELLOW
    check("图像(w-1,h-1) 落在 buf[0] 的高 2 位", pack_2bpp(bytes(img))[0] >> 6 == YELLOW)

    # 第一行最左 4 像素：反转后进最后一个字节，位序 x3<<6 | x2<<4 | x1<<2 | x0
    img = bytearray(solid_pixels(BLACK))
    img[0:4] = bytes([BLACK, WHITE, YELLOW, RED])
    expect = (RED << 6) | (YELLOW << 4) | (WHITE << 2) | BLACK
    check(f"字节内 MSB 优先 == 0x{expect:02X}", pack_2bpp(bytes(img))[-1] == expect,
          hex(pack_2bpp(bytes(img))[-1]))

    # 垂直方向：图像最后一行 -> 缓冲第一行
    img = bytearray(solid_pixels(BLACK))
    last_row = (FRAME_H - 1) * FRAME_W
    img[last_row:last_row + FRAME_W] = solid_pixels(RED, FRAME_W, 1)
    check("图像最后一行 -> 缓冲第一行", pack_2bpp(bytes(img))[0] == 0xFF)

    print("与逐像素参考实现交叉验证")
    for (w, h) in ((8, 4), (16, 7), (32, 9), (768, 3)):
        rnd = bytes(random.randrange(4) for _ in range(w * h))
        check(f"pack_2bpp == pack_ref  ({w}x{h})", pack_2bpp(rnd, w, h) == pack_ref(rnd, w, h))

    print("参数校验")
    for f, args, name in (
        (lambda: pack_2bpp(b"\x00" * 10), (), "长度不符要报错"),
        (lambda: pack_2bpp(b"\x00" * 12, 3, 4), (), "宽度非 4 倍数要报错"),
        (lambda: solid_frame(4), (), "非法色码要报错"),
        (lambda: unpack_2bpp(b"\x00" * 3), (), "解包长度不符要报错"),
    ):
        try:
            f(*args)
            check(name, False, "没有抛异常")
        except ValueError:
            check(name, True)

    print()
    if _failed:
        print(f"FAILED: {_failed} 项")
        return 1
    print("ALL PASS —— 帧编码与固件契约一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())