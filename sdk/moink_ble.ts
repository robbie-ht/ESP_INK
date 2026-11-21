/**
 * MoInk BLE 协议 —— TypeScript / JavaScript 参考客户端
 * =====================================================
 *
 * 两套用法：
 *   1) 支持 Web Bluetooth 的浏览器：可用 WebBluetoothTransport（webview 需先检测支持）
 *   2) App（uni-app 原生 / React Native / Capacitor）：自己实现 BleTransport 接口，
 *      把原生插件的回调往 onNotify 里塞即可 —— 协议层完全复用本文件
 *
 * 协议权威定义见 docs/API.md。原生接入细节见 docs/APP-INTEGRATION.md。
 *
 * 约定：所有整数 **小端序**；CRC16 = CCITT-FALSE（poly 0x1021, init 0xFFFF）。
 */

/* ------------------------------------------------------------------ 常量 */

export const SVC_UUID = '6d6f496e-4b00-4000-8000-000000000000';
export const CTRL_UUID = '6d6f496e-4b00-4000-8000-000000000001';
export const DATA_UUID = '6d6f496e-4b00-4000-8000-000000000002';
export const EVT_UUID = '6d6f496e-4b00-4000-8000-000000000003';

export const OP = {
  PING: 0x01,
  GET_STATUS: 0x02,
  GET_SETTINGS: 0x03,
  SET_SETTINGS: 0x04,
  FRAME_BEGIN: 0x10,
  FRAME_END: 0x12,
  FRAME_ABORT: 0x13,
  CACHE_SHOW: 0x20,
  CACHE_INFO: 0x21,
  CAROUSEL_INFO: 0x22,
  CAROUSEL_SHOW: 0x23,
  CAROUSEL_DELETE: 0x24,
  CAROUSEL_CLEAR: 0x25,
  CAROUSEL_LIST: 0x26,
  CAROUSEL_READ: 0x27,
  GHOST_CLEAR: 0x30,
  OTA_BEGIN: 0x40,
  OTA_END: 0x42,
  OTA_ABORT: 0x43,
  REBOOT: 0x50,
  FACTORY_RESET: 0x51,
  SLEEP_NOW: 0x52,
} as const;

export const ST = {
  OK: 0x00,
  UNKNOWN_OP: 0x01,
  BAD_ARG: 0x02,
  BAD_STATE: 0x03,
  BUSY: 0x04,
  NO_MEM: 0x05,
  GEOMETRY: 0x06,
  LEN_MISMATCH: 0x07,
  CRC: 0x08,
  INCOMPLETE: 0x09,
  SEQ_GAP: 0x0a,
  FLASH_ERROR: 0x0b,
  OTA_VERIFY: 0x0c,
  NOT_READY: 0x0d,
  UNSUPPORTED: 0x0e,
  FULL: 0x0f,
  STALE: 0x10,
} as const;

export const ST_NAME: Record<number, string> = {
  0x00: 'OK', 0x01: 'UNKNOWN_OP', 0x02: 'BAD_ARG', 0x03: 'BAD_STATE',
  0x04: 'BUSY', 0x05: 'NO_MEM', 0x06: 'GEOMETRY', 0x07: 'LEN_MISMATCH',
  0x08: 'CRC', 0x09: 'INCOMPLETE', 0x0a: 'SEQ_GAP', 0x0b: 'FLASH_ERROR',
  0x0c: 'OTA_VERIFY', 0x0d: 'NOT_READY', 0x0e: 'UNSUPPORTED',
  0x0f: 'FULL', 0x10: 'STALE',
};

export const EVT = {
  DISPLAY_DONE: 0x01,
  BUTTON: 0x02,
  LOW_BATTERY: 0x03,
  STATE: 0x04,
  TRANSFER_GAP: 0x05,
  STORE_DONE: 0x06,
} as const;

/** 色码（2bit） */
export const COLOR = { BLACK: 0, WHITE: 1, YELLOW: 2, RED: 3 } as const;

export const FRAME_W = 768;
export const FRAME_H = 552;
export const FRAME_LEN = (FRAME_W / 4) * FRAME_H; // 105984

export const STATE_NAME: Record<number, string> = {
  0: 'idle', 1: 'recv_frame', 2: 'recv_ota', 3: 'refreshing', 4: 'clearing', 6: 'sleeping',
};

/* ------------------------------------------------------------------ 错误 */

export class MoinkError extends Error {
  constructor(
    public status: number,
    public op: string,
    public detail?: Uint8Array,
  ) {
    super(`${op}: ${ST_NAME[status] ?? '0x' + status.toString(16)}`);
    this.name = 'MoinkError';
  }
}

/* ------------------------------------------------------------------ CRC16 */

const CRC_TABLE = (() => {
  const t = new Uint16Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i << 8;
    for (let b = 0; b < 8; b++) c = c & 0x8000 ? ((c << 1) ^ 0x1021) & 0xffff : (c << 1) & 0xffff;
    t[i] = c;
  }
  return t;
})();

/** CRC-16/CCITT-FALSE。金标：crc16("123456789") === 0x29B1 */
export function crc16(data: Uint8Array): number {
  let crc = 0xffff;
  for (let i = 0; i < data.length; i++) {
    crc = ((crc << 8) & 0xffff) ^ CRC_TABLE[((crc >> 8) ^ data[i]) & 0xff];
  }
  return crc;
}

/** CRC-32/ISO-HDLC = Python zlib.crc32（OTA 用） */
export function crc32(data: Uint8Array): number {
  let crc = 0xffffffff;
  for (let i = 0; i < data.length; i++) {
    crc ^= data[i];
    for (let b = 0; b < 8; b++) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return (crc ^ 0xffffffff) >>> 0;
}

/* ------------------------------------------------------------------ 2bpp 打包 */

/**
 * 把「正向、自上而下、左到右」的 2bit 色码打成固件要的帧缓冲。
 *
 * 为什么旋转 180°：面板从上往下扫描，但固件按「缓冲自下而上 + 逐行整行反转」
 * 的契约取数（epd_drv.c 的 row_mirror / H-1-j）。所以交给固件的缓冲必须是
 * 正向图旋转 180°，这里一次做完，上层只画正常的图。
 *
 * 字节内布局：4 像素/字节，MSB 优先，像素 x 在位 (6 - 2*(x%4))。
 */
export function pack2bpp(
  codes: Uint8Array | Uint8ClampedArray,
  w = FRAME_W,
  h = FRAME_H,
): Uint8Array {
  if (codes.length !== w * h) throw new Error(`codes 长度应为 ${w * h}，实际 ${codes.length}`);
  if (w % 4 !== 0) throw new Error('宽度必须是 4 的倍数');

  const rowBytes = w >> 2;
  const out = new Uint8Array(rowBytes * h);

  for (let y = 0; y < h; y++) {
    const dst = (h - 1 - y) * rowBytes;
    const src = y * w;
    for (let x = 0; x < w; x++) {
      const c = codes[src + x] & 3;
      const sx = w - 1 - x;
      out[dst + (sx >> 2)] |= c << (6 - 2 * (sx & 3));
    }
  }
  return out;
}

export function unpack2bpp(buf: Uint8Array, w = FRAME_W, h = FRAME_H): Uint8Array {
  const rowBytes = w >> 2;
  const out = new Uint8Array(w * h);
  for (let y = 0; y < h; y++) {
    const row = (h - 1 - y) * rowBytes;
    for (let x = 0; x < w; x++) {
      const sx = w - 1 - x;
      out[y * w + x] = (buf[row + (sx >> 2)] >> (6 - 2 * (sx & 3))) & 3;
    }
  }
  return out;
}

/** 整屏单色帧（连通性自测用，比跑一张真图快得多） */
export function solidFrame(color: number = COLOR.WHITE, w = FRAME_W, h = FRAME_H): Uint8Array {
  const codes = new Uint8Array(w * h).fill(color & 3);
  return pack2bpp(codes, w, h);
}

/* ------------------------------------------------------------------ 传输抽象 */

export interface BleTransport {
  connect(opts?: { namePrefix?: string; serviceUuid?: string }): Promise<void>;
  disconnect(): Promise<void>;
  /** 协商后的 ATT MTU（未知时返回 23，协议层会自动降到 18 字节分片） */
  readonly mtu: number;
  writeCtrl(data: Uint8Array): Promise<void>;
  writeData(data: Uint8Array): Promise<void>;
  onNotify(cb: (data: Uint8Array) => void): void;
}

/* ------------------------------------------------------------------ Web Bluetooth 实现 */

type AnyBluetooth = any;

export class WebBluetoothTransport implements BleTransport {
  private device: AnyBluetooth = null;
  private server: AnyBluetooth = null;
  private ctrl: AnyBluetooth = null;
  private data: AnyBluetooth = null;
  private evt: AnyBluetooth = null;
  private notifyCb: ((d: Uint8Array) => void) | null = null;
  private mtuValue = 23;

  static get supported(): boolean {
    return typeof navigator !== 'undefined' && !!(navigator as any).bluetooth;
  }

  get mtu(): number {
    return this.mtuValue;
  }

  async connect(opts: { namePrefix?: string; serviceUuid?: string } = {}): Promise<void> {
    const bt = (navigator as any).bluetooth as AnyBluetooth;
    if (!bt) throw new Error('当前环境不支持 Web Bluetooth（iOS Safari 不支持，请用原生插件）');
    const serviceUuid = opts.serviceUuid ?? SVC_UUID;

    // 名字在扫描响应里（广播包 31 字节装不下 128-bit UUID + 名字），
    // 所以用 namePrefix 过滤，而不是 service 过滤 —— 后者要求广播包含 UUID，两者只能取一。
    this.device = await bt.requestDevice({
      filters: [{ namePrefix: opts.namePrefix ?? 'MoInk' }],
      optionalServices: [serviceUuid],
    });
    this.device.addEventListener('gattserverdisconnected', () => {
      this.server = null;
    });

    this.server = await this.device.gatt.connect();
    const svc = await this.server.getPrimaryService(serviceUuid);
    this.ctrl = await svc.getCharacteristic(CTRL_UUID);
    this.data = await svc.getCharacteristic(DATA_UUID);
    this.evt = await svc.getCharacteristic(EVT_UUID);

    this.evt.addEventListener('characteristicvaluechanged', (e: any) => {
      const v: DataView = e.target.value;
      const out = new Uint8Array(v.byteLength);
      for (let i = 0; i < out.length; i++) out[i] = v.getUint8(i);
      this.notifyCb?.(out);
    });
    await this.evt.startNotifications();

    // 浏览器不暴露 MTU。这里仅给出建议值，必须使用 BEGIN 响应里的 chunk_used。
    // iOS Safari 本身不支持 Web Bluetooth；具有该 API 的其它宿主也须现场验证。
    const ua = typeof navigator !== 'undefined' ? navigator.userAgent : '';
    this.mtuValue = /iPhone|iPad|iPod/.test(ua) ? 185 : 517;
  }

  async disconnect(): Promise<void> {
    try {
      await this.evt?.stopNotifications();
    } catch {
      /* ignore */
    }
    try {
      if (this.server?.connected) this.device?.gatt?.disconnect();
    } catch {
      /* ignore */
    }
    this.server = null;
    this.device = null;
  }

  onNotify(cb: (data: Uint8Array) => void): void {
    this.notifyCb = cb;
  }

  async writeCtrl(data: Uint8Array): Promise<void> {
    if (this.ctrl.writeValueWithResponse) await this.ctrl.writeValueWithResponse(data);
    else await this.ctrl.writeValue(data);
  }

  async writeData(data: Uint8Array): Promise<void> {
    // Write Without Response：传 100KB+ 时吞吐差 5~10 倍
    if (this.data.writeValueWithoutResponse) await this.data.writeValueWithoutResponse(data);
    else await this.data.writeValue(data);
  }
}

/* ------------------------------------------------------------------ 协议客户端 */

export interface StatusInfo {
  api: number;
  state: string;
  panel: number;
  batteryMv: number | null;
  uptimeS: number;
  heapFree: number;
  mtu: number;
  wakeReason: number;
  sleepS: number;
  timeoutAction: number;
  autoWakeS: number;
  cacheValid: boolean;
  version: string;
}

export interface SettingsInfo {
  panel: number;
  sleepS: number;
  timeoutAction: number;
  autoWakeS: number;
  gpioPreset: number;
  pins: number[];
  cacheOn: boolean;
  name: string;
  wakeShow: boolean;
}

export interface GalleryInfo { count: number; current: number | null; slots: number; revision: number }
export interface GalleryItem {
  slot: number; w: number; h: number; len: number; crc16: number;
  thumbnailFormat: number; thumbnailW: number; thumbnailH: number;
  thumbnailLen: number; thumbnailCrc16: number; revision: number;
}
export interface FrameOptions {
  saveCache?: boolean; onProgress?: (p: number) => void; timeoutMs?: number;
  saveGallery?: boolean; storeOnly?: boolean; waitDisplay?: boolean;
  thumbnail?: Uint8Array; thumbnailFormat?: 1 | 2; thumbnailW?: number; thumbnailH?: number;
}

export type EventHandler = (evt: number, arg: number, payload: Uint8Array) => void;

export class MoinkBle {
  private seq = 0;
  private pending = new Map<number, { resolve: (v: [number, Uint8Array]) => void; reject: (e: Error) => void }>();
  private handlers: EventHandler[] = [];
  private gapAt: number | null = null;
  private lastState = 0;

  constructor(public readonly t: BleTransport) {
    t.onNotify((d) => this.onNotify(d));
  }

  static async connectFirst(opts: { namePrefix?: string } = {}): Promise<MoinkBle> {
    const t = new WebBluetoothTransport();
    await t.connect(opts);
    const cli = new MoinkBle(t);
    await cli.ping();
    return cli;
  }

  get mtu(): number {
    return this.t.mtu;
  }

  get chunkSize(): number {
    return Math.max(16, Math.min((this.t.mtu || 23) - 5, 512));
  }

  async disconnect(): Promise<void> {
    for (const p of this.pending.values()) p.reject(new Error('BLE disconnected'));
    this.pending.clear();
    await this.t.disconnect();
  }

  on(handler: EventHandler): void {
    this.handlers.push(handler);
  }

  /* ------------------------------ 底层 ------------------------------ */

  private onNotify(d: Uint8Array): void {
    if (d.length < 1) return;
    const kind = d[0];

    if (kind === 0x01 && d.length >= 3) {
      const seq = d[1];
      const status = d[2];
      const payload = d.slice(3);
      const p = this.pending.get(seq);
      if (p) {
        this.pending.delete(seq);
        p.resolve([status, payload]);
      }
      return;
    }

    if (kind === 0x81 && d.length >= 3) {
      const evt = d[1];
      const arg = d[2];
      const payload = d.slice(3);
      if (evt === EVT.TRANSFER_GAP && payload.length >= 2) {
        this.gapAt = payload[0] | (payload[1] << 8);
      }
      if (evt === EVT.STATE) this.lastState = arg;
      for (const h of [...this.handlers]) {
        try {
          h(evt, arg, payload);
        } catch {
          /* 单个回调异常不影响协议层 */
        }
      }
    }
  }

  private nextSeq(): number {
    for (let i = 0; i < 256; i++) {
      this.seq = (this.seq + 1) & 0xff;
      if (!this.pending.has(this.seq)) return this.seq;
    }
    throw new Error('too many pending BLE commands');
  }

  private eventWaiter(event: number, accepted?: number[]) {
    let resolve!: (v: [number, Uint8Array]) => void;
    const promise = new Promise<[number, Uint8Array]>((r) => { resolve = r; });
    const handler: EventHandler = (evt, arg, payload) => {
      if (evt === event && (!accepted || accepted.includes(arg))) resolve([arg, payload]);
    };
    this.handlers.push(handler);
    return { promise, cleanup: () => { this.handlers = this.handlers.filter((h) => h !== handler); } };
  }

  private async waitResult(promise: Promise<[number, Uint8Array]>, timeoutMs: number, op: string, store = false): Promise<Uint8Array> {
    let timer: ReturnType<typeof setTimeout> | undefined;
    try {
      const [status, payload] = await Promise.race([promise,
        new Promise<never>((_, reject) => { timer = setTimeout(() => reject(new Error(`${op} timeout`)), timeoutMs); }),
      ]);
      if ((store && status !== ST.OK) || (!store && status === 1)) {
        throw new MoinkError(store ? status : ST.NOT_READY, op, payload);
      }
      return payload;
    } finally { if (timer !== undefined) clearTimeout(timer); }
  }

  private async cmd(op: number, args: Uint8Array = new Uint8Array(0), timeoutMs = 15000): Promise<[number, Uint8Array]> {
    const seq = this.nextSeq();
    const buf = new Uint8Array(2 + args.length);
    buf[0] = op;
    buf[1] = seq;
    buf.set(args, 2);

    return new Promise<[number, Uint8Array]>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(seq);
        reject(new MoinkError(ST.NOT_READY, `op${op.toString(16)} 超时 ${timeoutMs}ms`));
      }, timeoutMs);

      this.pending.set(seq, {
        resolve: (v) => {
          clearTimeout(timer);
          resolve(v);
        },
        reject: (e) => {
          clearTimeout(timer);
          reject(e);
        },
      });

      this.t.writeCtrl(buf).catch((e) => {
        clearTimeout(timer);
        this.pending.delete(seq);
        reject(e);
      });
    });
  }

  private async call(op: number, args?: Uint8Array, timeoutMs?: number): Promise<Uint8Array> {
    const [status, payload] = await this.cmd(op, args, timeoutMs);
    if (status !== ST.OK) throw new MoinkError(status, `op${op.toString(16)}`, payload);
    return payload;
  }

  private async writeData(seq: number, chunk: Uint8Array): Promise<void> {
    const buf = new Uint8Array(2 + chunk.length);
    buf[0] = seq & 0xff;
    buf[1] = (seq >> 8) & 0xff;
    buf.set(chunk, 2);
    await this.t.writeData(buf);
  }

  /* ------------------------------ 命令 ------------------------------ */

  async ping(): Promise<{ api: number; version: string }> {
    const p = await this.call(OP.PING);
    const vl = p[1];
    return { api: p[0], version: new TextDecoder().decode(p.slice(2, 2 + vl)) };
  }

  async getStatus(): Promise<StatusInfo> {
    const p = await this.call(OP.GET_STATUS, undefined, 20000);
    const dv = new DataView(p.buffer, p.byteOffset, p.byteLength);
    const battery = dv.getInt16(3, true);
    const vl = p[26];
    return {
      api: p[0],
      state: STATE_NAME[p[1]] ?? String(p[1]),
      panel: p[2],
      batteryMv: battery > 0 ? battery : null,
      uptimeS: dv.getUint32(5, true),
      heapFree: dv.getUint32(9, true),
      mtu: dv.getUint16(13, true),
      wakeReason: p[15],
      sleepS: dv.getUint32(16, true),
      timeoutAction: p[20],
      autoWakeS: dv.getUint32(21, true),
      cacheValid: p[25] === 1,
      version: new TextDecoder().decode(p.slice(27, 27 + vl)),
    };
  }

  async getSettings(): Promise<SettingsInfo> {
    const p = await this.call(OP.GET_SETTINGS);
    const dv = new DataView(p.buffer, p.byteOffset, p.byteLength);
    if (p.length < 21 || p.length < 21 + p[20]) throw new Error('GET_SETTINGS truncated; negotiate a larger MTU');
    const nameLen = p[20];
    return {
      panel: p[0],
      sleepS: dv.getUint32(1, true),
      timeoutAction: p[5],
      autoWakeS: dv.getUint32(6, true),
      gpioPreset: p[10],
      pins: Array.from(p.slice(11, 19)),
      cacheOn: p[19] === 1,
      name: new TextDecoder().decode(p.slice(21, 21 + nameLen)),
      wakeShow: p.length > 21 + nameLen ? p[21 + nameLen] === 1 : false,
    };
  }

  /** 只改传入的字段。返回 {applied, errId, errCode, flags}。 */
  async setSettings(o: Partial<{
    panel: number;
    sleepS: number;
    timeoutAction: number;
    autoWakeS: number;
    name: string;
    gpioPreset: number;
    pins: number[];
    cacheOn: boolean;
    wakeShow: boolean;
  }>): Promise<{ applied: number; errId: number; errCode: number; flags: number }> {
    const parts: Uint8Array[] = [];
    const tlv = (id: number, payload: Uint8Array) => {
      const b = new Uint8Array(2 + payload.length);
      b[0] = id;
      b[1] = payload.length;
      b.set(payload, 2);
      parts.push(b);
    };
    const u32 = (v: number) => { const b = new Uint8Array(4); new DataView(b.buffer).setUint32(0, v, true); return b; };

    if (o.panel !== undefined) tlv(0x01, new Uint8Array([o.panel]));
    if (o.sleepS !== undefined) tlv(0x02, u32(o.sleepS));
    if (o.timeoutAction !== undefined) tlv(0x03, new Uint8Array([o.timeoutAction]));
    if (o.autoWakeS !== undefined) tlv(0x04, u32(o.autoWakeS));
    if (o.name !== undefined) tlv(0x05, new TextEncoder().encode(o.name));
    if (o.gpioPreset !== undefined) tlv(0x06, new Uint8Array([o.gpioPreset]));
    if (o.pins !== undefined) tlv(0x07, new Uint8Array(o.pins));
    if (o.cacheOn !== undefined) tlv(0x08, new Uint8Array([o.cacheOn ? 1 : 0]));
    if (o.wakeShow !== undefined) tlv(0x09, new Uint8Array([o.wakeShow ? 1 : 0]));

    const total = parts.reduce((n, p) => n + p.length, 0);
    const args = new Uint8Array(total);
    let off = 0;
    for (const p of parts) {
      args.set(p, off);
      off += p.length;
    }

    const [status, p] = await this.cmd(OP.SET_SETTINGS, args);
    const res = { applied: p[0] ?? 0, errId: p[1] ?? 0, errCode: p[2] ?? status, flags: p[3] ?? 0 };
    if (status !== ST.OK) throw new MoinkError(status, 'SET_SETTINGS', p);
    return res;
  }

  /* ------------------------------ 传图 ------------------------------ */

  /** 默认在排队后返回；waitDisplay 等刷完。saveGallery 等待落盘，storeOnly 不刷屏。 */
  async sendFrame(buf: Uint8Array, w = FRAME_W, h = FRAME_H, opts: FrameOptions = {}): Promise<void> {
    if (!Number.isInteger(w) || w <= 0 || h <= 0 || w % 4 || buf.length !== w / 4 * h) throw new Error('invalid frame geometry/length');
    if (opts.storeOnly && (!opts.saveGallery || opts.waitDisplay)) throw new Error('storeOnly requires saveGallery and cannot waitDisplay');
    const thumb = opts.thumbnail ?? new Uint8Array();
    const format = opts.thumbnailFormat ?? 2;
    const tw = opts.thumbnailW ?? 0, th = opts.thumbnailH ?? 0;
    if (thumb.length && (!opts.saveGallery || ![1, 2].includes(format) || tw <= 0 || th <= 0 || tw > 65535 || th > 65535 || thumb.length > 20000)) throw new Error('invalid thumbnail metadata');
    if (thumb.length && format === 1 && (tw % 4 || tw > 200 || th > 150 || thumb.length !== tw / 4 * th)) throw new Error('invalid packed thumbnail length');
    if (thumb.length && format === 2 && (thumb.length < 4 || thumb[0] !== 255 || thumb[1] !== 216 || thumb[thumb.length - 2] !== 255 || thumb[thumb.length - 1] !== 217)) throw new Error('invalid JPEG thumbnail markers');
    if (buf.length + thumb.length + 64 > 131072) throw new Error('frame and thumbnail exceed gallery slot capacity');
    const data = new Uint8Array(buf.length + thumb.length);
    data.set(buf); data.set(thumb, buf.length);
    const begin = new Uint8Array(thumb.length ? 24 : 13);
    begin[0] = (opts.saveCache === false ? 1 : 0) | (opts.saveGallery ? 2 : 0) | (opts.storeOnly ? 4 : 0);
    const dv = new DataView(begin.buffer);
    dv.setUint16(1, w, true); dv.setUint16(3, h, true); dv.setUint32(5, buf.length, true);
    dv.setUint16(9, crc16(buf), true); dv.setUint16(11, this.chunkSize, true);
    if (thumb.length) {
      begin[13] = format; dv.setUint16(14, tw, true); dv.setUint16(16, th, true);
      dv.setUint32(18, thumb.length, true); dv.setUint16(22, crc16(thumb), true);
    }
    const pl = await this.call(OP.FRAME_BEGIN, begin);
    if (pl.length < 2) throw new Error('truncated FRAME_BEGIN response');
    const chunk = pl[0] | (pl[1] << 8);
    if (chunk < 16 || chunk > this.chunkSize) throw new Error('invalid negotiated chunk size');
    const total = Math.ceil(data.length / chunk);
    this.gapAt = null;
    let idx = 0, retries = 0;
    // Arm before FRAME_END: a fast device may emit the completion before its RSP.
    const store = opts.saveGallery ? this.eventWaiter(EVT.STORE_DONE) : undefined;
    const display = opts.waitDisplay ? this.eventWaiter(EVT.DISPLAY_DONE, [0, 1]) : undefined;
    try {
      for (;;) {
        while (idx < total) {
          if (this.gapAt !== null) {
            const gap = this.gapAt; this.gapAt = null;
            if (gap < idx) { idx = gap; retries++; }
          }
          if (retries > 8) throw new MoinkError(ST.SEQ_GAP, 'FRAME');
          await this.writeData(idx, data.subarray(idx * chunk, (idx + 1) * chunk));
          idx++;
          if ((idx & 15) === 0) opts.onProgress?.(idx / total);
          if ((idx & 31) === 0) await new Promise((r) => setTimeout(r, 5));
        }
        const [status, payload] = await this.cmd(OP.FRAME_END, new Uint8Array(), opts.timeoutMs ?? 180000);
        if (status === ST.OK) {
          if (store) await this.waitResult(store.promise, opts.timeoutMs ?? 900000, 'STORE', true);
          if (display) await this.waitResult(display.promise, opts.timeoutMs ?? 900000, 'DISPLAY');
          opts.onProgress?.(1); return;
        }
        if (++retries > 8) throw new MoinkError(ST.SEQ_GAP, 'FRAME');
        if (status === ST.INCOMPLETE && payload.length >= 2) {
          idx = payload[0] | (payload[1] << 8);
          if (idx >= total) throw new Error('invalid FRAME_END resume offset');
          continue;
        }
        if (status === ST.CRC) { idx = 0; continue; }
        throw new MoinkError(status, 'FRAME_END', payload);
      }
    } catch (e) {
      try { await this.cmd(OP.FRAME_ABORT, new Uint8Array(), 5000); } catch { /* keep original error */ }
      throw e;
    } finally { store?.cleanup(); display?.cleanup(); }
  }

  /* ------------------------------ 设备图库 ------------------------------ */

  async galleryInfo(): Promise<GalleryInfo> {
    const p = await this.call(OP.CAROUSEL_INFO);
    if (p.length !== 7) throw new Error('invalid CAROUSEL_INFO response');
    return { count: p[0], current: p[1] === 255 ? null : p[1], slots: p[2],
      revision: new DataView(p.buffer, p.byteOffset, p.byteLength).getUint32(3, true) };
  }

  async galleryList(): Promise<GalleryItem[]> {
    const info = await this.galleryInfo();
    const items: GalleryItem[] = [];
    while (items.length < info.count) {
      const start = items.length;
      const p = await this.call(OP.CAROUSEL_LIST, new Uint8Array([start, info.count - start]));
      if (p.length < 6) throw new Error('truncated CAROUSEL_LIST response');
      const dv = new DataView(p.buffer, p.byteOffset, p.byteLength);
      const revision = dv.getUint32(0, true), count = p[5];
      if (revision !== info.revision) throw new MoinkError(ST.STALE, 'CAROUSEL_LIST');
      if (p[4] !== start || !count || count > info.count - start || p.length !== 6 + 22 * count) throw new Error('invalid CAROUSEL_LIST page');
      for (let i = 0; i < count; i++) {
        const o = 6 + 22 * i;
        if (p[o] !== start + i) throw new Error('invalid gallery slot order');
        items.push({ slot: p[o], w: dv.getUint16(o + 1, true), h: dv.getUint16(o + 3, true),
          len: dv.getUint32(o + 5, true), crc16: dv.getUint16(o + 9, true), thumbnailFormat: p[o + 11],
          thumbnailW: dv.getUint16(o + 12, true), thumbnailH: dv.getUint16(o + 14, true),
          thumbnailLen: dv.getUint32(o + 16, true), thumbnailCrc16: dv.getUint16(o + 20, true), revision });
      }
    }
    return items;
  }

  async galleryShow(slot: number, timeoutMs = 900000): Promise<void> {
    if (!Number.isInteger(slot) || slot < 0 || slot > 255) throw new Error('invalid slot');
    const wait = this.eventWaiter(EVT.DISPLAY_DONE, [1, 4]);
    try { await this.call(OP.CAROUSEL_SHOW, new Uint8Array([slot])); await this.waitResult(wait.promise, timeoutMs, 'CAROUSEL_SHOW'); }
    finally { wait.cleanup(); }
  }

  async galleryNext(timeoutMs = 900000): Promise<void> { await this.galleryShow(255, timeoutMs); }

  private async galleryMutation(op: number, args = new Uint8Array()) {
    const wait = this.eventWaiter(EVT.STORE_DONE);
    try {
      await this.call(op, args);
      const p = await this.waitResult(wait.promise, 60000, 'STORE', true);
      if (p.length !== 3) throw new Error('invalid STORE_DONE response');
      return { slot: p[0], count: p[1], current: p[2] === 255 ? null : p[2] };
    } finally { wait.cleanup(); }
  }

  async galleryDelete(slot: number) {
    if (!Number.isInteger(slot) || slot < 0 || slot > 255) throw new Error('invalid slot');
    return this.galleryMutation(OP.CAROUSEL_DELETE, new Uint8Array([slot]));
  }

  async galleryClear() { return this.galleryMutation(OP.CAROUSEL_CLEAR); }

  async galleryDownload(slot: number, thumbnail = false, onProgress?: (p: number) => void): Promise<Uint8Array> {
    const item = (await this.galleryList()).find((it) => it.slot === slot);
    if (!item) throw new MoinkError(ST.NOT_READY, 'CAROUSEL_READ');
    const total = thumbnail ? item.thumbnailLen : item.len;
    const expectedCrc = thumbnail ? item.thumbnailCrc16 : item.crc16;
    const format = thumbnail ? item.thumbnailFormat : 1;
    if (!total) throw new MoinkError(ST.NOT_READY, 'CAROUSEL_READ');
    const kind = thumbnail ? 1 : 0, data = new Uint8Array(total);
    let offset = 0;
    while (offset < total) {
      const requested = Math.min(Math.max(1, this.mtu - 19), total - offset, 65535);
      const args = new Uint8Array(12), ad = new DataView(args.buffer);
      args[0] = slot; args[1] = kind; ad.setUint32(2, offset, true);
      ad.setUint16(6, requested, true); ad.setUint32(8, item.revision, true);
      const p = await this.call(OP.CAROUSEL_READ, args);
      if (p.length < 14) throw new Error('empty/truncated CAROUSEL_READ response');
      const dv = new DataView(p.buffer, p.byteOffset, p.byteLength), block = p.subarray(13);
      if (p[0] !== slot || p[1] !== kind || dv.getUint32(2, true) !== offset || dv.getUint32(6, true) !== total ||
          dv.getUint16(10, true) !== expectedCrc || p[12] !== format || block.length > requested || offset + block.length > total) {
        throw new Error('CAROUSEL_READ identity/offset/metadata mismatch');
      }
      data.set(block, offset); offset += block.length; onProgress?.(offset / total);
    }
    if (crc16(data) !== expectedCrc) throw new MoinkError(ST.CRC, 'CAROUSEL_READ');
    return data;
  }

  /* ------------------------------ 缓存 / 清理 ------------------------------ */

  async cacheShow(): Promise<void> {
    await this.call(OP.CACHE_SHOW, undefined, 180000);
  }

  async cacheInfo(): Promise<{ valid: boolean; w: number; h: number; crc16: number; len: number }> {
    const p = await this.call(OP.CACHE_INFO);
    const dv = new DataView(p.buffer, p.byteOffset, p.byteLength);
    return {
      valid: p[0] === 1,
      w: dv.getUint16(1, true),
      h: dv.getUint16(3, true),
      crc16: dv.getUint16(5, true),
      len: dv.getUint32(7, true),
    };
  }

  async ghostClear(cycles = 2): Promise<void> {
    await this.call(OP.GHOST_CLEAR, new Uint8Array([cycles]), 180000);
  }

  /* ------------------------------ OTA ------------------------------ */

  /** 推送固件镜像。成功后设备约 1.5s 自动重启并在新固件跑满 45s 后才确认。 */
  async ota(
    image: Uint8Array,
    opts: { onProgress?: (p: number) => void; timeoutMs?: number } = {},
  ): Promise<void> {
    let chunk = this.chunkSize;
    const begin = new Uint8Array(10);
    const dv = new DataView(begin.buffer);
    dv.setUint32(0, image.length, true);
    dv.setUint32(4, crc32(image), true);
    dv.setUint16(8, chunk, true);

    const pl = await this.call(OP.OTA_BEGIN, begin);
    chunk = pl[0] | (pl[1] << 8);

    if (chunk < 16 || chunk > this.chunkSize) throw new Error('invalid negotiated OTA chunk size');
    const total = Math.ceil(image.length / chunk);
    this.gapAt = null;
    let idx = 0, retries = 0;
    try {
      for (;;) {
        while (idx < total) {
          if (this.gapAt !== null) {
            const gap = this.gapAt; this.gapAt = null;
            if (gap < idx) { idx = gap; retries++; }
          }
          if (retries > 8) throw new MoinkError(ST.SEQ_GAP, 'OTA');
          await this.writeData(idx, image.subarray(idx * chunk, (idx + 1) * chunk));
          idx++;
          if ((idx & 15) === 0) opts.onProgress?.(idx / total);
          if ((idx & 31) === 0) await new Promise((r) => setTimeout(r, 5));
        }
        const [status, payload] = await this.cmd(OP.OTA_END, new Uint8Array(), opts.timeoutMs ?? 180000);
        if (status === ST.OK) break;
        if (status === ST.INCOMPLETE && payload.length >= 2) {
          idx = payload[0] | (payload[1] << 8);
          if (++retries > 8 || idx >= total) throw new MoinkError(ST.SEQ_GAP, 'OTA');
          continue;
        }
        throw new MoinkError(status, 'OTA_END', payload);
      }
    } catch (e) {
      try { await this.cmd(OP.OTA_ABORT, new Uint8Array(), 5000); } catch { /* keep original error */ }
      throw e;
    }
    opts.onProgress?.(1);
  }

  /* ------------------------------ 维护 ------------------------------ */

  async abortFrame(): Promise<void> {
    await this.call(OP.FRAME_ABORT);
  }

  async reboot(delayS = 1): Promise<void> {
    await this.call(OP.REBOOT, new Uint8Array([delayS]));
  }

  async factoryReset(): Promise<void> {
    await this.call(OP.FACTORY_RESET);
  }

  async sleepNow(): Promise<void> {
    await this.call(OP.SLEEP_NOW);
  }
}

export default MoinkBle;