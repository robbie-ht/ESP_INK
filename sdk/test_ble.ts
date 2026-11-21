/** Offline mock transport tests; Node >= 22.7: node --experimental-transform-types --test sdk/test_ble.ts */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { MoinkBle, OP, ST, EVT, crc16, type BleTransport } from './moink_ble.ts';

class MockTransport implements BleTransport {
  mtu = 64;
  callback: (data: Uint8Array) => void = () => {};
  commands: Array<[number, Uint8Array]> = [];
  frame = Uint8Array.from({ length: 32 }, (_, i) => i);
  thumb = new Uint8Array([255, 216, 11, 12, 13, 255, 217]);
  received: number[] = [];
  revision = 7;
  readFault = '';
  storeError: number = ST.OK;
  endStatuses: number[] = [];
  forceGap = false;
  oldSettings = false;
  sleepBusy = false;
  flags = 0;
  statusFlags: number | undefined;
  async connect() {}
  async disconnect() {}
  onNotify(cb: (data: Uint8Array) => void) { this.callback = cb; }
  rsp(seq: number, payload = new Uint8Array(), status: number = ST.OK) {
    this.callback(new Uint8Array([1, seq, status, ...payload]));
  }
  event(evt: number, arg = 0, payload = new Uint8Array()) {
    this.callback(new Uint8Array([0x81, evt, arg, ...payload]));
  }
  async writeData(d: Uint8Array) {
    const seq = d[0] | (d[1] << 8);
    if (this.forceGap) { this.event(EVT.TRANSFER_GAP, 0, new Uint8Array([0, 0])); return; }
    this.received.splice(seq * 16);
    this.received.push(...d.subarray(2));
  }
  async writeCtrl(d: Uint8Array) {
    const op = d[0], seq = d[1], a = d.subarray(2);
    this.commands.push([op, a]);
    if (op === OP.FRAME_BEGIN || op === OP.OTA_BEGIN) {
      this.flags = a[0]; this.received = [];
      this.rsp(seq, new Uint8Array([16, 0, 59, 0]));
    } else if (op === OP.FRAME_END) {
      const status = this.endStatuses.shift() ?? ST.OK;
      if (status !== ST.OK) { this.rsp(seq, new Uint8Array([0, 0]), status); return; }
      if (this.flags & 2) this.event(EVT.STORE_DONE, this.storeError, new Uint8Array([0, 1, 0]));
      if (!(this.flags & 4)) this.event(EVT.DISPLAY_DONE);
      this.rsp(seq, new Uint8Array([1]));
    } else if (op === OP.OTA_END) {
      const status = this.endStatuses.shift() ?? ST.OK;
      this.rsp(seq, status === ST.INCOMPLETE ? new Uint8Array([0, 0]) : undefined, status);
    } else if (op === OP.CAROUSEL_INFO) {
      const p = new Uint8Array([2, 0, 8, 0, 0, 0, 0]);
      new DataView(p.buffer).setUint32(3, this.revision, true); this.rsp(seq, p);
    } else if (op === OP.CAROUSEL_LIST) {
      const p = new Uint8Array(28), v = new DataView(p.buffer);
      v.setUint32(0, this.revision + (this.readFault === 'list_revision' ? 1 : 0), true);
      p[4] = a[0]; p[5] = 1; p[6] = a[0];
      v.setUint16(7, 16, true); v.setUint16(9, 8, true);
      v.setUint32(11, this.frame.length, true); v.setUint16(15, crc16(this.frame), true);
      p[17] = 2; v.setUint16(18, 3, true); v.setUint16(20, 2, true);
      v.setUint32(22, this.thumb.length, true); v.setUint16(26, crc16(this.thumb), true);
      this.rsp(seq, p);
    } else if (op === OP.CAROUSEL_READ) {
      if (this.readFault === 'stale') { this.rsp(seq, undefined, ST.STALE); return; }
      const av = new DataView(a.buffer, a.byteOffset, a.byteLength);
      const offset = av.getUint32(2, true), count = av.getUint16(6, true);
      assert.equal(av.getUint32(8, true), this.revision);
      const src = a[1] ? this.thumb : this.frame;
      const block = this.readFault === 'empty' ? new Uint8Array() : src.slice(offset, offset + Math.min(count, 7));
      if (this.readFault === 'crc' && offset === 0) block[0] ^= 1;
      const p = new Uint8Array(13 + block.length), v = new DataView(p.buffer);
      p[0] = a[0]; p[1] = a[1]; v.setUint32(2, offset + (this.readFault === 'offset' ? 1 : 0), true);
      v.setUint32(6, src.length, true); v.setUint16(10, crc16(src), true); p[12] = a[1] ? 2 : 1;
      p.set(block, 13); this.rsp(seq, p);
    } else if (op === OP.CAROUSEL_SHOW) {
      this.event(EVT.DISPLAY_DONE, 4); this.rsp(seq);
    } else if ([OP.CAROUSEL_DELETE, OP.CAROUSEL_CLEAR].includes(op as any)) {
      this.event(EVT.STORE_DONE, this.storeError, new Uint8Array([a[0] ?? 255, 1, 0])); this.rsp(seq);
    } else if (op === OP.GET_STATUS) {
      const version = new TextEncoder().encode('B2.0.1');
      const p = new Uint8Array(27 + version.length + (this.statusFlags === undefined ? 0 : 1));
      p[0] = 1; p[1] = 3; p[26] = version.length; p.set(version, 27);
      if (this.statusFlags !== undefined) p[p.length - 1] = this.statusFlags;
      this.rsp(seq, p);
    } else if (op === OP.GET_SETTINGS) {
      const name = new TextEncoder().encode('墨印');
      const p = new Uint8Array(21 + name.length + (this.oldSettings ? 0 : 1));
      p[19] = 1; p[20] = name.length; p.set(name, 21);
      if (!this.oldSettings) p[p.length - 1] = 1;
      this.rsp(seq, p);
    } else if (op === OP.SET_SETTINGS) {
      this.rsp(seq, new Uint8Array([1, 0, 0, 0]));
    } else if (op === OP.SLEEP_NOW && this.sleepBusy) {
      this.rsp(seq, undefined, ST.BUSY);
    } else this.rsp(seq);
  }
}

function setup() { const t = new MockTransport(); return { t, cli: new MoinkBle(t) }; }

test('GET_STATUS optional gallery window flags preserve old response compatibility', async () => {
  const { t, cli } = setup();
  const old = await cli.getStatus();
  assert.equal(old.version, 'B2.0.1'); assert.equal(old.galleryReadFlags, 0);
  assert.equal(old.galleryReadSupported, false);
  for (const flags of [1, 3, 7]) {
    t.statusFlags = flags; const status = await cli.getStatus();
    assert.equal(status.galleryReadFlags, flags); assert.equal(status.galleryReadSupported, true);
    assert.equal(status.galleryReadWindow, (flags & 2) !== 0);
    assert.equal(status.galleryAppendCommitted, (flags & 4) !== 0);
  }
});

test('gallery download caps payload at 496 even with MTU 517', async () => {
  const { t, cli } = setup(); t.mtu = 517;
  t.frame = Uint8Array.from({ length: 1024 }, (_, i) => i & 255);
  assert.deepEqual(await cli.galleryDownload(0), t.frame);
  const requests = t.commands.filter(([op]) => op === OP.CAROUSEL_READ)
    .map(([, a]) => new DataView(a.buffer, a.byteOffset, a.byteLength).getUint16(6, true));
  assert.equal(Math.max(...requests), 496); assert.ok(requests.every((n) => n > 0 && n <= 496));
});

test('legacy frame wire, CRC/incomplete recovery and event before response', async () => {
  const { t, cli } = setup(); t.endStatuses = [ST.INCOMPLETE, ST.CRC, ST.OK];
  await cli.sendFrame(t.frame, 16, 8, { waitDisplay: true });
  assert.equal(t.commands[0][1].length, 13); assert.equal(t.commands[0][1][0], 0);
  assert.deepEqual(new Uint8Array(t.received), t.frame);
});
test('store-only upload serializes JPEG extension and waits fast STORE_DONE', async () => {
  const { t, cli } = setup();
  await cli.sendFrame(t.frame, 16, 8, { saveGallery: true, storeOnly: true, thumbnail: t.thumb, thumbnailW: 3, thumbnailH: 2 });
  const a = t.commands[0][1], v = new DataView(a.buffer, a.byteOffset, a.byteLength);
  assert.equal(a.length, 24); assert.equal(a[0], 6); assert.equal(a[13], 2);
  assert.equal(v.getUint32(18, true), t.thumb.length); assert.equal(v.getUint16(22, true), crc16(t.thumb));
  assert.deepEqual(t.received, [...t.frame, ...t.thumb]);
});
test('store failure rejects', async () => {
  const { t, cli } = setup(); t.storeError = ST.FULL;
  await assert.rejects(cli.sendFrame(t.frame, 16, 8, { saveGallery: true, storeOnly: true }), { status: ST.FULL });
});
test('persistent gap retry bounded and aborts transfer', async () => {
  const { t, cli } = setup(); t.forceGap = true;
  await assert.rejects(cli.sendFrame(t.frame, 16, 8), { status: ST.SEQ_GAP });
  assert.equal(t.commands.at(-1)![0], OP.FRAME_ABORT);
});
test('paged gallery list and full/thumbnail download', async () => {
  const { t, cli } = setup();
  assert.deepEqual((await cli.galleryList()).map((r) => r.slot), [0, 1]);
  assert.deepEqual(await cli.galleryDownload(0), t.frame);
  assert.deepEqual(await cli.galleryDownload(1, true), t.thumb);
});
test('read rejects identity/offset, empty packets, CRC and stale revision', async () => {
  for (const fault of ['offset', 'empty', 'crc', 'stale', 'list_revision']) {
    const { t, cli } = setup(); t.readFault = fault; await assert.rejects(cli.galleryDownload(0));
  }
});
test('gallery show/next/delete/clear completion may arrive before response', async () => {
  const { t, cli } = setup();
  await cli.galleryShow(0); await cli.galleryNext();
  assert.equal(t.commands.at(-1)![1][0], 255);
  assert.equal((await cli.galleryDelete(1)).count, 1); await cli.galleryClear();
  t.storeError = ST.FLASH_ERROR; await assert.rejects(cli.galleryClear(), { status: ST.FLASH_ERROR });
});
test('settings name offset 21 and optional wakeShow extension', async () => {
  const { t, cli } = setup();
  assert.equal((await cli.getSettings()).name, '墨印'); assert.equal((await cli.getSettings()).wakeShow, true);
  t.oldSettings = true; assert.equal((await cli.getSettings()).wakeShow, false);
  await cli.setSettings({ wakeShow: true }); assert.deepEqual(t.commands.at(-1)![1], new Uint8Array([9, 1, 1]));
});
test('invalid store-only and thumbnail inputs do not send', async () => {
  const { t, cli } = setup();
  await assert.rejects(cli.sendFrame(t.frame, 16, 8, { storeOnly: true }));
  await assert.rejects(cli.sendFrame(t.frame, 16, 8, { thumbnail: t.thumb }));
  assert.equal(t.commands.length, 0);
});

test('OTA incomplete END recovers without restarting session', async () => {
  const { t, cli } = setup(); t.endStatuses = [ST.INCOMPLETE, ST.OK];
  await cli.ota(t.frame);
  assert.deepEqual(new Uint8Array(t.received), t.frame);
  assert.equal(t.commands.filter(([op]) => op === OP.OTA_END).length, 2);
});
test('OTA persistent gap is bounded and aborts', async () => {
  const { t, cli } = setup(); t.forceGap = true;
  await assert.rejects(cli.ota(t.frame), { status: ST.SEQ_GAP });
  assert.equal(t.commands.at(-1)![0], OP.OTA_ABORT);
});
test('OTA verification failure aborts', async () => {
  const { t, cli } = setup(); t.endStatuses = [ST.OTA_VERIFY];
  await assert.rejects(cli.ota(t.frame), { status: ST.OTA_VERIFY });
  assert.equal(t.commands.at(-1)![0], OP.OTA_ABORT);
});

test('SLEEP_NOW busy is propagated', async () => {
  const { t, cli } = setup(); t.sleepBusy = true;
  await assert.rejects(cli.sleepNow(), { status: ST.BUSY });
});
