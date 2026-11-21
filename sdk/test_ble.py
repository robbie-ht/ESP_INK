"""Offline protocol tests: no BLE adapter, bleak, phone, or device required."""
import asyncio
import struct
import unittest

from moink_ble import *


class MockBle:
    def __init__(self):
        self.cli = MoinkClient(self, 64)
        self.commands = []
        self.received = bytearray()
        self.frame = bytes(range(32))
        self.thumb = b'\xff\xd8sample\xff\xd9'
        self.revision = 7
        self.read_fault = None
        self.store_error = ST_OK
        self.end_statuses = []
        self.force_gap = False
        self.abort_count = 0
        self.old_settings = False
        self.sleep_busy = False
        self.status_flags = None

    def rsp(self, seq, payload=b'', status=ST_OK):
        self.cli._on_notify(0, bytearray([1, seq, status]) + payload)

    def event(self, evt, arg=0, payload=b''):
        self.cli._on_notify(0, bytearray([0x81, evt, arg]) + payload)

    async def write_gatt_char(self, uuid, data, response):
        if uuid == DATA_UUID:
            seq, = struct.unpack_from('<H', data)
            if self.force_gap:
                self.event(EVT_TRANSFER_GAP, payload=struct.pack('<H', 0))
                return
            start = seq * 16
            if start < len(self.received):
                del self.received[start:]
            self.received.extend(data[2:])
            return
        op, seq, args = data[0], data[1], bytes(data[2:])
        self.commands.append((op, args))
        if op in (OP_FRAME_BEGIN, OP_OTA_BEGIN):
            self.flags = args[0]
            self.received.clear()
            self.rsp(seq, struct.pack('<HH', 16, 59))
        elif op == OP_FRAME_END:
            if self.end_statuses:
                status = self.end_statuses.pop(0)
                if status != ST_OK:
                    self.rsp(seq, struct.pack('<H', 0), status)
                    return
            # Deliberately emit BOTH completion notifications before the RSP.
            if self.flags & 2:
                self.event(EVT_STORE_DONE, self.store_error, bytes([0, 1, 0]))
            if not self.flags & 4:
                self.event(EVT_DISPLAY_DONE)
            self.rsp(seq, b'\x01')
        elif op == OP_OTA_END:
            status = self.end_statuses.pop(0) if self.end_statuses else ST_OK
            self.rsp(seq, struct.pack('<H', 0) if status == ST_INCOMPLETE else b'', status)
        elif op in (OP_FRAME_ABORT, OP_OTA_ABORT):
            self.abort_count += 1
            self.rsp(seq)
        elif op == OP_CAROUSEL_INFO:
            self.rsp(seq, struct.pack('<BBBI', 2, 0, 8, self.revision))
        elif op == OP_CAROUSEL_LIST:
            start = args[0]
            record = struct.pack('<BHHIHBHHIH', start, 16, 8, len(self.frame), crc16(self.frame), 2, 3, 2, len(self.thumb), crc16(self.thumb))
            self.rsp(seq, struct.pack('<IBB', self.revision + (self.read_fault == 'list_revision'), start, 1) + record)
        elif op == OP_CAROUSEL_READ:
            slot, kind, offset, requested, revision = struct.unpack('<BBIHI', args)
            if self.read_fault == 'stale':
                self.rsp(seq, status=ST_STALE)
                return
            assert revision == self.revision
            src = self.thumb if kind else self.frame
            part = src[offset:offset + min(requested, 7)]
            if self.read_fault == 'crc' and offset == 0:
                part = bytes([part[0] ^ 1]) + part[1:]
            elif self.read_fault == 'empty':
                part = b''
            off = offset + (self.read_fault == 'offset')
            self.rsp(seq, struct.pack('<BBIIHB', slot, kind, off, len(src), crc16(src), 2 if kind else 1) + part)
        elif op in (OP_CAROUSEL_DELETE, OP_CAROUSEL_CLEAR):
            self.event(EVT_STORE_DONE, self.store_error, bytes([args[0] if args else 255, 1, 0]))
            self.rsp(seq)
        elif op == OP_CAROUSEL_SHOW:
            self.event(EVT_DISPLAY_DONE, 4)
            self.rsp(seq)
        elif op == OP_GET_STATUS:
            payload = bytearray(27)
            payload[0] = 1
            payload[1] = 3
            payload[26] = len(b'B2.0.1')
            payload.extend(b'B2.0.1')
            if self.status_flags is not None:
                payload.append(self.status_flags)
            self.rsp(seq, payload)
        elif op == OP_GET_SETTINGS:
            name = '墨印'.encode()
            payload = struct.pack('<BIBIB8sBB', 0, 180, 0, 3600, 0, bytes(range(8)), 1, len(name)) + name
            self.rsp(seq, payload + (b'' if self.old_settings else b'\x01'))
        elif op == OP_SET_SETTINGS:
            self.rsp(seq, bytes([1, 0, 0, 0]))
        elif op == OP_SLEEP_NOW and self.sleep_busy:
            self.rsp(seq, status=ST_BUSY)
        else:
            self.rsp(seq)


class ProtocolTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.mock = MockBle()
        self.cli = self.mock.cli

    async def test_status_optional_gallery_window_flags(self):
        old = await self.cli.get_status()
        self.assertEqual(old['version'], 'B2.0.1')
        self.assertEqual(old['gallery_read_flags'], 0)
        self.assertFalse(old['gallery_read_supported'])
        for flags in (1, 3, 7):
            self.mock.status_flags = flags
            status = await self.cli.get_status()
            self.assertEqual(status['gallery_read_flags'], flags)
            self.assertTrue(status['gallery_read_supported'])
            self.assertEqual(status['gallery_read_window'], bool(flags & 2))
            self.assertEqual(status['gallery_append_committed'], bool(flags & 4))

    async def test_gallery_read_496_cap_with_mtu_517(self):
        self.cli.mtu = 517
        self.mock.frame = bytes(range(256)) * 4
        self.assertEqual(await self.cli.gallery_download(0), self.mock.frame)
        requests = [struct.unpack_from('<H', args, 6)[0]
                    for op, args in self.mock.commands if op == OP_CAROUSEL_READ]
        self.assertEqual(max(requests), 496)
        self.assertTrue(all(0 < n <= 496 for n in requests))

    async def test_upload_legacy_wire_and_retry(self):
        self.mock.end_statuses = [ST_INCOMPLETE, ST_CRC, ST_OK]
        await self.cli.send_frame(self.mock.frame, 16, 8, wait_display=True)
        begin = self.mock.commands[0][1]
        self.assertEqual(len(begin), 13)
        self.assertEqual(begin[0], 0)
        self.assertEqual(self.mock.received, self.mock.frame)
        self.assertEqual(self.cli._events, [])

    async def test_store_only_jpeg_and_event_before_rsp(self):
        await self.cli.send_frame(self.mock.frame, 16, 8, save_gallery=True, store_only=True,
                                  thumbnail=self.mock.thumb, thumbnail_w=3, thumbnail_h=2)
        begin = self.mock.commands[0][1]
        self.assertEqual(len(begin), 24)
        self.assertEqual(begin[0], 6)
        self.assertEqual(struct.unpack_from('<BHHIH', begin, 13), (2, 3, 2, len(self.mock.thumb), crc16(self.mock.thumb)))
        self.assertEqual(self.mock.received, self.mock.frame + self.mock.thumb)
        self.assertEqual(self.cli._events, [])

    async def test_store_failure_propagates(self):
        self.mock.store_error = ST_FULL
        with self.assertRaises(MoinkError) as ctx:
            await self.cli.send_frame(self.mock.frame, 16, 8, save_gallery=True, store_only=True)
        self.assertEqual(ctx.exception.status, ST_FULL)
        self.assertEqual(self.cli._events, [])

    async def test_repeated_gap_is_bounded_and_aborts(self):
        self.mock.force_gap = True
        with self.assertRaises(MoinkError):
            await asyncio.wait_for(self.cli.send_frame(self.mock.frame, 16, 8), 1)
        self.assertEqual(self.mock.abort_count, 1)

    async def test_gallery_pages_and_full_thumbnail_download(self):
        rows = await self.cli.gallery_list()
        self.assertEqual([r['slot'] for r in rows], [0, 1])
        self.assertEqual(await self.cli.gallery_download(0), self.mock.frame)
        self.assertEqual(await self.cli.gallery_download(1, thumbnail=True), self.mock.thumb)

    async def test_read_rejects_offset_empty_crc_and_stale(self):
        for fault in ('offset', 'empty', 'crc', 'stale', 'list_revision'):
            with self.subTest(fault=fault):
                self.mock.read_fault = fault
                with self.assertRaises((ValueError, MoinkError)):
                    await self.cli.gallery_download(0)

    async def test_mutations_and_next_fast_completion(self):
        await self.cli.gallery_show(0)
        await self.cli.gallery_next()
        self.assertIn((OP_CAROUSEL_SHOW, b'\xff'), self.mock.commands)
        self.assertEqual((await self.cli.gallery_delete(1))['count'], 1)
        await self.cli.gallery_clear()
        self.assertEqual(self.cli._events, [])

    async def test_mutation_failure_propagates(self):
        self.mock.store_error = ST_FLASH_ERROR
        with self.assertRaises(MoinkError):
            await self.cli.gallery_clear()
        self.assertEqual(self.cli._events, [])

    async def test_wake_show_and_legacy_settings(self):
        settings = await self.cli.get_settings()
        self.assertEqual(settings['name'], '墨印')
        self.assertTrue(settings['wake_show'])
        self.mock.old_settings = True
        self.assertFalse((await self.cli.get_settings())['wake_show'])
        await self.cli.set_settings(wake_show=True)
        self.assertEqual(self.mock.commands[-1], (OP_SET_SETTINGS, b'\x09\x01\x01'))

    async def test_ota_incomplete_end_recovers(self):
        self.mock.end_statuses = [ST_INCOMPLETE, ST_OK]
        await self.cli.ota(self.mock.frame)
        self.assertEqual(self.mock.received, self.mock.frame)
        self.assertEqual(sum(op == OP_OTA_END for op, _ in self.mock.commands), 2)

    async def test_ota_repeated_gap_is_bounded_and_aborts(self):
        self.mock.force_gap = True
        with self.assertRaises(MoinkError):
            await asyncio.wait_for(self.cli.ota(self.mock.frame), 1)
        self.assertEqual(self.mock.commands[-1][0], OP_OTA_ABORT)

    async def test_ota_verification_failure_aborts(self):
        self.mock.end_statuses = [ST_OTA_VERIFY]
        with self.assertRaises(MoinkError) as ctx:
            await self.cli.ota(self.mock.frame)
        self.assertEqual(ctx.exception.status, ST_OTA_VERIFY)
        self.assertEqual(self.mock.commands[-1][0], OP_OTA_ABORT)

    async def test_sleep_busy_is_not_silently_successful(self):
        self.mock.sleep_busy = True
        with self.assertRaises(MoinkError) as ctx:
            await self.cli.sleep_now()
        self.assertEqual(ctx.exception.status, ST_BUSY)

    async def test_invalid_store_arguments_do_not_send(self):
        with self.assertRaises(ValueError):
            await self.cli.send_frame(self.mock.frame, 16, 8, store_only=True)
        with self.assertRaises(ValueError):
            await self.cli.send_frame(self.mock.frame, 16, 8, thumbnail=b'abc')
        self.assertEqual(self.mock.commands, [])


if __name__ == '__main__':
    unittest.main()
