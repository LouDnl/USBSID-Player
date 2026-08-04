/*
 * USBSID-Player web backend.
 *
 * src/web/web_backend.cpp
 *
 * Implements the usbsid_web_* hooks declared in src/web/stub/USBSID.h. The WEB
 * (Emscripten/WASM) build routes every SID register write / flush / read
 * through here. WASM never touches USB: it enqueues cycle-exact
 * (register, value, cycles) triples into a single-producer / single-consumer
 * ring buffer that lives in the WASM heap. The JS side (worker) drains the ring
 * each animation frame and forwards the writes over WebUSB, issuing a device
 * flush at each VSYNC boundary (signalled by the flush counter).
 *
 * Ring layout: USBSID_WEB_RING_ENTRIES entries of 4 bytes each:
 *   [0] reg   [1] val   [2] cycles_hi   [3] cycles_lo
 * head/tail are monotonic uint32 counters (they wrap at 2^32; the consumer
 * computes pending = head - tail, and indexes with (idx % entries)).
 *
 * File author: LouD
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

#include <cstdint>

/* Power-of-two entry count so (idx % entries) is a cheap mask. 8192 entries
 * (32 KiB) comfortably holds a full PAL/NTSC frame of SID writes with headroom. */
#ifndef USBSID_WEB_RING_ENTRIES
#define USBSID_WEB_RING_ENTRIES 8192u
#endif
#define USBSID_WEB_RING_MASK  (USBSID_WEB_RING_ENTRIES - 1u)
#define USBSID_WEB_ENTRY_BYTES 4u

static uint8_t  s_ring[USBSID_WEB_RING_ENTRIES * USBSID_WEB_ENTRY_BYTES];
static volatile uint32_t s_head = 0; /* producer: entries written  */
static volatile uint32_t s_tail = 0; /* consumer: entries read (JS) */

static volatile uint32_t s_flush_count = 0; /* VSYNC boundaries      */
static volatile uint32_t s_drop_count  = 0; /* entries lost to a full ring */
static volatile uint32_t s_write_count = 0; /* total enqueued (diag) */
static volatile long     s_clockrate   = 985248; /* last requested clock */

extern "C" {

void usbsid_web_write(uint8_t reg, uint8_t val, uint16_t cycles)
{
  s_write_count++;
  /* Full when the ring holds ENTRIES pending items. Drop the newest and count
   * it; a correctly-paced consumer drains every frame so this should stay 0. */
  if ((uint32_t)(s_head - s_tail) >= USBSID_WEB_RING_ENTRIES) {
    s_drop_count++;
    return;
  }
  uint32_t off = (s_head & USBSID_WEB_RING_MASK) * USBSID_WEB_ENTRY_BYTES;
  s_ring[off + 0] = reg;
  s_ring[off + 1] = val;
  s_ring[off + 2] = (uint8_t)(cycles >> 8);
  s_ring[off + 3] = (uint8_t)(cycles & 0xFF);
  s_head++;
}

uint8_t usbsid_web_read(uint8_t reg)
{
  /* Real reads would need a round-trip to the device; not supported on the web
   * path yet. SID reads are rare for playback; return 0. */
  (void)reg;
  return 0;
}

void usbsid_web_flush(void)
{
  s_flush_count++;
}

void usbsid_web_reset(void)
{
  /* Drop anything queued; the device is reset independently by JS. */
  s_tail = s_head;
}

void usbsid_web_set_clockrate(long clockrate)
{
  s_clockrate = clockrate;
}

/* ---- Exports consumed by the JS drain loop ---------------------------- */

/* Base address of the ring data in the WASM heap (read via HEAPU8). */
uint8_t* usbsid_web_ring_ptr(void)      { return s_ring; }
uint32_t usbsid_web_ring_entries(void)  { return USBSID_WEB_RING_ENTRIES; }
uint32_t usbsid_web_ring_head(void)     { return s_head; }
uint32_t usbsid_web_ring_tail(void)     { return s_tail; }
void     usbsid_web_ring_set_tail(uint32_t t) { s_tail = t; }

/* Diagnostics / control signals. */
uint32_t usbsid_web_flush_count(void)   { return s_flush_count; }
uint32_t usbsid_web_drop_count(void)    { return s_drop_count; }
uint32_t usbsid_web_write_count(void)   { return s_write_count; }
long     usbsid_web_get_clockrate(void) { return s_clockrate; }

} /* extern "C" */
