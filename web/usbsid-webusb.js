/*
 * USBSID-Player web backend - WebUSB transport.
 *
 * web/usbsid-webusb.js
 *
 * A transport for USBSIDPlayerWeb that talks to a physical USBSID-Pico over
 * WebUSB. The connect/enable sequence and the CYCLED_WRITE frame format mirror
 * the proven driver in repo/examples/config-tool-web/usbsid-driver.js.
 *
 * Packet format (see repo/src/usbsid.c handle_buffer_task + globals.h):
 *   byte 0 : command. Top 2 bits = type (CYCLED_WRITE = 0b10 = 0x80).
 *            Lower 6 bits = payload BYTE count = (numWrites * 4).
 *   byte 1.. : numWrites x 4-byte writes [ reg, val, cycles_hi, cycles_lo ].
 *   Max 15 writes per packet: 1 + 15*4 = 61 bytes (<= 64).
 *
 * Firmware special case: a command byte with lower 6 bits == 0 (i.e. plain
 * 0x80) means a SINGLE 4-byte cycled write in bytes 1..4 - and the firmware
 * processes only that one write, ignoring the rest of the packet. So a real
 * batch MUST set the byte count (numWrites*4); { batch: false } uses the
 * single-write form (one transfer per write).
 *
 * Writes go through a bounded queue with a small in-flight window (see the
 * constructor) so the device is pipelined but backpressured, never overrun.
 *
 * File author: LouD
 * Copyright (c) 2026 LouD - GPLv2 (see repo LICENSE).
 */

const USBSID_VID   = 0xcafe;
const USBSID_PID   = 0x4011;
const DEVICE_CLASS = 0xFF;   /* vendor interface carrying the SID bus */
const CTRL_TRANSFER = 0x22;  /* WebUSB enable request */
const CTRL_ENABLE   = 0x01;

const CYCLED_WRITE = 2;                    /* top-2-bits type */
const CYCLED_CMD   = (CYCLED_WRITE << 6);  /* 0x80 */
const COMMAND      = 3;                     /* top-2-bits type */
const CMD          = (COMMAND << 6);        /* 0xC0 */
/* COMMAND sub-commands (globals.h) */
const MUTE      = 12;
const UNMUTE    = 13;
const RESET_SID = 14;
const CONFIG    = 18;                 /* COMMAND sub-type carrying config cmds */
const CFG_CMD   = (CMD | CONFIG);     /* 0xD2 */
const SET_CLOCK = 0x50;               /* config sub-command: set SID clock */
/* SET_CLOCK rate ids */
export const CLOCK = { DEFAULT: 0, PAL: 1, NTSC: 2, DREAN: 3 };
const WRITE_BYTES  = 4;                    /* payload bytes per cycled write */
const MAX_PACKET   = 64;
const MAX_FRAMES   = 15;                    /* 1 hdr + 15*4 = 61 bytes <= 64 */
/* Bound on queued-but-unsent packets (~1 KB). At steady state the queue holds
 * 1-2 packets; a sustained overflow means we are behind the device, so the
 * oldest packet is dropped to cap latency rather than lag by seconds. */
const MAX_QUEUE    = 256;

const delay = (ms) => new Promise((r) => setTimeout(r, ms));

export class USBSIDWebUSBTransport {
  /**
   * @param {object} opts
   *   opts.batch  - false to send one write per transfer (default: batched)
   *   opts.device - an already-open USBSID device to reuse instead of opening
   *                 our own. Must expose writeArray(Uint8Array) (or write()).
   *                 This is how the player plugs into the config-tool-web app,
   *                 which already owns a connected USBSIDDevice.
   */
  constructor(opts = {}) {
    this.batch = opts.batch !== false;
    this._extDev = opts.device || null;   // injected USBSIDDevice, or null
    this._dev = null;
    this._ifaceNum = null;
    this._epOut = null;
    this._epIn = null;
    this._open = false;
    /* Packing buffer for batched frames. */
    this._pkt = new Uint8Array(MAX_PACKET);
    this._pktFrames = 0;
    /* Optional tap: called with (reg, val) for every write, so a host can
     * mirror the SID register state in a UI. Set by the adapter. */
    this.onWrite = null;
    /* Send queue with a single transfer in flight: never send the next packet
     * until the previous transferOut resolves (USB backpressure, mirrors the
     * libusb driver's transfer_out_pending guard). */
    this._q = [];
    this._inflight = false;
    this._maxQ = MAX_QUEUE;
  }

  get isOpen() {
    if (this._extDev) return this._extDev.isOpen !== false;
    return this._open;
  }
  get productName() {
    if (this._extDev) return this._extDev.productName || 'USBSID-Pico';
    return this._dev ? (this._dev.productName || '') : '';
  }

  /**
   * Prompt for and open a USBSID-Pico. Must be called from a user gesture.
   * If an external device was injected (opts.device), this is a no-op that just
   * confirms the shared device is available.
   */
  async connect() {
    if (this._extDev) { this._open = true; return this.isOpen; }
    if (this._open) return true;
    this._dev = await navigator.usb.requestDevice({
      filters: [{ vendorId: USBSID_VID, productId: USBSID_PID }],
    });
    return this._openDevice();
  }

  async _openDevice() {
    await this._dev.open();
    if (this._dev.configuration === null) await this._dev.selectConfiguration(1);

    /* Find the vendor (DEVICE_CLASS) interface + its bulk endpoints. */
    for (const iface of this._dev.configuration.interfaces) {
      for (const alt of iface.alternates) {
        if (alt.interfaceClass === DEVICE_CLASS) {
          this._ifaceNum = iface.interfaceNumber;
          for (const ep of alt.endpoints) {
            if (ep.direction === 'out') this._epOut = ep.endpointNumber;
            if (ep.direction === 'in')  this._epIn  = ep.endpointNumber;
          }
        }
      }
    }
    if (this._ifaceNum === null) { await this._dev.close(); return false; }

    await this._dev.claimInterface(this._ifaceNum);
    await this._dev.selectAlternateInterface(this._ifaceNum, 0);
    try { await this._dev.clearHalt('out', this._epOut); } catch (_) {}
    try { await this._dev.clearHalt('in',  this._epIn);  } catch (_) {}
    await this._dev.controlTransferOut({
      requestType: 'class',
      recipient:   'interface',
      request:     CTRL_TRANSFER,
      value:       CTRL_ENABLE,
      index:       this._ifaceNum,
    });
    /* Settle: SET_INTERFACE resets bulk endpoints; first OUT can be dropped. */
    await delay(100);
    this._open = true;
    return true;
  }

  async disconnect() {
    this._open = false;
    this._q.length = 0; this._inflight = false;
    if (this._extDev) { this._pktFrames = 0; return; } // app owns the device
    if (this._dev) {
      try {
        if (this._ifaceNum !== null) await this._dev.releaseInterface(this._ifaceNum);
        await this._dev.close();
      } catch (_) {}
    }
    this._dev = null; this._epOut = null; this._epIn = null; this._ifaceNum = null;
    this._pktFrames = 0;
  }

  /* ---- Transport interface used by USBSIDPlayerWeb ---------------------- */

  writeCycled(reg, val, cycles) {
    if (!this._open) return;
    if (this.onWrite) this.onWrite(reg, val);
    const chi = (cycles >> 8) & 0xFF;
    const clo = cycles & 0xFF;
    if (!this.batch) {
      /* Single-write form: cmd 0x80 (byte count 0), then one 4-byte write. */
      this._send(new Uint8Array([CYCLED_CMD, reg & 0xFF, val & 0xFF, chi, clo]));
      return;
    }
    const o = 1 + this._pktFrames * WRITE_BYTES; /* byte 0 is the header */
    this._pkt[o]     = reg & 0xFF;
    this._pkt[o + 1] = val & 0xFF;
    this._pkt[o + 2] = chi;
    this._pkt[o + 3] = clo;
    if (++this._pktFrames >= MAX_FRAMES) this._flushPacket();
  }

  /** VSYNC boundary: push any partially-filled packet. */
  flush() {
    if (this.batch) this._flushPacket();
  }

  reset() {
    this._pktFrames = 0;
  }

  /* ---- Device commands (COMMAND type, 6-byte packet) ------------------- */

  _command(sub, b1 = 0) {
    if (!this._open) return;
    this._send(new Uint8Array([CMD | (sub & 0x3F), b1, 0, 0, 0, 0]));
  }

  /* Silence the device. Drop any pending queued writes first (no trailing burst),
   * then zero all SID registers. b1=1 -> reset_sid_registers() writes 0 to every
   * register incl. volume ($d418) = immediate silence; b1=0 -> reset_sid() only
   * pulses the RES line, which does NOT reliably kill a sustained note. */
  resetSID() {
    this._pktFrames = 0;
    this._q.length = 0;   /* drop the backlog so no trailing burst plays and the
                             reset is not stuck behind queued writes */
    this._command(RESET_SID, 1);
  }

  mute()   { if (this.batch) this._flushPacket(); this._command(MUTE); }
  unmute() { this._command(UNMUTE); }

  /* Set the device SID clock. rateId: 0=default, 1=PAL, 2=NTSC, 3=DREAN
   * (use the exported CLOCK map). Packet: [0xD2, SET_CLOCK, rateId, 0,0,0]. */
  setClock(rateId) {
    if (!this._open) return;
    this._send(new Uint8Array([CFG_CMD, SET_CLOCK, rateId & 0xFF, 0, 0, 0]));
  }

  _flushPacket() {
    if (this._pktFrames === 0) return;
    const nbytes = this._pktFrames * WRITE_BYTES;   /* payload byte count */
    this._pkt[0] = CYCLED_CMD | (nbytes & 0x3F);    /* type | byte count */
    this._send(this._pkt.slice(0, 1 + nbytes));
    this._pktFrames = 0;
  }

  /* Enqueue a packet and keep a single transfer in flight (serialized send). */
  _send(buf) {
    this._q.push(buf);
    if (this._q.length > this._maxQ) this._q.shift();   /* bounded latency */
    this._pump();
  }

  _pump() {
    if (this._inflight) return;
    const buf = this._q.shift();
    if (!buf) return;
    this._inflight = true;
    const done = () => { this._inflight = false; this._pump(); };
    let p;
    try {
      p = this._transfer(buf);
    } catch (_) { done(); return; }
    if (p && p.then) p.then(done, done);
    else done();
  }

  /* Issue one transfer, returning a promise that resolves when the device has
   * accepted the packet (real backpressure). In injected mode prefer the app
   * device's awaiting writeArrayAwait; fall back to fire-and-forget writeArray
   * (which resolves early) if an older app build lacks it. */
  _transfer(buf) {
    if (this._extDev) {
      const fn = this._extDev.writeArrayAwait
        || this._extDev.writeArray || this._extDev.write;
      return fn.call(this._extDev, buf);
    }
    return this._dev.transferOut(this._epOut, buf);
  }
}
