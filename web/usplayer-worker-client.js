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
   *   opts.wasmUrl    URL of usbsid.mjs, resolved from the worker. The
   *                   default assumes it sits beside the worker, which is
   *                   where the WEB build puts it.
   *   opts.workerUrl  URL of usplayer-worker.js (default: next to this file)
   */
  constructor(opts = {}) {
    this._wasmUrl = opts.wasmUrl || './usbsid.mjs';
    this._workerUrl = opts.workerUrl ||
      new URL('./usplayer-worker.js', import.meta.url);
    this._worker = null;
    this._audio = null;
    this._board = null;
    /* Which transport the worker should build. Undefined means let it choose,
     * which is what everything but the bench wants. Passing it matters because
     * a main-thread leg and a worker leg using different transports is not a
     * comparison of threads, it is a comparison of two unrelated things. */
    this._prefer = opts.prefer;
    /* Where the worker's own notes go. The page has a log the user can read and
     * paste; the console is where a worker's troubles used to stay. */
    this._onLog = opts.onLog || null;
    /** Set from the worker's own answer once connect() has run. */
    this.transportKind = null;
    /** When a tick-driven state message last arrived. 0 means never. */
    this._stateAt = 0;
    /** True when the audio clock never ticked and the worker timer took over. */
    this.clockFallback = false;
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

  /**
   * One round trip to the worker.
   *
   * With a timeout, because the failure modes that do not produce a reply are
   * the ones that used to look like nothing happening at all: a module worker
   * whose import graph fails to load never installs its message handler, and an
   * AudioContext that is not allowed to start leaves `resume()` pending
   * indefinitely rather than rejecting. Either one left this promise unsettled
   * and the page silent, with nothing to go on.
   *
   * `init` gets longer than the rest: it compiles the wasm and opens the board.
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

  /** Fail everything outstanding, for when the worker itself has gone wrong. */
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
      /* Only onTick posts these, so the timestamp is proof the clock runs. */
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

  /**
   * Ask for the board, then bring the worker up.
   *
   * Must be called from a user gesture: the picker will not open otherwise.
   */
  async connect() {
    /* Breadcrumbs, because every step here can hang and they all used to look
     * the same from outside: nothing. */
    this._log('looking for a granted board');
    const devices = await navigator.usb.getDevices();
    const known = devices.some((d) => d.vendorId === USBSID_VID &&
                                      d.productId === USBSID_PID);
    this._log(known ? 'board already granted, no picker needed'
                    : 'not granted yet, asking');
    if (!known) {
      /* Only the picker needs the gesture; the worker opens it afterwards. */
      await navigator.usb.requestDevice({
        filters: [{ vendorId: USBSID_VID, productId: USBSID_PID }],
      });
    }

    this._log('starting the worker at ' + this._workerUrl);
    this._worker = new Worker(this._workerUrl, { type: 'module' });
    this._worker.onmessage = (e) => this._onMessage(e);
    /* A module worker that cannot load its imports fires this and never runs a
     * line of its own code, so without a handler every call hangs. Same for a
     * message that cannot be deserialised. */
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
    /** Which transport the worker actually built, not which one we assumed. */
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
    await this._ensureClockRunning();
  }

  /**
   * Make sure something is actually driving the emulation.
   *
   * The worker only advances on a tick, and every way of not getting one looks
   * identical from here: the tune loads, the board is open, the calls all
   * succeed, and nothing plays. A suspended AudioContext does it, and so does a
   * browser that will not transfer a MessagePort into an AudioWorkletProcessor.
   *
   * State messages come from onTick and nowhere else, so their arrival is the
   * only honest test. If none has, start the worker's own timer, which is worse
   * in a backgrounded tab but is playing rather than silence, and say so.
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
    /* Give the fallback the same chance to prove itself. */
    const beforeFallback = this._stateAt;
    await new Promise((r) => setTimeout(r, 800));
    if (this._stateAt === beforeFallback) {
      this._log('the fallback timer produced no ticks either, so the clock is ' +
                'not what is wrong: the worker is not stepping the emulation.');
    }
    return false;
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
    const r = await this._call('clock', { port: chan.port1 }, [chan.port1]);
    if (!r || !r.ok) {
      this._log('the worker did not get a usable clock port back (' +
                ((r && r.kind) || 'no answer') + '), so the audio thread cannot ' +
                'drive it. The fallback timer will be used.');
    }
    node.port.postMessage({ port: chan.port2 }, [chan.port2]);
    node.connect(ctx.destination);   // something has to pull it or it never runs
    /* resume() on a context the browser will not let start does not reject: it
     * stays pending until the user interacts with the page. Waiting on it for
     * ever is how a second leg of a benchmark silently never began. Give it a
     * moment, then carry on and say so: the clock will start by itself as soon
     * as the context is allowed to run. */
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
