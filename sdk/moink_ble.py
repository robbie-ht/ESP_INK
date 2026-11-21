#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MoInk BLE 协议 Python 参考客户端
================================

依赖:  pip install bleak>=0.21

用法:
    import asyncio
    from moink_ble import MoinkClient, pack_2bpp

    async def main():
        cli = await MoinkClient.connect_first()      # 扫描并连接第一个 MoInk-*
        await cli.ping()
        print(await cli.get_status())

        w, h = 768, 552
        codes = bytearray(w * h)                     # 2bit 色码，行优先、自上而下
        for i in range(0, w * h, 2):
            codes[i] = 1                             # 白
        await cli.send_frame(pack_2bpp(codes, w, h), w, h)

    asyncio.run(main())

协议权威定义见 docs/API.md。本文件实现三件事：
  1. 扫描/连接/MTU/分片写入与流控
  2. 2bpp 打包（fixing 180° 缓冲契约，APP 只管画正向图）
  3. 命令封装 + 事件回调 + 分片丢包自愈

Android / iOS 原生接入看 docs/APP-INTEGRATION.md；浏览器/uni-app 看 sdk/moink_ble.ts。
"""

from __future__ import annotations

import asyncio
import struct
import zlib
from typing import Callable, Iterable, Optional

try:
    from bleak import BleakClient, BleakScanner
    from bleak.exc import BleakError
except ImportError:  # Mock transports and pure frame helpers do not need bleak.
    BleakClient = BleakScanner = None
    BleakError = RuntimeError

# ---------------------------------------------------------------- 常量

SVC_UUID = "6d6f496e-4b00-4000-8000-000000000000"
CTRL_UUID = "6d6f496e-4b00-4000-8000-000000000001"
DATA_UUID = "6d6f496e-4b00-4000-8000-000000000002"
EVT_UUID = "6d6f496e-4b00-4000-8000-000000000003"

OP_PING = 0x01
OP_GET_STATUS = 0x02
OP_GET_SETTINGS = 0x03
OP_SET_SETTINGS = 0x04
OP_FRAME_BEGIN = 0x10
OP_FRAME_END = 0x12
OP_FRAME_ABORT = 0x13
OP_CACHE_SHOW = 0x20
OP_CACHE_INFO = 0x21
OP_CAROUSEL_INFO = 0x22
OP_CAROUSEL_SHOW = 0x23
OP_CAROUSEL_DELETE = 0x24
OP_CAROUSEL_CLEAR = 0x25
OP_CAROUSEL_LIST = 0x26
OP_CAROUSEL_READ = 0x27
OP_GHOST_CLEAR = 0x30
OP_OTA_BEGIN = 0x40
OP_OTA_END = 0x42
OP_OTA_ABORT = 0x43
OP_REBOOT = 0x50
OP_FACTORY_RESET = 0x51
OP_SLEEP_NOW = 0x52

ST_OK = 0x00
ST_UNKNOWN_OP = 0x01
ST_BAD_ARG = 0x02
ST_BAD_STATE = 0x03
ST_BUSY = 0x04
ST_NO_MEM = 0x05
ST_GEOMETRY = 0x06
ST_LEN_MISMATCH = 0x07
ST_CRC = 0x08
ST_INCOMPLETE = 0x09
ST_SEQ_GAP = 0x0A
ST_FLASH_ERROR = 0x0B
ST_OTA_VERIFY = 0x0C
ST_NOT_READY = 0x0D
ST_UNSUPPORTED = 0x0E
ST_FULL = 0x0F
ST_STALE = 0x10

EVT_DISPLAY_DONE = 0x01
EVT_BUTTON = 0x02
EVT_LOW_BATTERY = 0x03
EVT_STATE = 0x04
EVT_TRANSFER_GAP = 0x05
EVT_STORE_DONE = 0x06

STATUS_NAME = {
    0x00: "OK", 0x01: "UNKNOWN_OP", 0x02: "BAD_ARG", 0x03: "BAD_STATE",
    0x04: "BUSY", 0x05: "NO_MEM", 0x06: "GEOMETRY", 0x07: "LEN_MISMATCH",
    0x08: "CRC", 0x09: "INCOMPLETE", 0x0A: "SEQ_GAP", 0x0B: "FLASH_ERROR",
    0x0C: "OTA_VERIFY", 0x0D: "NOT_READY", 0x0E: "UNSUPPORTED",
    0x0F: "FULL", 0x10: "STALE",
}

STATE_NAME = {0: "idle", 1: "recv_frame", 2: "recv_ota", 3: "refreshing", 4: "clearing", 6: "sleeping"}

# 帧编码与校验和放在 moink_frame.py：纯函数、不依赖 bleak，可单独测试/移植
from moink_frame import (  # noqa: E402  (必须在 bleak 之后，保持分组清晰)
    BLACK, WHITE, YELLOW, RED,
    FRAME_W, FRAME_H, FRAME_LEN, FRAME_PIXELS,
    crc16, crc32, pack_2bpp, unpack_2bpp, solid_frame, solid_pixels,
)


class MoinkError(Exception):
    """固件返回非 0 状态。status 见 STATUS_NAME。"""

    def __init__(self, status: int, op: str = "", detail: bytes = b""):
        self.status = status
        self.op = op
        self.detail = detail
        super().__init__(
            f"{op}: {STATUS_NAME.get(status, hex(status))}"
            + (f" (payload={detail.hex()})" if detail else "")
        )


# ---------------------------------------------------------------- 帧编码
#
# crc16 / crc32 / pack_2bpp / unpack_2bpp / solid_frame / solid_pixels 都定义在
# moink_frame.py —— 纯函数、不依赖 bleak，方便单元测试与移植到 Kotlin/Swift。
# 本文件顶部已 import，对外名字和签名保持不变。


# ---------------------------------------------------------------- 客户端

class MoinkClient:
    def __init__(self, client: BleakClient, mtu: int = 23):
        self.ble = client
        self.mtu = mtu
        self._seq = 0
        self._rsp: dict[int, asyncio.Future] = {}
        self._gap_at: Optional[int] = None
        self._events: list[Callable[[int, int, bytes], None]] = []
        self._last_state = 0

    # ---------- 连接 ----------

    @staticmethod
    async def scan(timeout: float = 5.0, prefix: str = "MoInk") -> list[tuple]:
        """返回 [(BLEDevice, name, rssi), ...]，按 RSSI 降序。"""
        if BleakScanner is None:
            raise RuntimeError("需要 bleak: pip install 'bleak>=0.21'")
        found = await BleakScanner.discover(timeout=timeout, return_adv=True)
        out = []
        for device, adv in found.values():
            name = adv.local_name or device.name or ""
            if name.startswith(prefix):
                out.append((device, name, adv.rssi))
        out.sort(key=lambda t: t[2], reverse=True)
        return out

    @classmethod
    async def connect_first(cls, timeout: float = 5.0, prefix: str = "MoInk",
                            attempts: int = 3) -> "MoinkClient":
        last: Optional[Exception] = None
        for _ in range(attempts):
            devs = await cls.scan(timeout, prefix)
            if not devs:
                last = BleakError(f"没扫到 {prefix}*（可能已深睡，按一下按键）")
                continue
            try:
                cli = await cls.connect(devs[0][0])
                return cli
            except Exception as e:      # noqa: BLE001
                last = e
                await asyncio.sleep(0.5)
        raise last or BleakError("连接失败")

    @classmethod
    async def connect(cls, device, timeout: float = 20.0) -> "MoinkClient":
        if BleakClient is None:
            raise RuntimeError("需要 bleak: pip install 'bleak>=0.21'")
        client = BleakClient(device, timeout=timeout)
        await client.connect()

        # Windows/macOS 连上后 MTU 协商是异步的，稍等一拍再读。
        await asyncio.sleep(0.4)
        mtu = getattr(client, "mtu_size", 23) or 23
        self = cls(client, mtu)

        await client.start_notify(EVT_UUID, self._on_notify)
        await self.ping()
        return self

    async def disconnect(self) -> None:
        try:
            await self.ble.stop_notify(EVT_UUID)
        except Exception:      # noqa: BLE001
            pass
        await self.ble.disconnect()

    async def __aenter__(self) -> "MoinkClient":
        return self

    async def __aexit__(self, *exc) -> None:
        await self.disconnect()

    # ---------- 通知 ----------

    def _on_notify(self, _handle, data: bytearray) -> None:
        if not data:
            return
        kind = data[0]
        if kind == 0x01 and len(data) >= 3:               # RSP
            seq, status, payload = data[1], data[2], bytes(data[3:])
            fut = self._rsp.get(seq)
            if fut and not fut.done():
                fut.set_result((status, payload))
            return
        if kind == 0x81 and len(data) >= 3:               # EVENT
            evt, arg, payload = data[1], data[2], bytes(data[3:])
            if evt == EVT_TRANSFER_GAP and len(payload) >= 2:
                self._gap_at = struct.unpack_from("<H", payload, 0)[0]
            if evt == EVT_STATE:
                self._last_state = arg
            for cb in tuple(self._events):
                try:
                    cb(evt, arg, payload)
                except Exception:      # noqa: BLE001
                    pass

    def on_event(self, cb: Callable[[int, int, bytes], None]) -> None:
        """注册事件回调 cb(evt, arg, payload)。"""
        self._events.append(cb)

    # ---------- 底层收发 ----------

    def _next_seq(self) -> int:
        for _ in range(256):
            self._seq = (self._seq + 1) & 0xFF
            if self._seq not in self._rsp:
                return self._seq
        raise RuntimeError("too many pending BLE commands")

    def _event_waiter(self, event: int, accepted=None):
        """Register before sending a command; events may precede its RSP."""
        future = asyncio.get_running_loop().create_future()
        def cb(evt, arg, payload):
            if evt == event and (accepted is None or arg in accepted) and not future.done():
                future.set_result((arg, payload))
        self._events.append(cb)
        def cleanup():
            if cb in self._events:
                self._events.remove(cb)
            if not future.done():
                future.cancel()
        return future, cleanup

    async def _wait_result(self, future, timeout, op, store=False):
        result, payload = await asyncio.wait_for(future, timeout)
        if (store and result != ST_OK) or (not store and result == 1):
            raise MoinkError(result if store else ST_NOT_READY, op, payload)
        return payload

    @property
    def chunk_size(self) -> int:
        return max(16, min(self.mtu - 5, 512))

    async def _cmd(self, op: int, args: bytes = b"", timeout: float = 15.0) -> tuple[int, bytes]:
        seq = self._next_seq()
        fut: asyncio.Future = asyncio.get_running_loop().create_future()
        self._rsp[seq] = fut
        try:
            await self.ble.write_gatt_char(CTRL_UUID, bytes([op, seq]) + args, response=True)
            return await asyncio.wait_for(fut, timeout)
        finally:
            self._rsp.pop(seq, None)

    async def _call(self, op: int, args: bytes = b"", timeout: float = 15.0) -> bytes:
        status, payload = await self._cmd(op, args, timeout)
        if status != ST_OK:
            raise MoinkError(status, f"op{op:02X}", payload)
        return payload

    async def _write_data(self, header_seq: int, chunk: bytes) -> None:
        await self.ble.write_gatt_char(DATA_UUID, struct.pack("<H", header_seq) + chunk,
                                       response=False)

    # ---------- 命令 ----------

    async def ping(self) -> dict:
        pl = await self._call(OP_PING)
        return {"api": pl[0], "version": pl[2:2 + pl[1]].decode("utf-8", "replace")}

    async def get_status(self) -> dict:
        pl = await self._call(OP_GET_STATUS, timeout=20.0)
        o = 0
        out = {"api": pl[o], "state": STATE_NAME.get(pl[o + 1], pl[o + 1]),
               "panel": pl[o + 2]}
        o += 3
        mv = struct.unpack_from("<h", pl, o)[0]; o += 2
        out["battery_mv"] = mv if mv > 0 else None
        out["uptime_s"], out["heap_free"], out["mtu"] = struct.unpack_from("<IIH", pl, o)
        o += 10
        out["wake_reason"] = pl[o]; o += 1
        out["sleep_s"] = struct.unpack_from("<I", pl, o)[0]; o += 4
        out["timeout_action"] = pl[o]; o += 1
        out["auto_wake_s"] = struct.unpack_from("<I", pl, o)[0]; o += 4
        out["cache_valid"] = bool(pl[o]); o += 1
        out["version"] = pl[o + 1:o + 1 + pl[o]].decode("utf-8", "replace")
        end = o + 1 + pl[o]
        out["gallery_read_flags"] = pl[end] if len(pl) > end else 0
        out["gallery_read_supported"] = bool(out["gallery_read_flags"] & 1)
        out["gallery_read_window"] = bool(out["gallery_read_flags"] & 2)
        out["gallery_append_committed"] = bool(out["gallery_read_flags"] & 4)
        return out

    async def get_settings(self) -> dict:
        pl = await self._call(OP_GET_SETTINGS)
        if len(pl) < 21:
            raise ValueError("GET_SETTINGS truncated; negotiate a larger MTU")
        o = 0
        out = {"panel": pl[o]}; o += 1
        out["sleep_s"] = struct.unpack_from("<I", pl, o)[0]; o += 4
        out["timeout_action"] = pl[o]; o += 1
        out["auto_wake_s"] = struct.unpack_from("<I", pl, o)[0]; o += 4
        out["gpio_preset"] = pl[o]; o += 1
        out["pins"] = list(pl[o:o + 8]); o += 8
        out["cache_on"] = bool(pl[o]); o += 1
        name_end = o + 1 + pl[o]
        if len(pl) < name_end:
            raise ValueError("GET_SETTINGS truncated; negotiate a larger MTU")
        out["name"] = pl[o + 1:name_end].decode("utf-8", "replace")
        out["wake_show"] = bool(pl[name_end]) if len(pl) > name_end else False
        return out

    async def set_settings(self, **kw) -> dict:
        """只改传进来的字段，其余保持。返回 {applied, err_id, err_code, flags}。

        可用字段: panel=0|1, sleep_s=int, timeout_action=0|1, auto_wake_s=int,
                  name=str, gpio_preset=0..3, pins=[8 个引脚], cache_on=bool, wake_show=bool
        pins 仅适用于自定义 gpio_preset=3；B2 支持同请求任意字段顺序。
        """
        tlv = bytearray()

        def add(tid: int, payload: bytes) -> None:
            tlv.extend(bytes([tid, len(payload)]) + payload)

        if "panel" in kw:
            add(0x01, bytes([kw["panel"]]))
        if "sleep_s" in kw:
            add(0x02, struct.pack("<I", kw["sleep_s"]))
        if "timeout_action" in kw:
            add(0x03, bytes([kw["timeout_action"]]))
        if "auto_wake_s" in kw:
            add(0x04, struct.pack("<I", kw["auto_wake_s"]))
        if "name" in kw:
            add(0x05, kw["name"].encode("utf-8"))
        if "gpio_preset" in kw:
            add(0x06, bytes([kw["gpio_preset"]]))
        if "pins" in kw:
            add(0x07, bytes(kw["pins"]))
        if "cache_on" in kw:
            add(0x08, bytes([1 if kw["cache_on"] else 0]))
        if "wake_show" in kw:
            add(0x09, bytes([1 if kw["wake_show"] else 0]))

        status, pl = await self._cmd(OP_SET_SETTINGS, bytes(tlv))
        if len(pl) >= 4:
            res = {"applied": pl[0], "err_id": pl[1], "err_code": pl[2], "flags": pl[3]}
        else:
            res = {"applied": 0, "err_id": 0, "err_code": status, "flags": 0}
        if status != ST_OK:
            raise MoinkError(status, "SET_SETTINGS", pl)
        return res

    # ---------- 传图 ----------

    async def send_frame(self, buf: bytes, w: int = FRAME_W, h: int = FRAME_H,
                         save_cache: bool = True, timeout: float = 900.0,
                         progress: Optional[Callable[[float], None]] = None, *,
                         save_gallery: bool = False, store_only: bool = False,
                         thumbnail: bytes = b"", thumbnail_format: int = 2,
                         thumbnail_w: int = 0, thumbnail_h: int = 0,
                         wait_display: bool = False) -> None:
        """上传帧；默认在排队刷屏后返回，wait_display=True 才等刷完。

        save_gallery=True 追加到设备图库，并等待 STORE_DONE 确认落盘。
        store_only=True 仅存图库，不刷屏；必须同时设置 save_gallery。
        thumbnail 可为 JPEG(format=2) 或 packed2bpp(format=1)，最大 20000 B。
        """
        if w <= 0 or h <= 0 or w % 4 or len(buf) != w // 4 * h:
            raise ValueError("invalid frame geometry/length")
        if store_only and (not save_gallery or wait_display):
            raise ValueError("store_only requires save_gallery and cannot wait_display")
        if thumbnail and (not save_gallery or thumbnail_format not in (1, 2)
                          or not 0 < thumbnail_w <= 65535 or not 0 < thumbnail_h <= 65535
                          or len(thumbnail) > 20000):
            raise ValueError("invalid gallery thumbnail metadata")
        if thumbnail and thumbnail_format == 1 and (thumbnail_w % 4 or thumbnail_w > 200 or thumbnail_h > 150 or
                len(thumbnail) != thumbnail_w // 4 * thumbnail_h):
            raise ValueError("invalid packed thumbnail length")
        if thumbnail and thumbnail_format == 2 and (len(thumbnail) < 4 or
                thumbnail[:2] != b"\xff\xd8" or thumbnail[-2:] != b"\xff\xd9"):
            raise ValueError("invalid JPEG thumbnail markers")
        if len(buf) + len(thumbnail) + 64 > 131072:
            raise ValueError("frame and thumbnail exceed gallery slot capacity")
        flags = (0 if save_cache else 1) | (2 if save_gallery else 0) | (4 if store_only else 0)
        args = struct.pack("<BHHIHH", flags, w, h, len(buf), crc16(buf), self.chunk_size)
        if thumbnail:
            args += struct.pack("<BHHIH", thumbnail_format, thumbnail_w,
                                thumbnail_h, len(thumbnail), crc16(thumbnail))
        data = bytes(buf) + bytes(thumbnail)
        pl = await self._call(OP_FRAME_BEGIN, args)
        if len(pl) < 2:
            raise ValueError("truncated FRAME_BEGIN response")
        chunk = struct.unpack_from("<H", pl)[0]
        if not 16 <= chunk <= self.chunk_size:
            raise ValueError("invalid negotiated chunk size")
        total = (len(data) + chunk - 1) // chunk
        self._gap_at = None
        idx = retries = 0
        store_wait = self._event_waiter(EVT_STORE_DONE) if save_gallery else None
        display_wait = self._event_waiter(EVT_DISPLAY_DONE, {0, 1}) if wait_display else None
        try:
            while True:
                while idx < total:
                    if self._gap_at is not None:
                        gap, self._gap_at = self._gap_at, None
                        if gap < idx:
                            idx = gap
                            retries += 1
                    if retries > 8:
                        raise MoinkError(ST_SEQ_GAP, "FRAME", b"too many retries")
                    await self._write_data(idx, data[idx * chunk:(idx + 1) * chunk])
                    idx += 1
                    if progress and (idx & 15) == 0:
                        progress(idx / total)
                    if (idx & 31) == 0:
                        await asyncio.sleep(0.005)
                status, pl = await self._cmd(OP_FRAME_END, timeout=timeout)
                if status == ST_OK:
                    if store_wait:
                        await self._wait_result(store_wait[0], timeout, "STORE", store=True)
                    if display_wait:
                        await self._wait_result(display_wait[0], timeout, "DISPLAY")
                    if progress:
                        progress(1.0)
                    return
                retries += 1
                if retries > 8:
                    raise MoinkError(ST_SEQ_GAP, "FRAME", b"too many retries")
                if status == ST_INCOMPLETE and len(pl) >= 2:
                    idx = struct.unpack_from("<H", pl)[0]
                    if idx >= total:
                        raise ValueError("invalid FRAME_END resume offset")
                    continue
                if status == ST_CRC:
                    idx = 0
                    continue
                raise MoinkError(status, "FRAME_END", pl)
        except BaseException:
            try:
                await self._cmd(OP_FRAME_ABORT, timeout=5)
            except Exception:
                pass
            raise
        finally:
            if store_wait:
                store_wait[1]()
            if display_wait:
                display_wait[1]()

    # ---------- 设备图库（B2.0.0，api=1 增量扩展） ----------

    async def gallery_info(self) -> dict:
        p = await self._call(OP_CAROUSEL_INFO)
        if len(p) != 7:
            raise ValueError("invalid CAROUSEL_INFO response")
        count, cur, slots, revision = struct.unpack("<BBBI", p)
        return {"count": count, "current": None if cur == 255 else cur,
                "slots": slots, "revision": revision}

    async def gallery_list(self) -> list[dict]:
        """返回一致版本的完整列表；图库被另一操作修改时抛 ST_STALE。"""
        info = await self.gallery_info()
        items = []
        while len(items) < info["count"]:
            start = len(items)
            p = await self._call(OP_CAROUSEL_LIST, bytes([start, info["count"] - start]))
            if len(p) < 6:
                raise ValueError("truncated CAROUSEL_LIST response")
            revision, first, count = struct.unpack_from("<IBB", p)
            if revision != info["revision"]:
                raise MoinkError(ST_STALE, "CAROUSEL_LIST")
            if first != start or not count or count > info["count"] - start or len(p) != 6 + 22 * count:
                raise ValueError("invalid CAROUSEL_LIST page")
            for i in range(count):
                slot, w, h, size, crc, fmt, tw, th, ts, tc = struct.unpack_from("<BHHIHBHHIH", p, 6 + 22*i)
                if slot != start + i:
                    raise ValueError("invalid gallery slot order")
                items.append({"slot": slot, "w": w, "h": h, "len": size, "crc16": crc,
                              "thumbnail_format": fmt, "thumbnail_w": tw, "thumbnail_h": th,
                              "thumbnail_len": ts, "thumbnail_crc16": tc,
                              "revision": revision})
        return items

    async def gallery_show(self, slot: int, timeout: float = 900.0) -> None:
        if not 0 <= slot <= 255:
            raise ValueError("invalid slot")
        wait, cleanup = self._event_waiter(EVT_DISPLAY_DONE, {1, 4})
        try:
            await self._call(OP_CAROUSEL_SHOW, bytes([slot]))
            await self._wait_result(wait, timeout, "CAROUSEL_SHOW")
        finally:
            cleanup()

    async def gallery_next(self, timeout: float = 900.0) -> None:
        await self.gallery_show(255, timeout)

    async def _gallery_mutation(self, op: int, args: bytes = b"", timeout: float = 60.0) -> dict:
        wait, cleanup = self._event_waiter(EVT_STORE_DONE)
        try:
            await self._call(op, args)
            p = await self._wait_result(wait, timeout, f"op{op:02X}", store=True)
            if len(p) != 3:
                raise ValueError("invalid STORE_DONE response")
            return {"slot": p[0], "count": p[1], "current": None if p[2] == 255 else p[2]}
        finally:
            cleanup()

    async def gallery_delete(self, slot: int) -> dict:
        return await self._gallery_mutation(OP_CAROUSEL_DELETE, bytes([slot]))

    async def gallery_clear(self) -> dict:
        return await self._gallery_mutation(OP_CAROUSEL_CLEAR)

    async def gallery_download(self, slot: int, thumbnail: bool = False,
                               progress: Optional[Callable[[float], None]] = None) -> bytes:
        """按偏移拉取整帧/缩略图；每片检查身份、偏移、版本，末尾验证 CRC16。"""
        items = await self.gallery_list()
        item = next((it for it in items if it["slot"] == slot), None)
        if item is None:
            raise MoinkError(ST_NOT_READY, "CAROUSEL_READ")
        total = item["thumbnail_len" if thumbnail else "len"]
        expected_crc = item["thumbnail_crc16" if thumbnail else "crc16"]
        expected_format = item["thumbnail_format"] if thumbnail else 1
        if not total:
            raise MoinkError(ST_NOT_READY, "CAROUSEL_READ", b"thumbnail absent")
        kind = int(thumbnail)
        data = bytearray()
        while len(data) < total:
            requested = min(max(1, self.mtu - 19), 496, total - len(data))
            args = struct.pack("<BBIHI", slot, kind, len(data), requested, item["revision"])
            p = await self._call(OP_CAROUSEL_READ, args)
            if len(p) < 14:
                raise ValueError("empty/truncated CAROUSEL_READ response")
            got_slot, got_kind, offset, size, crc, fmt = struct.unpack_from("<BBIIHB", p)
            block = p[13:]
            if ((got_slot, got_kind, offset, size, crc, fmt) !=
                    (slot, kind, len(data), total, expected_crc, expected_format)
                    or len(block) > requested or len(data) + len(block) > total):
                raise ValueError("CAROUSEL_READ identity/offset/metadata mismatch")
            data.extend(block)
            if progress:
                progress(len(data) / total)
        if crc16(data) != expected_crc:
            raise MoinkError(ST_CRC, "CAROUSEL_READ")
        return bytes(data)

    # ---------- 缓存 / 清理 ----------

    async def cache_show(self) -> None:
        await self._call(OP_CACHE_SHOW, timeout=180.0)

    async def cache_info(self) -> dict:
        pl = await self._call(OP_CACHE_INFO)
        w, h, crc = struct.unpack_from("<HHH", pl, 1)
        return {"valid": bool(pl[0]), "w": w, "h": h, "crc16": crc,
                "len": struct.unpack_from("<I", pl, 7)[0]}

    async def ghost_clear(self, cycles: int = 2) -> None:
        await self._call(OP_GHOST_CLEAR, bytes([cycles]), timeout=180.0)

    # ---------- OTA ----------

    async def ota(self, image: bytes, timeout: float = 180.0,
                  progress: Optional[Callable[[float], None]] = None) -> None:
        """推送固件镜像（.bin）。成功返回后设备约 1.5s 自动重启。

        ⚠️ 检测到传输出错时调用 ota_abort()，别让设备停在半擦的槽里 ——
        实际不会损坏运行中的固件（写的是另一个 ota 槽），只是浪费一次流量。
        """
        crc = zlib.crc32(image) & 0xFFFFFFFF
        chunk = self.chunk_size
        pl = await self._call(OP_OTA_BEGIN, struct.pack("<IIH", len(image), crc, chunk))
        chunk = struct.unpack_from("<H", pl, 0)[0]

        if not 16 <= chunk <= self.chunk_size:
            raise ValueError("invalid negotiated OTA chunk size")
        total = (len(image) + chunk - 1) // chunk
        self._gap_at = None
        idx = retries = 0
        try:
            while True:
                while idx < total:
                    if self._gap_at is not None:
                        gap, self._gap_at = self._gap_at, None
                        if gap < idx:
                            idx = gap
                            retries += 1
                    if retries > 8:
                        raise MoinkError(ST_SEQ_GAP, "OTA", b"too many retries")
                    await self._write_data(idx, image[idx * chunk:(idx + 1) * chunk])
                    idx += 1
                    if progress and (idx & 15) == 0:
                        progress(idx / total)
                    if (idx & 31) == 0:
                        await asyncio.sleep(0.005)
                status, pl = await self._cmd(OP_OTA_END, timeout=timeout)
                if status == ST_OK:
                    break
                if status == ST_INCOMPLETE and len(pl) >= 2:
                    idx = struct.unpack_from("<H", pl)[0]
                    retries += 1
                    if idx >= total or retries > 8:
                        raise MoinkError(ST_SEQ_GAP, "OTA", b"invalid/excessive resume")
                    continue
                raise MoinkError(status, "OTA_END", pl)
        except BaseException:
            try:
                await self._cmd(OP_OTA_ABORT, timeout=5.0)
            except Exception:
                pass
            raise
        if progress:
            progress(1.0)

    # ---------- 维护 ----------

    async def abort_frame(self) -> None:
        await self._call(OP_FRAME_ABORT)

    async def reboot(self, delay_s: int = 1) -> None:
        await self._call(OP_REBOOT, bytes([delay_s]))

    async def factory_reset(self) -> None:
        await self._call(OP_FACTORY_RESET)

    async def sleep_now(self) -> None:
        """立刻深睡。之后必须按设备按键才能再被扫到。"""
        await self._call(OP_SLEEP_NOW)
