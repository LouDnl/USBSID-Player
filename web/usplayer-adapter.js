/*
 * USBSID-Player adapter for the config-tool-web app.
 *
 * usplayer/usplayer-adapter.js
 *
 * Wraps the WASM USBSID-Player (usplayer-web.js) behind the same interface the
 * app's SIDPlayer exposes (load / play / pause / stop / setVolume / getSongInfo
 * / paused / emulator), so it can be used as a drop-in player backend for the
 * `usplayer` (WebUSB) and `usplayer-asid` (Web MIDI) modes.
 *
 * WebUSB mode reuses the app's already-connected USBSIDDevice (injected by
 * createPlayer) - no second USB connection. ASID mode opens Web MIDI itself and
 * uses the output picked in #asid-midi-outputs when present.
 *
 * Loaded as an ES module; registers window.USPlayerAdapter for the classic-script
 * app to instantiate. The WASM factory (window.USBSIDPlayer) is provided by
 * usplayer/usbsid.js, which the page loads as a classic script.
 *
 * Copyright (c) 2026 LouD - GPLv2.
 */

import { USBSIDPlayerWeb } from './usplayer-web.js';
import { USBSIDWebUSBTransport } from './usbsid-webusb.js';
import { ASIDMIDITransport } from './asid-midi.js';

let _modulePromise = null;
function getModule() {
  if (!_modulePromise) {
    // window.USBSIDPlayer is the Emscripten MODULARIZE factory from usbsid.js.
    _modulePromise = window.USBSIDPlayer({ locateFile: (p) => 'usplayer/' + p });
  }
  return _modulePromise;
}

/* Parse a PSID/RSID header for song metadata (no WASM round-trip needed). */
function parseSidInfo(bytes) {
  const info = { maxSubsong: 0, songName: '', songAuthor: '', songReleased: '', numSids: 1 };
  if (!bytes || bytes.length < 0x76) return info;
  const magic = String.fromCharCode(bytes[0], bytes[1], bytes[2], bytes[3]);
  if (magic !== 'PSID' && magic !== 'RSID') return info; // PRG etc: no header
  const be16 = (o) => (bytes[o] << 8) | bytes[o + 1];
  const str = (o) => {
    let s = '';
    for (let i = 0; i < 32 && bytes[o + i]; i++) s += String.fromCharCode(bytes[o + i]);
    return s.trim();
  };
  const songs = be16(0x0e);
  info.maxSubsong = Math.max(0, songs - 1);
  info.songName = str(0x16);
  info.songAuthor = str(0x36);
  info.songReleased = str(0x56);
  info.numSids = countSids(bytes);
  return info;
}

/* SID count from the PSID/RSID header. v3 adds a 2nd SID address at 0x7a, v4 a
 * 3rd at 0x7b; a non-zero byte (the mid-nibbles of $Dxx0) means that SID is
 * present. Header versions < 3 (or short files) are single-SID. */
function countSids(bytes) {
  if (!bytes || bytes.length < 0x7c) return 1;
  const version = (bytes[0x04] << 8) | bytes[0x05];
  let n = 1;
  if (version >= 3 && bytes[0x7a] !== 0) n++;
  if (version >= 4 && bytes[0x7b] !== 0) n++;
  return n;
}

export class USPlayerAdapter {
  /**
   * @param {string} emulator  'usplayer' (WebUSB) or 'usplayer-asid'
   * @param {object} device    the app's connected USBSIDDevice (WebUSB mode)
   */
  constructor(emulator, device) {
    this.emulator = emulator;
    this._device = device || null;
    this._isAsid = (emulator === 'usplayer-asid');
    this._player = null;
    this._transport = null;
    this._bytes = null;
    this._info = { maxSubsong: 0 };
    this._ready = null;    // promise for lazy init
    this._paused = false;
  }

  get paused() { return this._paused; }
  get stopped() { return !this._player || !this._player.isPlaying(); }

  async _ensure() {
    if (this._ready) return this._ready;
    this._ready = (async () => {
      const M = await getModule();
      if (this._isAsid) {
        this._transport = new ASIDMIDITransport();
        try {
          await this._transport.connect(null);   // auto-pick USBSID/first output
          this._wireMidiPicker();
        } catch (e) { console.warn('ASID connect:', e); }
      } else {
        this._transport = new USBSIDWebUSBTransport({ device: this._device });
        try { await this._transport.connect(); } catch (_) {}
      }
      this._player = new USBSIDPlayerWeb(M, this._transport);
      /* Mirror every player write into the app's live register grid, which is
       * otherwise only fed by the jsSID webusb.writeReg path. */
      this._transport.onWrite = (reg, val) => {
        if (typeof window.updateSIDReg === 'function') {
          window.updateSIDReg((reg >> 5) & 0x03, reg & 0x1f, val);
        }
      };
    })();
    return this._ready;
  }

  /**
   * Follow the app's shared #asid-midi-outputs selection, WITHOUT rewriting it.
   * The list is owned by the app/jsSID (option values are numeric indices into
   * jsSID's own MIDIAccess; jsSID reads outputs[select.value]). Rewriting it with
   * our port ids broke Hermit ASID (outputs[<id>] = undefined -> crash). So we
   * only read the selected option's name and map it onto our own transport.
   */
  _wireMidiPicker() {
    const sel = document.getElementById('asid-midi-outputs');
    if (!sel || !this._transport.selectOutputByName || this._midiWired) return;
    const apply = () => {
      const opt = sel.options[sel.selectedIndex];
      if (opt) this._transport.selectOutputByName(opt.textContent);
    };
    apply();                                 // match the current selection by name
    sel.addEventListener('change', apply);   // passive: never touch the options
    this._midiWired = true;
  }

  /** Fetch bytes from a blob:/http: URL. */
  async _fetch(url) {
    const resp = await fetch(url);
    if (!resp.ok) throw new Error('HTTP ' + resp.status);
    return new Uint8Array(await resp.arrayBuffer());
  }

  /**
   * app -> p.load(subtune, timeout, url, callback). subtune is 0-based (0 =
   * default). Fetches the file, loads it, and starts playback.
   */
  async load(subtune, timeout, url, callback) {
    await this._ensure();
    try {
      const bytes = await this._fetch(url);
      this._bytes = bytes;
      this._info = parseSidInfo(bytes);
      /* Size the ASID snapshot + the app register grid to the tune's SID count.
       * ASID drops SID2..4 unless nosids is raised (flush() only emits nosids
       * chips); the grid otherwise hides the extra SID panels. */
      const nsids = this._info.numSids || 1;
      if (this._transport && 'nosids' in this._transport) this._transport.nosids = nsids;
      if (typeof window.updateRegGridSIDCount === 'function') {
        window.updateRegGridSIDCount(nsids);
      }
      const isPrg = /\.(prg|p00)(\?|$)/i.test(url) ||
        !(bytes.length >= 4 &&
          (String.fromCharCode(bytes[0], bytes[1], bytes[2], bytes[3]) === 'PSID' ||
           String.fromCharCode(bytes[0], bytes[1], bytes[2], bytes[3]) === 'RSID'));
      if (isPrg) this._player.loadPRG(bytes);
      else this._player.loadSID(bytes, subtune || 0);
      await this._player.start();
      this._paused = false;
      if (typeof callback === 'function') callback();
    } catch (e) {
      console.error('USPlayerAdapter.load:', e);
    }
  }

  play() {
    if (!this._player) return;
    if (this._paused) { this._player.pause(false); this._paused = false; }
  }

  pause() {
    if (!this._player) return;
    this._player.pause(true);
    this._paused = true;
  }

  stop() {
    if (this._player) this._player.stop();
    this._paused = false;
  }

  setVolume(_v) { /* silence handled by pause()/stop(); no-op */ }

  getSongInfo() { return this._info; }

  /* extras the app may not call but are handy */
  fastForward(on) { if (this._player) this._player.fastForward(on); }
}

if (typeof window !== 'undefined') {
  window.USPlayerAdapter = USPlayerAdapter;
}
