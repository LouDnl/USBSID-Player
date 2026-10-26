/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * usplayer-worker-client.js
 * Page side of the worker player
 *
 * Same surface as USBSIDPlayerWeb, a page can swap one for the other
 * Owns the worker and the AudioContext, the audio thread is wired to the worker
 * The main thread is not in the playback path
 *
 * requestDevice() runs in the page, it needs a user gesture and a document
 * The worker finds the granted board with getDevices(), see USBSIDWebUSBTransport.connectGranted()
 *
 * USBSIDPlayerWorker.available() is false without Worker, WebUSB or Web Audio, fall back to USBSIDPlayerWeb
 * ASID always falls back, Web MIDI has no worker API
 *
 * This file is part of USBSID-Player (https://github.com/LouDnl/USBSID-Player)
 * File author: LouD
 *
 * Copyright (c) 2026 LouD
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
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
   *                   (default: beside the worker, where the WEB build puts it)
   *   opts.workerUrl  URL of usplayer-worker.js (default: beside this file)
   */
  constructor(opts = {}) {
    this._wasmUrl = opts.wasmUrl || './usbsid.mjs';
    this._workerUrl = opts.workerUrl ||
      new URL('./usplayer-worker.js', import.meta.url);
    this._worker = null;
    this._audio = null;
    this._board = null;
    /* Transport for the worker to build, undefined = worker chooses (bench sets it) */
    this._prefer = opts.prefer;
    /* Log callback for worker messages */
    this._onLog = opts.onLog || null;
    /* Set from the worker answer after connect() */
    this.transportKind = null;
    /* Time of the last tick driven state message, 0 = never */
    this._stateAt = 0;
    /* True when the audio clock never ticked and the worker timer took over */
    this.clockFallback = false;
    this._seq = 1;
    this._pending = new Map();
    this._state = {
      playing: false, prg: false, frames: 0, sidWrites: 0, dropped: 0,
      queueDepth: 0, refreshHz: 50.125, name: '', author: '', released: '',
      song: 1, songs: 1, stats: null,
    };
    /* Called with the state snapshot on each worker report (about 10 per second) */
    this.onState = null;
  }

  /* Worker path can run in this environment */
  static available() {
    return typeof Worker !== 'undefined' &&
           typeof navigator !== 'undefined' && !!navigator.usb &&
           (typeof AudioContext !== 'undefined' ||
            typeof globalThis.webkitAudioContext !== 'undefined');
  }

  /*
   * One round trip to the worker, with a timeout
   * A worker that fails to load its imports never replies, resume() on a blocked AudioContext stays pending
   * init gets 30 s (compiles the wasm and opens the board), the rest 15 s
   */
  _call(type, payload, transfer) {
    return new Promise((resolve, reject) => {
      const id = this._seq++;
      const ms = (type === 'init') ? 30000 : 15000;
      const timer = setTimeout(() => {
        if (!this._pending.delete(id)) return;
        reject(new Error(`the worker did not answer '${type}' within ${ms / 1000}s`));
      }, ms);
      this._pending.set(id, { resolve, reject, timer });
      this._worker.postMessage({ type, payload, id }, transfer || []);
    });
  }

  /* Fail all outstanding calls */
  _failPending(message) {
    for (const [, waiting] of this._pending) {
      if (waiting.timer) clearTimeout(waiting.timer);
      waiting.reject(new Error(message));
    }
    this._pending.clear();
  }

  _log(message) {
    if (this._onLog) this._onLog('[worker] ' + message);
    else console.log('[usplayer worker]', message);
  }

  _onMessage(e) {
    const { type, payload, id } = e.data || {};
    if (type === 'log') { this._log((payload && payload.message) || ''); return; }
    if (type === 'state') {
      /* Posted by onTick only, the timestamp shows the clock runs */
      this._stateAt = Date.now();
      this._state = payload || this._state;
      if (this.onState) this.onState(this._state);
      return;
    }
    const waiting = this._pending.get(id);
    if (!waiting) return;
    this._pending.delete(id);
    if (waiting.timer) clearTimeout(waiting.timer);
    if (type === 'error') reject_(waiting, payload);
    else waiting.resolve(payload);
  }

  /*
   * Ask for the board, then start the worker
   * Needs a user gesture for the picker
   */
  async connect() {
    /* Log each step, any of them can hang */
    this._log('looking for a granted board');
    const devices = await navigator.usb.getDevices();
    const known = devices.some((d) => d.vendorId === USBSID_VID &&
                                      d.productId === USBSID_PID);
    this._log(known ? 'board already granted, no picker needed'
                    : 'not granted yet, asking');
    if (!known) {
      /* Only the picker needs the gesture, the worker opens the board afterwards */
      await navigator.usb.requestDevice({
        filters: [{ vendorId: USBSID_VID, productId: USBSID_PID }],
      });
    }

    this._log('starting the worker at ' + this._workerUrl);
    this._worker = new Worker(this._workerUrl, { type: 'module' });
    this._worker.onmessage = (e) => this._onMessage(e);
    /* Fires when a module worker cannot load its imports, every call hangs without it */
    this._worker.onerror = (e) => {
      const where = e && e.filename ? ` (${e.filename}:${e.lineno})` : '';
      const msg = 'the worker failed to start: ' +
                  ((e && e.message) || 'unknown error') + where;
      this._log(msg);
      this._failPending(msg);
    };
    this._worker.onmessageerror = () => {
      this._failPending('the worker could not read a message from the page');
    };

    this._log('worker started, asking it to load ' + this._wasmUrl +
              ' and open the board');
    const { opened, board, transport } =
      await this._call('init', { wasmUrl: this._wasmUrl, prefer: this._prefer });
    this._log('init answered: opened=' + opened + ', transport=' + transport);
    /* Transport the worker built */
    this.transportKind = transport || null;
    if (!opened) throw new Error('the worker could not open the board');
    this._board = board || null;
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

  /*
   * Start the clock and give the worker its end
   * AudioWorkletProcessor and worker each get one port of a MessageChannel
   * Ticks bypass this thread, a background tab does not throttle playback
   */
  async start() {
    await this._startAudioClock();
    await this._call('start');
    await this._ensureClockRunning();
  }

  /*
   * Check the emulation is driven by ticks
   * A suspended AudioContext or a refused MessagePort transfer gives no ticks and no sound
   * State messages come from onTick only, if none arrived start the worker timer
   */
  async _ensureClockRunning() {
    const before = this._stateAt;
    await new Promise((r) => setTimeout(r, 800));
    if (this._stateAt !== before) return true;
    const state = this._audio ? this._audio.ctx.state : 'no context';
    this._log(`no ticks from the audio clock after 800 ms. AudioContext is ` +
              `${state}. Falling back to a timer in the worker.`);
    this.clockFallback = true;
    await this._call('fallbackClock', { on: true });
    /* Same check for the fallback */
    const beforeFallback = this._stateAt;
    await new Promise((r) => setTimeout(r, 800));
    if (this._stateAt === beforeFallback) {
      this._log('the fallback timer produced no ticks either, the clock is ' +
                'not what is wrong: the worker is not stepping the emulation.');
    }
    return false;
  }

  async _startAudioClock() {
    if (this._audio) return;
    const AC = globalThis.AudioContext || globalThis.webkitAudioContext;
    const ctx = new AC();
    /* No audio, only runs on the audio thread */
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
    const r = await this._call('clock', { port: chan.port1 }, [chan.port1]);
    if (!r || !r.ok) {
      this._log('the worker did not get a usable clock port back (' +
                ((r && r.kind) || 'no answer') + '), the audio thread cannot ' +
                'drive it. The fallback timer will be used.');
    }
    node.port.postMessage({ port: chan.port2 }, [chan.port2]);
    node.connect(ctx.destination);   // needs a consumer to run
    /* resume() stays pending until the user interacts with the page, wait 2 s at most */
    await Promise.race([
      ctx.resume(),
      new Promise((r) => setTimeout(r, 2000)),
    ]);
    if (ctx.state !== 'running') {
      console.warn('[usplayer] the AudioContext is', ctx.state +
        '. The browser needs a click on the page before audio may start.');
    }
    this._audio = { ctx, node };
  }

  async stop() {
    if (this.clockFallback) {
      try { await this._call('fallbackClock', { on: false }); } catch (_) {}
      this.clockFallback = false;
    }
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
  setSidConfig(numsids, one, two, fmopl) {
    return this._call('setSidConfig', { numsids, one, two, fmopl });
  }
  async applyBoardConfig() {
    const r = await this._call('applyBoardConfig');
    if (r && r.board) this._board = r.board;
    return this._board || null;
  }
  boardConfig() { return this._board || null; }
  stats() { return this._state.stats || null; }

  /* Read from the last snapshot, synchronous like the main thread player */
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
