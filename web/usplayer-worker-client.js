/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback, for embedding on RP2350 (Pico2), and in a browser.
 *
 * web/usplayer-worker-client.js
 * The page's side of the worker player.
 *
 * Presents the same surface `USBSIDPlayerWeb` does, so a page can swap one for
 * the other and change nothing else. What it actually does is own the worker,
 * own the AudioContext, and wire the audio thread straight to the worker so the
 * main thread is not in the playback path.
 *
 * The one thing that has to happen here is `requestDevice()`: it shows a picker,
 * so it needs a user gesture and a document. Once granted, the worker finds the
 * same board with `getDevices()`. See USBSIDWebUSBTransport.connectGranted().
 *
 * Use `USBSIDPlayerWorker.available()` to decide: it is false without Worker,
 * WebUSB or Web Audio, and the page should fall back to USBSIDPlayerWeb. ASID
 * always falls back, because Web MIDI has no worker API.
 *
 * This file is part of USBSID-Pico (https://github.com/LouDnl/USBSID-Player)
 * File author: LouD
 *
 * Copyright (c) 2026 LouD
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

const USBSID_VID = 0xcafe;
const USBSID_PID = 0x4011;

export class USBSIDPlayerWorker {
  /**
   * @param {object} opts
   *   opts.wasmUrl    URL of usbsid.mjs, resolved from the worker
   *   opts.workerUrl  URL of usplayer-worker.js (default: next to this file)
   */
  constructor(opts = {}) {
    this._wasmUrl = opts.wasmUrl || '../temp/build_web/usbsid.mjs';
    this._workerUrl = opts.workerUrl ||
      new URL('./usplayer-worker.js', import.meta.url);
    this._worker = null;
    this._audio = null;
    this._seq = 1;
    this._pending = new Map();
    this._state = {
      playing: false, prg: false, frames: 0, sidWrites: 0, dropped: 0,
      queueDepth: 0, refreshHz: 50.125, name: '', author: '', released: '',
      song: 1, songs: 1, stats: null,
    };
    /* Called with the state snapshot whenever the worker reports, about ten
     * times a second. A page that wants a counter reads it from here rather
     * than polling across the thread boundary. */
    this.onState = null;
  }

  /** Whether the worker path can run at all here. */
  static available() {
    return typeof Worker !== 'undefined' &&
           typeof navigator !== 'undefined' && !!navigator.usb &&
           (typeof AudioContext !== 'undefined' ||
            typeof globalThis.webkitAudioContext !== 'undefined');
  }

  _call(type, payload, transfer) {
    return new Promise((resolve, reject) => {
      const id = this._seq++;
      this._pending.set(id, { resolve, reject });
      this._worker.postMessage({ type, payload, id }, transfer || []);
    });
  }

  _onMessage(e) {
    const { type, payload, id } = e.data || {};
    if (type === 'state') {
      this._state = payload || this._state;
      if (this.onState) this.onState(this._state);
      return;
    }
    const waiting = this._pending.get(id);
    if (!waiting) return;
    this._pending.delete(id);
    if (type === 'error') reject_(waiting, payload);
    else waiting.resolve(payload);
  }

  /**
   * Ask for the board, then bring the worker up.
   *
   * Must be called from a user gesture: the picker will not open otherwise.
   */
  async connect() {
    const devices = await navigator.usb.getDevices();
    const known = devices.some((d) => d.vendorId === USBSID_VID &&
                                      d.productId === USBSID_PID);
    if (!known) {
      /* Only the picker needs the gesture; the worker opens it afterwards. */
      await navigator.usb.requestDevice({
        filters: [{ vendorId: USBSID_VID, productId: USBSID_PID }],
      });
    }

    this._worker = new Worker(this._workerUrl, { type: 'module' });
    this._worker.onmessage = (e) => this._onMessage(e);

    const { opened } = await this._call('init', { wasmUrl: this._wasmUrl });
    if (!opened) throw new Error('the worker could not open the board');
    return true;
  }

  async loadSID(bytes, subtune = 0) {
    const copy = bytes.slice().buffer;
    const r = await this._call('loadSID', { bytes: copy, subtune }, [copy]);
    if (r && r.info) this._state = r.info;
    return !!(r && r.ok);
  }

  async loadPRG(bytes) {
    const copy = bytes.slice().buffer;
    const r = await this._call('loadPRG', { bytes: copy }, [copy]);
    if (r && r.info) this._state = r.info;
    return !!(r && r.ok);
  }

  /**
   * Start the clock, and give the worker its end of it.
   *
   * The AudioWorkletProcessor gets one port of a MessageChannel and the worker
   * gets the other, so ticks go from the audio thread to the worker without
   * this thread seeing them. That is the difference that matters when the tab
   * goes to the back: nothing in the playback path is on a throttled thread.
   */
  async start() {
    await this._startAudioClock();
    await this._call('start');
  }

  async _startAudioClock() {
    if (this._audio) return;
    const AC = globalThis.AudioContext || globalThis.webkitAudioContext;
    const ctx = new AC();
    /* Produces no audio; it exists to be called on the audio thread. */
    const src = `
      class UspClock extends AudioWorkletProcessor {
        constructor() {
          super();
          this.out = null;
          this.port.onmessage = (e) => { this.out = e.data.port; };
        }
        process() { if (this.out) this.out.postMessage(0); return true; }
      }
      registerProcessor('usp-clock', UspClock);`;
    const url = URL.createObjectURL(new Blob([src], { type: 'application/javascript' }));
    await ctx.audioWorklet.addModule(url);
    URL.revokeObjectURL(url);

    const node = new AudioWorkletNode(ctx, 'usp-clock');
    const chan = new MessageChannel();
    await this._call('clock', { port: chan.port1 }, [chan.port1]);
    node.port.postMessage({ port: chan.port2 }, [chan.port2]);
    node.connect(ctx.destination);   // something has to pull it or it never runs
    await ctx.resume();
    this._audio = { ctx, node };
  }

  async stop() {
    await this._call('stop');
    if (this._audio) {
      const { ctx, node } = this._audio;
      try { node.disconnect(); } catch (_) {}
      try { await ctx.close(); } catch (_) {}
      this._audio = null;
    }
  }

  pause(on) { return this._call('pause', { on: !!on }); }
  setSpeed(mult) { return this._call('speed', { mult }); }
  fastForward(on, mult = 4) { return this._call('fastForward', { on, mult }); }
  nextSubtune() { return this._call('nextSubtune'); }
  prevSubtune() { return this._call('prevSubtune'); }
  runStop() { return this._call('runStop'); }
  forceSocketTwo() { return this._call('forceSocketTwo'); }
  setClock(rateId) { return this._call('setClock', { rateId }); }
  resetStats() { return this._call('resetStats'); }
  stats() { return this._state.stats || null; }

  /* The page reads these from the last snapshot rather than across the thread,
   * so they are synchronous like the main thread player's are. */
  isPlaying() { return this._state.playing; }
  isPrg() { return this._state.prg; }
  frames() { return this._state.frames; }
  sidWrites() { return this._state.sidWrites; }
  droppedWrites() { return this._state.dropped; }
  refreshHz() { return this._state.refreshHz; }
  info() {
    const s = this._state;
    return { name: s.name, author: s.author, released: s.released,
             song: s.song, songs: s.songs };
  }

  terminate() {
    if (this._worker) { this._worker.terminate(); this._worker = null; }
  }
}

function reject_(waiting, payload) {
  waiting.reject(new Error((payload && payload.message) || 'worker error'));
}
