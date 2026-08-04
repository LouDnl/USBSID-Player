/*
 * USBSID-Player web backend - JS glue.
 *
 * web/usplayer-web.js
 *
 * Wraps the Emscripten WASM build (build_web/usbsid.js) and drives playback:
 * per animation frame it steps the emulator one C64 video frame, drains the SID
 * write ring from the WASM heap, and forwards the (reg, val, cycles) triples to
 * a pluggable transport (WebUSB in the browser; a capture/null transport in
 * tests). A device flush is issued whenever the WASM flush counter advances
 * (once per VSYNC).
 *
 * This module is DOM-free so it can run on the main thread (default, rAF pump)
 * or be driven manually via stepAndDrain() from a test harness.
 *
 * A transport implements:
 *   writeCycled(reg, val, cycles)  - queue one cycle-exact register write
 *   flush()                        - VSYNC boundary; push queued writes
 *   reset?()                       - optional; reset the device
 *
 * File author: LouD
 * Copyright (c) 2026 LouD - GPLv2 (see repo LICENSE).
 */

/* A transport that discards everything. Useful as a default / smoke target. */
export class NullTransport {
  writeCycled(_reg, _val, _cycles) {}
  flush() {}
  reset() {}
}

/* A transport that records writes and flushes; used by the node test. */
export class CaptureTransport {
  constructor() { this.writes = []; this.flushes = 0; }
  writeCycled(reg, val, cycles) { this.writes.push([reg, val, cycles]); }
  flush() { this.flushes++; }
  reset() { this.writes.length = 0; this.flushes = 0; }
}

export class USBSIDPlayerWeb {
  /**
   * @param {object} module   instantiated Emscripten module (USBSIDPlayer())
   * @param {object} transport object implementing writeCycled/flush
   */
  constructor(module, transport = new NullTransport()) {
    this.M = module;
    this.transport = transport;
    this._running = false;
    this._rafId = 0;
    this._audio = null;     // { ctx, node } when using the AudioWorklet clock
    this._tunePtr = 0;
    this._lastFlush = 0;
    this._hz = 50.0;        // tune frame rate; refreshed on load
    this._acc = 0;          // wall-clock accumulator (ms)
    this._lastT = 0;
    this._speed = 1;        // playback speed multiplier (fast-forward)
    this._paused = false;   // when true the pump skips stepping (true pause)

    const M = module;
    // Control ABI
    this._alloc          = M.cwrap('usp_alloc', 'number', ['number']);
    this._freeBuf        = M.cwrap('usp_free', null, ['number']);
    this._loadSidtune    = M.cwrap('usp_load_sidtune', 'number', ['number', 'number', 'number']);
    this._loadPrg        = M.cwrap('usp_load_prg', null, ['number', 'number']);
    this._initSidplayer  = M.cwrap('usp_init_sidplayer', null, []);
    this._start          = M.cwrap('usp_start', null, ['number']);
    this._step           = M.cwrap('usp_step', null, []);
    this._stop           = M.cwrap('usp_stop', null, []);
    this._nextSubtune    = M.cwrap('usp_next_subtune', null, []);
    this._prevSubtune    = M.cwrap('usp_prev_subtune', null, []);
    this._pause          = M.cwrap('usp_pause', null, ['number']);
    this._isPlaying      = M.cwrap('usp_is_playing', 'number', []);
    this._forceSocketTwo = M.cwrap('usp_force_socket_two', null, []);
    this._refreshHz      = M.cwrap('usp_refresh_hz', 'number', []);
    this._isPal          = M.cwrap('usp_is_pal', 'number', []);
    // Ring ABI
    this._ringPtr     = M.cwrap('usbsid_web_ring_ptr', 'number', []);
    this._ringEntries = M.cwrap('usbsid_web_ring_entries', 'number', []);
    this._ringHead    = M.cwrap('usbsid_web_ring_head', 'number', []);
    this._ringTail    = M.cwrap('usbsid_web_ring_tail', 'number', []);
    this._ringSetTail = M.cwrap('usbsid_web_ring_set_tail', null, ['number']);
    this._flushCount  = M.cwrap('usbsid_web_flush_count', 'number', []);
    this._dropCount   = M.cwrap('usbsid_web_drop_count', 'number', []);

    // Ring geometry is fixed for the module's lifetime.
    this._ringBase = this._ringPtr();
    this._ringCap  = this._ringEntries();
  }

  /** Copy tune bytes into the WASM heap (freeing any previous tune). */
  _stage(bytes) {
    if (this._tunePtr) { this._freeBuf(this._tunePtr); this._tunePtr = 0; }
    const ptr = this._alloc(bytes.length);
    this.M.HEAPU8.set(bytes, ptr);
    this._tunePtr = ptr;
    return ptr;
  }

  /**
   * Load a PSID/RSID sidtune (Uint8Array). subtune 0 = file default.
   * Returns truthy on success.
   */
  loadSID(bytes, subtune = 0) {
    if (this._running) this.stop();   // tear down a currently-playing tune first
    const ptr = this._stage(bytes);
    const ok = this._loadSidtune(ptr, bytes.length, subtune);
    if (ok) {
      this._initSidplayer();
      this._start(0);          // set up, do not block
      this._afterStart();
    }
    return ok;
  }

  /** Load a raw PRG (Uint8Array). */
  loadPRG(bytes) {
    if (this._running) this.stop();   // tear down a currently-playing tune first
    const ptr = this._stage(bytes);
    this._loadPrg(ptr, bytes.length);
    this._start(0);
    this._afterStart();
  }

  _afterStart() {
    this._lastFlush = this._flushCount();
    const hz = this._refreshHz();
    if (hz > 1) this._hz = hz;   // PAL ~50.12 / NTSC ~59.83
    // Match the device SID clock to the tune (PAL=1 / NTSC=2) so pitch/speed
    // are correct regardless of the device's current clock setting.
    if (this.transport.setClock) this.transport.setClock(this._isPal() ? 1 : 2);
  }

  /**
   * Step the emulator one video frame and forward the resulting SID writes to
   * the transport. Called once per rAF (browser) or manually (tests).
   */
  stepAndDrain() {
    this._step();
    this._drain();
  }

  _drain() {
    const head = this._ringHead();
    let tail = this._ringTail();
    if (tail !== head) {
      const heap = this.M.HEAPU8;
      const base = this._ringBase;
      const cap = this._ringCap;
      for (; tail !== head; tail = (tail + 1) >>> 0) {
        const o = base + (tail % cap) * 4;
        const reg = heap[o];
        const val = heap[o + 1];
        const cyc = (heap[o + 2] << 8) | heap[o + 3];
        this.transport.writeCycled(reg, val, cyc);
      }
      this._ringSetTail(head >>> 0);
    }
    // One device flush per VSYNC boundary crossed this frame.
    const fc = this._flushCount();
    if (fc !== this._lastFlush) {
      this._lastFlush = fc;
      this.transport.flush();
    }
  }

  /**
   * One clock tick: advance emulated frames to match elapsed wall-clock time
   * (via performance.now(), so it stays correct across any tick source), scaled
   * by the fast-forward multiplier. Shared by the AudioWorklet and rAF clocks.
   */
  _tick() {
    if (!this._running) return;
    if (this._paused) {
      // Keep the clock anchored so resuming does not burst-step a huge backlog.
      this._lastT = (typeof performance !== 'undefined') ? performance.now() : Date.now();
      return;
    }
    const t = (typeof performance !== 'undefined') ? performance.now() : Date.now();
    let dt = t - this._lastT;
    this._lastT = t;
    if (dt < 0) dt = 0;
    if (dt > 250) dt = 250;               // clamp after a long stall
    this._acc += dt;
    // Fast-forward = advance more emulated frames per real second (shorter
    // effective frame period -> more writes/sec -> faster playback).
    const period = (1000 / this._hz) / this._speed;
    const cap = Math.max(6, Math.ceil(6 * this._speed));
    let steps = 0;
    while (this._acc >= period && steps < cap) {
      this.stepAndDrain();
      this._acc -= period;
      steps++;
    }
    if (this._acc > period) this._acc = period; // drop backlog beyond the cap
  }

  /**
   * Begin playback. Prefers an AudioWorklet clock, whose audio-thread callback
   * keeps firing when the tab is hidden (rAF is frozen in background tabs). If
   * Web Audio is unavailable it falls back to requestAnimationFrame.
   * Pacing is wall-clock based, so the tick source's rate does not matter.
   */
  async start() {
    if (this._running) return;
    this._running = true;
    this._paused = false;
    this._acc = 0;
    this._lastT = (typeof performance !== 'undefined') ? performance.now() : Date.now();
    // Let a transport enter its playback mode (ASID sends its 0x4C start here).
    if (this.transport.playbackStart) this.transport.playbackStart();
    if (typeof AudioContext !== 'undefined' || typeof webkitAudioContext !== 'undefined') {
      try { await this._startAudioClock(); return; }
      catch (e) { /* fall through to rAF */ }
    }
    this._startRafClock();
  }

  _startRafClock() {
    const pump = () => {
      if (!this._running) return;
      this._tick();
      this._rafId = requestAnimationFrame(pump);
    };
    this._rafId = requestAnimationFrame(pump);
  }

  async _startAudioClock() {
    const AC = window.AudioContext || window.webkitAudioContext;
    const ctx = new AC();
    // Inline worklet: posts a message every render quantum (~344 Hz @ 48k),
    // keeps running while the tab is hidden. It produces no audio.
    const src = `
      class UspClock extends AudioWorkletProcessor {
        process() { this.port.postMessage(0); return true; }
      }
      registerProcessor('usp-clock', UspClock);`;
    const url = URL.createObjectURL(new Blob([src], { type: 'application/javascript' }));
    await ctx.audioWorklet.addModule(url);
    URL.revokeObjectURL(url);
    const node = new AudioWorkletNode(ctx, 'usp-clock');
    node.port.onmessage = () => this._tick();
    node.connect(ctx.destination);        // pull the node so process() runs
    await ctx.resume();                   // needs a user gesture (play button)
    this._audio = { ctx, node };
  }

  _stopAudioClock() {
    if (!this._audio) return;
    const { ctx, node } = this._audio;
    try { node.port.onmessage = null; node.disconnect(); } catch (_) {}
    try { ctx.close(); } catch (_) {}
    this._audio = null;
  }

  /** Stop the pump and tear the tune down (and silence the device). */
  stop() {
    this._running = false;
    this._paused = false;
    if (this._rafId) { cancelAnimationFrame(this._rafId); this._rafId = 0; }
    this._stopAudioClock();
    // Silence the device FIRST (before WASM teardown), so the reset reliably
    // goes out even if teardown does nothing. The device holds the last
    // register state otherwise (voices keep sounding).
    if (this.transport.resetSID) this.transport.resetSID();
    else if (this.transport.reset) this.transport.reset();
    this._stop();
  }

  /** Pause/resume: also mute/unmute the device so a sustained note goes quiet. */
  pause(on) {
    this._paused = !!on;
    this._pause(on ? 1 : 0);
    if (on && this.transport.mute) this.transport.mute();
    else if (!on && this.transport.unmute) this.transport.unmute();
  }
  get paused() { return this._paused; }

  /** Set playback speed multiplier (1 = normal, e.g. 4 = 4x fast-forward). */
  setSpeed(mult) { this._speed = Math.max(0.1, Math.min(8, mult || 1)); }

  /** Toggle fast-forward. on -> `mult`x (default 4x), off -> 1x. */
  fastForward(on, mult = 4) { this.setSpeed(on ? mult : 1); }

  /** Set the device SID clock to match a region (uses transport if present). */
  setClock(rateId) { if (this.transport.setClock) this.transport.setClock(rateId); }
  nextSubtune() { this._nextSubtune(); }
  prevSubtune() { this._prevSubtune(); }
  forceSocketTwo() { this._forceSocketTwo(); }
  isPlaying() { return !!this._isPlaying(); }
  droppedWrites() { return this._dropCount(); }
}
