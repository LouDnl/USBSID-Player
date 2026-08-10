/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback, for embedding on RP2350 (Pico2), and in a browser.
 *
 * web/usplayer-worker.js
 * The emulation and the USB writes, off the main thread.
 *
 * Why: the main thread is not a reliable clock. Playing on it, the tune
 * crackled slightly while the tab was in front and audibly broke up when the
 * tab went to the back, because a backgrounded page stops feeding the board and
 * the board runs out of writes. The AudioWorklet clock already ran on the audio
 * thread, but it could only hand a tick to the main thread, which is where the
 * work was.
 *
 * So the work moves here. This is a module worker holding the same
 * `USBSIDPlayerWeb` the page would have used, with two differences:
 *
 *   - the clock reaches it directly. The page transfers one end of a
 *     MessageChannel into the AudioWorkletProcessor and the other end here, so
 *     the audio thread posts ticks straight to this worker and the main thread
 *     is not in the path at all.
 *   - the board is opened here. WebUSB permission belongs to the origin rather
 *     than to a thread, so once the page has asked once, `getDevices()` finds
 *     the board from a worker without a picker. See connectGranted().
 *
 * Web MIDI has no worker API, so ASID cannot come along; the page falls back to
 * running on the main thread for that. See usplayer-worker-client.js.
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

import { USBSIDPlayerWeb } from './usplayer-web.js';
import { USBSIDWebUSBTransport } from './usbsid-webusb.js';

let player = null;
let transport = null;
let clockPort = null;

/** Everything the page shows, in one message rather than a call per field. */
function snapshot() {
  if (player === null) return null;
  const info = player.info();
  return {
    playing: player.isPlaying(),
    prg: player.isPrg(),
    frames: player.frames(),
    sidWrites: player.sidWrites(),
    dropped: player.droppedWrites(),
    queueDepth: (transport && transport.queueDepth) || 0,
    refreshHz: player.refreshHz(),
    name: info.name,
    author: info.author,
    released: info.released,
    song: info.song,
    songs: info.songs,
    stats: player.stats(),
  };
}

function post(type, payload, id) {
  self.postMessage({ type, payload, id });
}

/* The tick has to be cheap and must never throw into the audio thread's port,
 * so the state the page wants is sampled on a slow beat rather than every one
 * of the ~344 ticks a second. */
let sinceReport = 0;
function onTick() {
  if (player === null) return;
  player.tick();
  if (++sinceReport >= 32) {
    sinceReport = 0;
    post('state', snapshot());
  }
}

const handlers = {
  /**
   * Load the wasm and open the board. `wasmUrl` is passed in rather than
   * hard coded because the page decides where the build artefacts live.
   */
  async init({ wasmUrl }) {
    const { default: USBSIDPlayer } = await import(wasmUrl);
    const M = await USBSIDPlayer();
    transport = new USBSIDWebUSBTransport();
    const opened = await transport.connectGranted();
    player = new USBSIDPlayerWeb(M, transport);
    /* The worker owns the board here, so it is the only one that can ask what
     * is in it. Done at init, before any tune loads. */
    const board = opened ? await player.applyBoardConfig() : null;
    return { opened, board };
  },

  /** The audio thread's end of the clock. */
  clock({ port }) {
    clockPort = port;
    clockPort.onmessage = onTick;
    clockPort.start && clockPort.start();
    return { ok: true };
  },

  loadSID({ bytes, subtune }) {
    const ok = player.loadSID(new Uint8Array(bytes), subtune || 0);
    return { ok, info: snapshot() };
  },

  loadPRG({ bytes }) {
    const ok = player.loadPRG(new Uint8Array(bytes));
    return { ok, info: snapshot() };
  },

  async start() { await player.start({ externalClock: true }); return { ok: true }; },
  stop() { player.stop(); return { ok: true }; },
  pause({ on }) { player.pause(!!on); return { ok: true }; },
  speed({ mult }) { player.setSpeed(mult); return { ok: true }; },
  fastForward({ on, mult }) { player.fastForward(!!on, mult); return { ok: true }; },
  nextSubtune() { player.nextSubtune(); return { ok: true }; },
  prevSubtune() { player.prevSubtune(); return { ok: true }; },
  runStop() { return { ok: player.runStop() }; },
  forceSocketTwo() { player.forceSocketTwo(); return { ok: true }; },
  setClock({ rateId }) { player.setClock(rateId); return { ok: true }; },
  setSidConfig({ numsids, one, two, fmopl }) {
    player.setSidConfig(numsids || 0, one || 0, two || 0,
                        (fmopl === undefined) ? -1 : fmopl);
    return { ok: true, board: player.boardConfig() };
  },
  async applyBoardConfig() { return { board: await player.applyBoardConfig() }; },
  state() { return snapshot(); },
  resetStats() { player.resetStats(); return { ok: true }; },
};

self.onmessage = async (e) => {
  const { type, payload, id } = e.data || {};
  const fn = handlers[type];
  if (!fn) { post('error', { message: 'unknown message ' + type }, id); return; }
  try {
    const result = await fn(payload || {});
    post('reply', result, id);
  } catch (err) {
    post('error', { message: String(err && err.message ? err.message : err) }, id);
  }
};

/**
 * The worker paces itself when there is no audio clock.
 *
 * Only a fallback: a worker's timers are throttled in a backgrounded tab, which
 * is the case the audio clock exists for. It is here so that a page without Web
 * Audio, or one whose AudioContext never got its user gesture, still plays.
 */
let fallbackTimer = 0;
self.addEventListener('message', (e) => {
  if (!e.data || e.data.type !== 'fallbackClock') return;
  if (fallbackTimer) { clearInterval(fallbackTimer); fallbackTimer = 0; }
  if (e.data.payload && e.data.payload.on) {
    fallbackTimer = setInterval(onTick, 4);
  }
});
