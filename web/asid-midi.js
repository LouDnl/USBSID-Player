/*
 * USBSID-Player web backend - ASID (Web MIDI) transport.
 *
 * web/asid-midi.js
 *
 * A transport for USBSIDPlayerWeb that drives a USBSID-Pico (or any ASID host)
 * over Web MIDI using the ASID SysEx protocol
 * (https://github.com/thomasj/asid-protocol).
 *
 * Unlike the WebUSB cycled path, ASID is NOT cycle-exact per write: it sends a
 * once-per-frame SNAPSHOT of the SID registers that changed during that frame.
 * This maps onto the transport interface as:
 *   writeCycled(reg,val,cycles) -> accumulate reg change (cycles ignored*)
 *   flush()                     -> emit one 0x4E SysEx for the frame
 * (*the ASID 0x30 write-order/timing extension is not implemented here; standard
 *  per-frame playback derives timing from the ~50/60 Hz flush cadence.)
 *
 * Register accumulation mirrors repo/src/... and player-repo/lib/midi/asid.cpp,
 * including the gate-register shadowing (0x04/0x0b/0x12 -> 0x19/0x1a/0x1b) that
 * preserves note retriggers happening twice within one frame.
 *
 * SysEx frames: F0 2D <cmd> <payload> F7   (manufacturer 0x2D)
 *   0x4C start   0x4D stop   0x4E SID1 data (0x50/0x51/0x52 = SID2/3/4)
 *
 * File author: LouD
 * Copyright (c) 2026 LouD - GPLv2 (see repo LICENSE).
 */

/* ASID register order: index = ASID bit position, value = SID register addr. */
const REGMAP = [0,1,2,3,5,6,7,8,9,10,12,13,14,15,16,17,19,20,21,22,23,24,4,11,18,25,26,27];
const SID_CMD = [0x4E, 0x50, 0x51, 0x52];   /* per-SID data command */
const ASID_MFR = 0x2D;
const ASID_START = 0x4C;
const ASID_STOP  = 0x4D;

function makeChip() {
  return { reg: new Uint8Array(32), modified: new Uint8Array(32) };
}

export class ASIDMIDITransport {
  /**
   * @param {object} opts { nosids?:number, deviceNameHint?:string }
   */
  constructor(opts = {}) {
    this.nosids = opts.nosids || 1;
    this._hint = (opts.deviceNameHint || 'USBSID').toLowerCase();
    this._access = null;
    this._out = null;
    this._open = false;
    this._chips = [makeChip(), makeChip(), makeChip(), makeChip()];
    /* Optional tap: called with (reg, val) for every write, so a host can
     * mirror the SID register state in a UI. Set by the adapter. */
    this.onWrite = null;
  }

  get isOpen() { return this._open; }
  get productName() { return this._out ? (this._out.name || 'MIDI') : ''; }

  /** List available MIDI outputs [{id,name}]. Requires connect() first. */
  outputs() {
    if (!this._access) return [];
    return [...this._access.outputs.values()].map((o) => ({ id: o.id, name: o.name }));
  }

  /**
   * Request Web MIDI (with SysEx) and pick an output. If outputId is omitted,
   * pick the first whose name matches the hint, else the first available.
   */
  async connect(outputId = null) {
    this._access = await navigator.requestMIDIAccess({ sysex: true });
    const outs = [...this._access.outputs.values()];
    if (outs.length === 0) throw new Error('no MIDI outputs');
    const byHint = outs.find((o) => (o.name || '').toLowerCase().includes(this._hint));
    // Prefer the requested id, then a name-hint match, then the first output.
    // Never throw just because a stale/foreign id was passed.
    this._out = (outputId && outs.find((o) => o.id === outputId)) || byHint || outs[0];
    this._open = true;
    return true;
  }

  selectOutput(outputId) {
    if (!this._access) return false;
    const o = [...this._access.outputs.values()].find((x) => x.id === outputId);
    if (o) { this._out = o; return true; }
    return false;
  }

  /** Pick an output by its display name. Used when a host wants to follow a
   * shared UI selection without exposing our internal port ids. */
  selectOutputByName(name) {
    if (!this._access || !name) return false;
    const o = [...this._access.outputs.values()].find((x) => (x.name || '') === name);
    if (o) { this._out = o; return true; }
    return false;
  }

  async disconnect() {
    this.playbackStop();
    this._open = false;
    this._out = null;
    this._access = null;
  }

  /* ---- Transport interface -------------------------------------------- */

  /** Enter ASID play mode (called by the player when playback starts). */
  playbackStart() { this._basic(ASID_START); }

  /** Leave ASID play mode / silence. */
  playbackStop() { this._basic(ASID_STOP); this._clearAll(); }

  /** Accumulate a register change into the current frame snapshot. */
  writeCycled(reg, val, _cycles) {
    if (!this._open) return;
    if (this.onWrite) this.onWrite(reg, val);
    const sid = (reg >> 5) & 0x03;          // 0x00-0x1f=SID0, 0x20-0x3f=SID1...
    const r = reg & 0x1f;
    const data = val & 0xFF;
    const c = this._chips[sid];
    if (c.modified[r] === 0) {
      c.reg[r] = data;
      c.modified[r] = 1;
      return;
    }
    // Register already written this frame: shadow the gate regs so a retrigger
    // within one frame is not lost; flush filter/vol double-writes immediately.
    switch (r) {
      case 0x04:
        if (c.modified[0x19] !== 0) c.reg[0x04] = c.reg[0x19];
        c.reg[0x19] = data; c.modified[0x19] = 1; break;
      case 0x0b:
        if (c.modified[0x1a] !== 0) c.reg[0x0b] = c.reg[0x1a];
        c.reg[0x1a] = data; c.modified[0x1a] = 1; break;
      case 0x12:
        if (c.modified[0x1b] !== 0) c.reg[0x12] = c.reg[0x1b];
        c.reg[0x1b] = data; c.modified[0x1b] = 1; break;
      case 0x16: case 0x17: case 0x18:
        this.flush();
        c.reg[r] = data; c.modified[r] = 1; break;
      default:
        c.reg[r] = data;   // latest value wins for the rest
    }
  }

  /** Emit one 0x4E snapshot SysEx per SID, then clear the frame. */
  flush() {
    if (!this._open) return;
    for (let sid = 0; sid < this.nosids; sid++) {
      const c = this._chips[sid];
      let mask = 0, msb = 0;
      for (let i = 0; i < 28; i++) {
        const j = REGMAP[i];
        if (c.modified[j] !== 0) mask |= (1 << i);
        if (c.reg[j] > 0x7f)     msb  |= (1 << i);
      }
      if (mask === 0) continue;   // nothing changed for this SID this frame
      const msg = [0xF0, ASID_MFR, SID_CMD[sid],
        mask & 0x7f, (mask >> 7) & 0x7f, (mask >> 14) & 0x7f, (mask >> 21) & 0x7f,
        msb & 0x7f, (msb >> 7) & 0x7f, (msb >> 14) & 0x7f, (msb >> 21) & 0x7f];
      for (let i = 0; i < 28; i++) {
        const j = REGMAP[i];
        if (c.modified[j] !== 0) msg.push(c.reg[j] & 0x7f);
      }
      msg.push(0xF7);
      this._send(msg);
      c.modified.fill(0);
    }
  }

  /** Silence on stop (player calls resetSID): leave ASID mode. */
  resetSID() { this.playbackStop(); }
  reset()    { this._clearAll(); }

  /* ---- internals ------------------------------------------------------ */

  _clearAll() { for (const c of this._chips) c.modified.fill(0); }

  _basic(cmd) {
    if (!this._out) return;
    this._send([0xF0, ASID_MFR, cmd, 0xF7]);
  }

  _send(bytes) {
    try { this._out.send(bytes); } catch (_) {}
  }
}
