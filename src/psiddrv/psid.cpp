
/*
 * psid.cpp - Adaption for USBSID-player
 *
 * Contains excerpts from vice code, see below.
 *
 * Changes and additions by LouD ~ (c) 2026
 *
 */

/*
 * (original) psid.c / psid.h - PSID file handling.
 *
 * Written by
 *  Dag Lem <resid@nimrod.no>
 *
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA.
 *
 */


#include <cstdio>
#include <cstring>
#include <cstdint>
#if DESKTOP
#include <cstdlib>
#elif EMBEDDED
#include <stdlib.h>
#include <pico/stdlib.h>
#include <pico/malloc.h>
#endif
#include <c64util.h>

/* Sync factors (changed to positive 2016-11-07, BW)  */
#define MACHINE_SYNC_PAL     1
#define MACHINE_SYNC_NTSC    2
#define MACHINE_SYNC_NTSCOLD 3
#define MACHINE_SYNC_PALN    4

extern uint8_t emu_dma_read_ram(uint16_t address);
extern void emu_dma_write_ram(uint16_t address, uint8_t data);
extern "C" int reloc65(char** buf, int* fsize, int addr);

bool is_pal = true;
int numsids = 1;
int sid2loc = 0xd000,  sid3loc = 0xd000;

typedef struct psid_s {
  /* PSID data */
  uint8_t is_rsid;
  uint16_t version;
  uint16_t data_offset;
  uint16_t load_addr;
  uint16_t init_addr;
  uint16_t play_addr;
  uint16_t songs;
  uint16_t start_song;
  uint32_t speed;
  /* psid v3 allows all 32 bytes to be used with no zero termination */
  uint8_t name[32 + 1];
  uint8_t author[32 + 1];
  uint8_t copyright[32 + 1];
  uint16_t flags;
  uint8_t start_page;
  uint8_t max_pages;
  uint16_t reserved;
  uint16_t data_size;
  uint8_t data[65536];

  /* Non-PSID data */
  uint32_t frames_played;
  uint16_t load_last_addr;
} psid_t;

#define PSID_V1_DATA_OFFSET 0x76
#define PSID_V2_DATA_OFFSET 0x7c

static psid_t* psid = NULL;
static int psid_tune = 0;       /* currently selected tune, 0: default 1: first, 2: second, etc */

#if DESKTOP
void *psid_calloc(size_t nmemb, size_t size)
{
  void *ptr;
  ptr = calloc(nmemb, size);

  if (ptr == NULL && (size * nmemb) > 0) {
    MOSDBG("[PSID] error: lib_calloc failed\n");
    return ptr;
  }

  return ptr;
}
#endif

#if EMBEDDED
int MEM_fread(unsigned char *buf, size_t size, size_t n, uint8_t **f) {
    memcpy(buf, *f, size * n);
    *f += size * n;
    return n;
}
#endif

static uint16_t psid_extract_word(uint8_t **buf)
{
  uint16_t word = (*buf)[0] << 8 | (*buf)[1];
  *buf += 2;
  return word;
}

#if DESKTOP
int psid_load_file(const char* filename, int subtune)
{
  FILE* f;
#elif EMBEDDED
int psid_load_file(uint8_t * binary_, size_t binsize_, int subtune)
{
#endif
  uint8_t buf[PSID_V2_DATA_OFFSET + 2];
  uint8_t *ptr = buf;
  unsigned int length;

  /* HACK: the selected tune number is handled by the "PSIDtune" resource, which
   *     is actually saved in the ini file, and thus loaded and restored at
   *     startup. however, we do not want that. instead we want the default
   *     tune of the respective .sid file to be used, or the explicit tune
   *     number given on commandline (if any).
   */
  psid_tune = subtune;
#if DESKTOP
  if (!(f = fopen(filename, "r\n"))) {
    return 0;
  }
#endif
  free(psid);
  psid = (psid_t*)calloc(sizeof(psid_t), 1);

  if (
#if DESKTOP
    fread(ptr, 1, 6, f) != 6 ||
#elif EMBEDDED
    MEM_fread(ptr, 1, 6, &binary_) != 6 ||
#endif
    (memcmp(ptr, "PSID", 4) != 0 && memcmp(ptr, "RSID", 4) != 0)) {
    MOSDBG("[PSID] SID file validation failed!\n");
    goto fail;
  }
  psid->is_rsid = (ptr[0] == 'R');

  ptr += 4;
  psid->version = psid_extract_word(&ptr);

  if (psid->version < 1 || psid->version > 4) {
    MOSDBG("[PSID] Unknown PSID version number: %d.\n", (int)psid->version);
    goto fail;
  }
  MOSDBG("[PSID] PSID version number: %d.\n", (int)psid->version);

  length = (unsigned int)((psid->version == 1 ? PSID_V1_DATA_OFFSET : PSID_V2_DATA_OFFSET) - 6);
#if DESKTOP
  if (fread(ptr, 1, length, f) != length) {
#elif EMBEDDED
  if (MEM_fread(ptr, 1, length, &binary_) != length) {
#endif
    MOSDBG("[PSID] Error reading PSID header.\n");
    goto fail;
  }

  psid->data_offset = psid_extract_word(&ptr);
  psid->load_addr = psid_extract_word(&ptr);
  psid->init_addr = psid_extract_word(&ptr);
  psid->play_addr = psid_extract_word(&ptr);
  psid->songs = psid_extract_word(&ptr);
  psid->start_song = psid_extract_word(&ptr);
  psid->speed = psid_extract_word(&ptr) << 16;
  psid->speed |= psid_extract_word(&ptr);
  psid->frames_played = 0;
  memcpy(psid->name, ptr, 32);
  psid->name[32] = '\0';
  ptr += 32;
  memcpy(psid->author, ptr, 32);
  psid->author[32] = '\0';
  ptr += 32;
  memcpy(psid->copyright, ptr, 32);
  psid->copyright[32] = '\0';
  ptr += 32;
  if (psid->version >= 2) {
    psid->flags = psid_extract_word(&ptr);
    psid->start_page = *ptr++;
    psid->max_pages = *ptr++;
    psid->reserved = psid_extract_word(&ptr);
  } else {
    psid->flags = 0;
    psid->start_page = 0;
    psid->max_pages = 0;
    psid->reserved = 0;
  }

  if ((psid->start_song < 1) || (psid->start_song > psid->songs)) {
    MOSDBG("[PSID] Default tune out of range (%d of %d ?), using 1 instead.\n", psid->start_song, psid->songs);
    psid->start_song = 1;
  }

  /* Zero load address => the load address is stored in the
     first two bytes of the binary C64 data. */
  if (psid->load_addr == 0) {
#if DESKTOP
    if (fread(ptr, 1, 2, f) != 2) {
#elif EMBEDDED
  if (MEM_fread(ptr, 1, 2, &binary_) != 2) {
#endif
      MOSDBG("[PSID] Error reading PSID load address.\n");
      goto fail;
    }
    psid->load_addr = ptr[0] | ptr[1] << 8;
  }

  /* Zero init address => use load address. */
  if (psid->init_addr == 0) {
    psid->init_addr = psid->load_addr;
  }

  /* Read binary C64 data. */
#if DESKTOP
  psid->data_size = (uint16_t)fread(psid->data, 1, sizeof(psid->data), f);
#elif EMBEDDED
  // psid->data_size = /* (uint16_t) */MEM_fread(psid->data, 1, (binsize_-(length+2)), &binary_);
  psid->data_size = /* (uint16_t) */MEM_fread(psid->data, 1, (binsize_-length), &binary_);
#endif
  psid->load_last_addr = (psid->load_addr + psid->data_size - 1);
#if DESKTOP
  if (ferror(f)) {
    MOSDBG("[PSID] Reading PSID data.\n");
    goto fail;
  }

  if (!feof(f)) {
    MOSDBG("[PSID] More than 64KiB PSID data.\n");
    goto fail;
  }
#endif
  /* Relocation setup. */
  if (psid->start_page == 0x00) {
    /* Start and end pages. */
    int startp = psid->load_addr >> 8;
    int endp = psid->load_last_addr >> 8;

    /* Used memory ranges. */
    unsigned int used[] = {
      0x00,
      0x03,
      0xa0,
      0xbf,
      0xd0,
      0xff,
      0x00,
      0x00
    };    /* calculated below */

    unsigned int pages[256];
    unsigned int last_page = 0;
    unsigned int i, page, tmp;

    MOSDBG("[PSID] No PSID freepages set, recalculating...\n");

    /* finish initialization */
    used[6] = startp; used[7] = endp;

    /* Mark used pages in table. */
    memset(pages, 0, sizeof(pages));
    for (i = 0; i < sizeof(used) / sizeof(*used); i += 2) {
      for (page = used[i]; page <= used[i + 1]; page++) {
        pages[page] = 1;
      }
    }

    /* Find largest free range. */
    psid->max_pages = 0x00;
    for (page = 0; page < sizeof(pages) / sizeof(*pages); page++) {
      if (!pages[page]) {
        continue;
      }
      tmp = page - last_page;
      if (tmp > psid->max_pages) {
        psid->start_page = last_page;
        psid->max_pages = tmp;
      }
      last_page = page + 1;
    }

    if (psid->max_pages == 0x00) {
      psid->start_page = 0xff;
    }
  }

  if (psid->start_page == 0xff || psid->max_pages < 2) {
    MOSDBG("[PSID] No space for driver.\n");
    goto fail;
  }
#if DESKTOP
  fclose(f);
#endif
  return 1;

fail:
  MOSDBG("[PSID] Load SID file failed!\n");
#if DESKTOP
  fclose(f);
#endif
  free(psid);
  psid = NULL;

  return 0;
}

void psid_shutdown(void)
{
  free(psid);
  psid = NULL;
}

/* Use CBM80 vector to start PSID driver. This is a simple method to
   transfer control to the PSID driver while running in a pure C64
   environment. */
static int psid_set_cbm80(uint16_t vec, uint16_t addr)
{
  unsigned int i;
  uint8_t cbm80[] = { 0x00, 0x00, 0x00, 0x00, 0xc3, 0xc2, 0xcd, 0x38, 0x30 };

  cbm80[0] = vec & 0xff;
  cbm80[1] = vec >> 8;

  for (i = 0; i < sizeof(cbm80); i++) {
    /* make backup of original content at 0x8000 */
    emu_dma_write_ram((uint16_t)(addr + i), emu_dma_read_ram((uint16_t)(0x8000 + i)));
    /* copy header */
    emu_dma_write_ram((uint16_t)(0x8000 + i), cbm80[i]);
  }

  return i;
}

void psid_init_tune(int install_driver_hook)
{
  int start_song = psid_tune;
  int sync, sid_model;
  int i;
  uint16_t reloc_addr;
  uint16_t addr;
  int speedbit;
  // char* irq;
  const char* irq;
  char irq_str[20];
  const char csidflag[4][8] = { "UNKNOWN", "6581", "8580", "ANY"};

  if (!psid) {
    return;
  }

  psid->frames_played = 0;

  reloc_addr = psid->start_page << 8;

  MOSDBG("[PSID] Driver=$%04X, Image=$%04X-$%04X, Init=$%04X, Play=$%04X\n",
    reloc_addr, psid->load_addr,
    (unsigned int)(psid->load_addr + psid->data_size - 1),
    psid->init_addr, psid->play_addr);

  /* PAL/NTSC. */
  // resources_get_int("MachineVideoStandard", &sync);

  /* MOS6581/MOS8580. */
  // resources_get_int("SidModel", &sid_model);

  /* Check tune number. */
  /* MOSDBG("[PSID] start_song: %d psid->start_song %d\n", start_song, psid->start_song); */

  if (start_song == 0) {
    start_song = psid->start_song;
  } else if (start_song < 1 || start_song > psid->songs) {
    MOSDBG("[PSID] Tune out of range.\n");
    start_song = psid->start_song;
  }

  /* Check for PlaySID specific file. */
  if (psid->flags & 0x02 && !psid->is_rsid) {
    MOSDBG("[PSID] Image is PlaySID specific - trying anyway.\n");
  }

  /* Check tune speed. */
  speedbit = 1;
  for (i = 1; i < start_song && i < 32; i++) {
    speedbit <<= 1;
  }

  irq = psid->speed & speedbit ? "CIA 1" : "VICII";

  if (psid->play_addr) {
    strcpy(irq_str, irq);
  } else {
    sprintf(irq_str, "custom (%s ?)", irq);
  }

  sync = (is_pal ? MACHINE_SYNC_PAL : MACHINE_SYNC_NTSC); /* Added by LouD */

  MOSDBG("[PSID]    Title: %s\n", (char *) psid->name);
  MOSDBG("[PSID]   Author: %s\n", (char *) psid->author);
  MOSDBG("[PSID] Released: %s\n", (char *) psid->copyright);
  MOSDBG("[PSID] Using %s sync\n", sync == MACHINE_SYNC_PAL ? "PAL" : "NTSC");
  MOSDBG("[PSID] SID model: %s\n", csidflag[(psid->flags >> 4) & 3]);
  MOSDBG("[PSID] Using %s interrupt\n", irq_str);
  MOSDBG("[PSID] Playing tune %d out of %d (default=%d)\n", start_song, psid->songs, psid->start_song);

  /* Store parameters for PSID player. */
  if (install_driver_hook) {
    /* Skip JMP. */
    addr = reloc_addr + 3 + 9;

    /* CBM80 reset vector. */
    addr += psid_set_cbm80((uint16_t)(reloc_addr + 9), addr);

    emu_dma_write_ram(addr, (uint8_t)(start_song));
  }

  /* put song number into address 780/1/2 (A/X/Y) for use by BASIC tunes */
  emu_dma_write_ram(780, (uint8_t)(start_song - 1));
  emu_dma_write_ram(781, (uint8_t)(start_song - 1));
  emu_dma_write_ram(782, (uint8_t)(start_song - 1));
  /* force flag in c64 memory, many sids reads it and must be set AFTER the sid flag is read */
  emu_dma_write_ram((uint16_t)(0x02a6), (uint8_t)(sync == MACHINE_SYNC_NTSC ? 0 : 1));
}

void psid_init_driver(void)
{
  // uint8_t psid_driver[] = {
#include <psiddrv.h>
  // };
  char *psid_reloc = (char *)psid_driver;
  int psid_size;

  uint16_t reloc_addr;
  uint16_t addr;
  int i;
  int sync;
  // int sid2loc, sid3loc;

  if (!psid) {
    return;
  }

  /* C64 PAL/NTSC flag. */
  // resources_get_int("MachineVideoStandard", &sync);
  // if (!keepenv) {
    switch ((psid->flags >> 2) & 0x03) {
      case 0x01:
        is_pal = true;
        sync = MACHINE_SYNC_PAL;
        // resources_set_int("MachineVideoStandard", sync);
        break;
      case 0x02:
        is_pal = false;
        sync = MACHINE_SYNC_NTSC;
        // resources_set_int("MachineVideoStandard", sync);
        break;
      default:
        /* Keep settings (00 = unknown, 11 = any) */
        break;
    }
  // }

  /* Stereo SID specification support from Wilfred Bos.
    * Top byte of reserved holds the middle nybbles of
    * the 2nd chip address. */
  // resources_set_int("SidStereo", 0);
  if (psid->version >= 3) {
    sid2loc = 0xd000 | ((psid->reserved >> 4) & 0x0ff0);
    MOSDBG("[PSID] 2nd SID at $%04x\n", (unsigned int)sid2loc);
    if (((sid2loc >= 0xd420 && sid2loc < 0xd800) || sid2loc >= 0xde00)
      && (sid2loc & 0x10) == 0) {
        numsids++;
      // resources_set_int("SidStereo", 1);
      // resources_set_int("Sid2AddressStart", sid2loc);
    }
    sid3loc = 0xd000 | ((psid->reserved << 4) & 0x0ff0);
    if (sid3loc != 0xd000) {
      MOSDBG("[PSID] 3rd SID at $%04x\n", (unsigned int)sid3loc);
      if (((sid3loc >= 0xd420 && sid3loc < 0xd800) || sid3loc >= 0xde00)
        && (sid3loc & 0x10) == 0) {
          numsids++;
        // resources_set_int("SidStereo", 2);
        // resources_set_int("Sid3AddressStart", sid3loc);
      }
    }
  }

  /* MOS6581/MOS8580 flag. */
  // if (!keepenv) {
  //   switch ((psid->flags >> 4) & 0x03) {
  //     case 0x01:
  //      resources_set_int("SidModel", 0);
  //       break;
  //     case 0x02:
  //       resources_set_int("SidModel", 1);
  //       break;
  //     default:
  //       /* Keep settings (00 = unknown, 11 = any) */
  //       break;
  //   }
  //   /* FIXME: second chip model is ignored,
  //     * but it is stored at (flags >> 6) & 3. */
  // }

  /* Clear low memory to minimize the damage of PSIDs doing bad reads. */
  for (addr = 0; addr < 0x0800; addr++) {
    emu_dma_write_ram(addr, (uint8_t)0x00);
  }

  /* Relocation of C64 PSID driver code. */
  reloc_addr = psid->start_page << 8;
  psid_size = sizeof(psid_driver);
  MOSDBG("[PSID] PSID free pages: $%04x-$%04x\n",
    reloc_addr, (reloc_addr + (psid->max_pages << 8)) - 1U);

  if (!reloc65((char **)&psid_reloc, &psid_size, reloc_addr)) {
    MOSDBG("[PSID] Relocation.\n");
    // psid_set_tune(-1);
    return;
  }

  for (i = 0; i < psid_size; i++) {
    emu_dma_write_ram((uint16_t)(reloc_addr + i), psid_reloc[i]);
  }

  /* Store binary C64 data. */
  for (i = 0; i < psid->data_size; i++) {
    emu_dma_write_ram((uint16_t)(psid->load_addr + i), psid->data[i]);
  }

  /* Skip JMP and CBM80 reset vector. */
  addr = reloc_addr + 3 + 9 + 9;

  /* Store parameters for PSID player. */
  emu_dma_write_ram(addr++, (uint8_t)(0));
  emu_dma_write_ram(addr++, (uint8_t)(psid->songs));
  emu_dma_write_ram(addr++, (uint8_t)(psid->load_addr & 0xff));
  emu_dma_write_ram(addr++, (uint8_t)(psid->load_addr >> 8));
  emu_dma_write_ram(addr++, (uint8_t)(psid->init_addr & 0xff));
  emu_dma_write_ram(addr++, (uint8_t)(psid->init_addr >> 8));
  emu_dma_write_ram(addr++, (uint8_t)(psid->play_addr & 0xff));
  emu_dma_write_ram(addr++, (uint8_t)(psid->play_addr >> 8));
  emu_dma_write_ram(addr++, (uint8_t)(psid->speed & 0xff));
  emu_dma_write_ram(addr++, (uint8_t)((psid->speed >> 8) & 0xff));
  emu_dma_write_ram(addr++, (uint8_t)((psid->speed >> 16) & 0xff));
  emu_dma_write_ram(addr++, (uint8_t)(psid->speed >> 24));
  emu_dma_write_ram(addr++, (uint8_t)((int)sync == MACHINE_SYNC_PAL ? 1 : 0));
  emu_dma_write_ram(addr++, (uint8_t)(psid->load_last_addr & 0xff));
  emu_dma_write_ram(addr++, (uint8_t)(psid->load_last_addr >> 8));
}
