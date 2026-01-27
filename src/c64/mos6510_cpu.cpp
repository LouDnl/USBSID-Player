/*
 * USBSID-Player aims to be a command line SID file player that is also
 * suited for embedding where both implementations target use
 * with USBSID-Pico. USBSID-Pico is a RPi Pico/PicoW (RP2040) &
 * Pico2/Pico2W (RP2350) based board for interfacing one or two
 * MOS SID chips and/or hardware SID emulators over (WEB)USB with
 * your computer, phone or ASID supporting player
 *
 * Parts if this emulator are based on other great emulators and players
 * like Vice, SidplayFp, Websid, SidBerry and emudore/adorable
 *
 * mos6510_cpu.cpp
 * This file is part of USBSID-Player (https://github.com/LouDnl/USBSID-Player)
 * File author: LouD
 *
 * Copyright (c) 2025-2026 LouD
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
 *
 */

#include <mmu.h>
#include <mos6510_cpu.h>
#include <mos6560_6561_vic.h>
#include <mos6526_cia.h>
#include <c64util.h>


/**
 * @brief Init static CPU variables
 */
CPUCLOCK mos6510::cycles_ = 0;
CPUCLOCK mos6510::prev_cycles_ = 0;

bool mos6510::loginstructions = false;
bool mos6510::pending_interrupt = false;
bool mos6510::irq_pending = false;
bool mos6510::nmi_pending = false;

val_t mos6510::last_insn = 0;
addr_t mos6510::pc_address = 0;
addr_t mos6510::d_address = 0;

/**
 * @brief Construct a new mos6510::mos6510 object
 *
 * @param r
 * @param w
 */
mos6510::mos6510(BusRead r, BusWrite w)
{
  MOSDBG("[CPU] Init\n");

  read_bus = (BusRead)r;
  write_bus = (BusWrite)w;
  fill_instruction_table();

  a_ = x_ = y_ = sp_ = 0x0;
  _flags = 0b00110000;


  return;
}

/**
 * @brief Destroy the mos6510::mos6510 object
 *
 */
mos6510::~mos6510()
{
  MOSDBG("[CPU] Deinit\n");
  initialized = false;
  instruction_table.fill(InstructionHandler{});

  return;
}

/**
 * @brief Glue required C64 modules
 *
 */
void __us_not_in_flash_func mos6510::glue_c64(mmu *_mmu, mos6560_6561 *_vic, mos6526 *_cia1, mos6526 *_cia2)
{
  mmu_ = _mmu;
  vic  = _vic;
  cia1 = _cia1;
  cia2 = _cia2;

  return;
}

/**
 * @brief Performs a cold reset
 *
 * https://www.c64-wiki.com/index.php/Reset_(Process)
 */
void __us_not_in_flash_func mos6510::reset(void)
{
  a_ = x_ = y_ = 0;
  sp_ = 0xFD;
  _flags = 0b00110000;
  val_t pc_lo = load_byte(pAddrResetVector);
  val_t pc_hi = load_byte(pAddrResetVector+1);
  pc(pc_lo | pc_hi << 8);
  prev_cycles_ = 0, cycles_ = 6;

  return;
}

/**
 * @brief Performs a "hot" reset
 *
 */
void __us_not_in_flash_func mos6510::hot_reset(void)
{
  _flags = 0b00110000;
  a_ = x_ = y_ = 0;
  sp_ = 0xFD;

  return;
}

/**
 * @brief Fill the CPU instruction table with opcodes
 *
 */
void mos6510::fill_instruction_table(void)
{
	if (initialized) return;
	initialized = true;
  { /* Fill instruction_table */
    /* 0x00 ~ 0x0F */
    instruction_table[0x00] = [this]() { this->brk(); };                                           /* BRK impl */
    instruction_table[0x01] = [this]() { this->ora(this->load_byte(this->addr_indx()), 6); };      /* ORA (ind,X) */
    instruction_table[0x02] = [this]() { this->jam(0x02); };                                       /* JAM ~ Illegal OPCode */
    instruction_table[0x03] = [this]() { this->slo(this->addr_indx(), 5, 3); };                    /* SLO (ind,X) ~ Illegal OPCode */
    instruction_table[0x04] = [this]() { this->load_byte(addr_zero()); this->nop(3); };            /* NOP zpg ~ Illegal OPCode */
    instruction_table[0x05] = [this]() { this->ora(this->load_byte(this->addr_zero()), 3); };      /* ORA zpg */
    instruction_table[0x06] = [this]() { this->asl_mem(this->addr_zero(), 5); };                   /* ASL zpg */
    instruction_table[0x07] = [this]() { this->slo(this->addr_zero(), 3, 2); };                    /* SLO zpg ~ Illegal OPCode */
    instruction_table[0x08] = [this]() { this->php(); };                                           /* PHP impl */
    instruction_table[0x09] = [this]() { this->ora(this->fetch_op(), 2); };                        /* ORA #imm */
    instruction_table[0x0A] = [this]() { this->asl_a(); };                                         /* ASL A */
    instruction_table[0x0B] = [this]() { this->anc(this->fetch_op()); };                           /* ANC(AAC) #imm ~ Illegal OPCode */
    instruction_table[0x0C] = [this]() { this->load_byte(this->addr_abs()); this->nop(4); };       /* NOP abs  ~ Illegal OPCode */
    instruction_table[0x0D] = [this]() { this->ora(this->load_byte(this->addr_abs()), 4); };       /* ORA abs */
    instruction_table[0x0E] = [this]() { this->asl_mem(this->addr_abs(), 6); };                    /* ASL abs */
    instruction_table[0x0F] = [this]() { this->slo(this->addr_abs(), 3, 3); };                     /* SLO abs ~ Illegal OPCode */
    /* 0x10 ~ 0x1F */
    instruction_table[0x10] = [this]() { this->bpl(); };                                           /* BPL rel */
    instruction_table[0x11] = [this]() { this->ora(this->load_byte(this->addr_indy()), 5); };      /* ORA (ind),Y */
    instruction_table[0x12] = [this]() { this->jam(0x12); };                                       /* JAM ~ Illegal OPCode */
    instruction_table[0x13] = [this]() { this->slo(this->addr_indy(), 5, 3); };                    /* SLO (ind),Y ~ Illegal OPCode */
    instruction_table[0x14] = [this]() { this->load_byte(this->addr_zerox()); this->nop(4); };     /* NOP zpg,X ~ Illegal OPCode */
    instruction_table[0x15] = [this]() { this->ora(this->load_byte(this->addr_zerox()), 4); };     /* ORA zpg,X */
    instruction_table[0x16] = [this]() { this->asl_mem(this->addr_zerox(), 6); };                  /* ASL zpg,X */
    instruction_table[0x17] = [this]() { this->slo(this->addr_zerox(), 2, 3); };                   /* SLO zpg,X ~ Illegal OPCode */
    instruction_table[0x18] = [this]() { this->clc(); };                                           /* CLC impl */
    instruction_table[0x19] = [this]() { this->ora(this->load_byte(this->addr_absy()), 4); };      /* ORA abs,Y */
    instruction_table[0x1A] = [this]() { this->nop(2); };                                          /* NOP impl ~ Illegal OPCode */
    instruction_table[0x1B] = [this]() { this->slo(this->addr_absy(), 4, 2); };                    /* SLO abs,Y ~ Illegal OPCode */
    instruction_table[0x1C] = [this]() { this->load_byte(this->addr_absx()); this->nop(4); };      /* NOP abs,X ~ Illegal OPCode */
    instruction_table[0x1D] = [this]() { this->ora(this->load_byte(this->addr_absx()), 4); };      /* ORA abs,X */
    instruction_table[0x1E] = [this]() { this->asl_mem(this->addr_absx(), 7); };                   /* ASL abs,X */
    instruction_table[0x1F] = [this]() { this->slo(this->addr_absx(), 4, 2); };                    /* SLO abs,X ~ Illegal OPCode */
    /* 0x20 ~ 0x2F */
    instruction_table[0x20] = [this]() { this->jsr(); };                                           /* JSR abs */
    instruction_table[0x21] = [this]() { this->_and(this->load_byte(this->addr_indx()), 6); };     /* AND (ind,X) */
    instruction_table[0x22] = [this]() { this->jam(0x22); };                                       /* JAM ~ Illegal OPCode */
    instruction_table[0x23] = [this]() { this->rla(this->addr_indx(), 5, 3); };                    /* RLA (ind,X) ~ Illegal OPCode */
    instruction_table[0x24] = [this]() { this->bit(this->addr_zero(), 3); };                       /* BIT zpg */
    instruction_table[0x25] = [this]() { this->_and(this->load_byte(this->addr_zero()), 3); };     /* AND zpg */
    instruction_table[0x26] = [this]() { this->rol_mem(this->addr_zero(), 5); };                   /* ROL zpg */
    instruction_table[0x27] = [this]() { this->rla(this->addr_zero(), 3, 2); };
    instruction_table[0x28] = [this]() { this->plp(); };
    instruction_table[0x29] = [this]() { this->_and(this->fetch_op(), 2); };
    instruction_table[0x2A] = [this]() { this->rol_a(); };
    instruction_table[0x2B] = [this]() { this->lxa(this->fetch_op(), 2); };
    instruction_table[0x2C] = [this]() { this->bit(this->addr_abs(), 4); };
    instruction_table[0x2D] = [this]() { this->_and(this->load_byte(this->addr_abs()), 4); };
    instruction_table[0x2E] = [this]() { this->rol_mem(this->addr_abs(), 6); };
    instruction_table[0x2F] = [this]() { this->rla_(this->addr_abs(), 4, 2); };                     /* RLA abs ~ Illegal OPCode */

    instruction_table[0x30] = [this]() { this->bmi(); };
    instruction_table[0x31] = [this]() { this->_and(this->load_byte(this->addr_indy()), 5); };
    instruction_table[0x32] = [this]() { this->jam(0x32); };
    instruction_table[0x33] = [this]() { this->rla(this->addr_indy(), 5, 3); };
    instruction_table[0x34] = [this]() { this->load_byte(this->addr_zerox()); this->nop(4); };
    instruction_table[0x35] = [this]() { this->_and(this->load_byte(this->addr_zerox()), 4); };
    instruction_table[0x36] = [this]() { this->rol_mem(this->addr_zerox(), 6); };
    instruction_table[0x37] = [this]() { this->rla(this->addr_zerox(), 4, 2); };
    instruction_table[0x38] = [this]() { this->sec(); };
    instruction_table[0x39] = [this]() { this->_and(this->load_byte(this->addr_absy()), 4); };
    instruction_table[0x3A] = [this]() { this->nop(2); };
    instruction_table[0x3B] = [this]() { this->rla(this->addr_absy(), 4, 2); };
    instruction_table[0x3C] = [this]() { this->load_byte(this->addr_absx()); this->nop(4); };
    instruction_table[0x3D] = [this]() { this->_and(this->load_byte(this->addr_absx()), 4); };
    instruction_table[0x3E] = [this]() { this->rol_mem(this->addr_absx(), 7); };
    instruction_table[0x3F] = [this]() { this->rla(this->addr_absx(), 4, 2); };

    instruction_table[0x40] = [this]() { this->rti(); };
    instruction_table[0x41] = [this]() { this->eor(this->load_byte(this->addr_indx()), 6); };
    instruction_table[0x42] = [this]() { this->jam(0x42); };
    instruction_table[0x43] = [this]() { this->sre(this->addr_indx(), 5, 3); };
    instruction_table[0x44] = [this]() { this->load_byte(this->addr_zero()); this->nop(3); };
    instruction_table[0x45] = [this]() { this->eor(this->load_byte(this->addr_zero()), 3); };
    instruction_table[0x46] = [this]() { this->lsr_mem(this->addr_zero(), 5); };
    instruction_table[0x47] = [this]() { this->sre(this->addr_zero(), 3, 2); };
    instruction_table[0x48] = [this]() { this->pha(); };
    instruction_table[0x49] = [this]() { this->eor(this->fetch_op(), 2); };
    instruction_table[0x4A] = [this]() { this->lsr_a(); };
    instruction_table[0x4B] = [this]() { this->_and(fetch_op(),0); this->lsr_a(); };
    instruction_table[0x4C] = [this]() { this->jmp(); };
    instruction_table[0x4D] = [this]() { this->eor(this->load_byte(this->addr_abs()), 4); };
    instruction_table[0x4E] = [this]() { this->lsr_mem(this->addr_abs(), 6); };
    instruction_table[0x4F] = [this]() { this->sre(this->addr_abs(), 4, 2); };

    instruction_table[0x50] = [this]() { this->bvc(); };
    instruction_table[0x51] = [this]() { this->eor(this->load_byte(this->addr_indy()), 5); };
    instruction_table[0x52] = [this]() { this->jam(0x52); };
    instruction_table[0x53] = [this]() { this->sre(this->addr_indy(), 5, 3); };
    instruction_table[0x54] = [this]() { this->load_byte(this->addr_zerox()); this->nop(4); };
    instruction_table[0x55] = [this]() { this->eor(this->load_byte(this->addr_zerox()), 4); };
    instruction_table[0x56] = [this]() { this->lsr_mem(this->addr_zerox(), 6); };
    instruction_table[0x57] = [this]() { this->sre(this->addr_zerox(), 4, 2); };
    instruction_table[0x58] = [this]() { this->cli(); };
    instruction_table[0x59] = [this]() { this->eor(this->load_byte(this->addr_absy()), 4); };
    instruction_table[0x5A] = [this]() { this->nop(2); };
    instruction_table[0x5B] = [this]() { this->sre(this->addr_absy(), 4, 2); };
    instruction_table[0x5C] = [this]() { this->load_byte(this->addr_absx()); this->nop(4); };
    instruction_table[0x5D] = [this]() { this->eor(this->load_byte(this->addr_absx()), 4); };
    instruction_table[0x5E] = [this]() { this->lsr_mem(this->addr_absx(), 7); };
    instruction_table[0x5F] = [this]() { this->sre(this->addr_absx(), 4, 2); };

    instruction_table[0x60] = [this]() { this->rts(); };
    instruction_table[0x61] = [this]() { this->adc(this->load_byte(this->addr_indx()), 6); };
    instruction_table[0x62] = [this]() { this->jam(0x62); };
    instruction_table[0x63] = [this]() { this->rra(this->addr_indx(), 5, 3); };
    instruction_table[0x64] = [this]() { this->load_byte(this->addr_zero()); this->nop(3); };
    instruction_table[0x65] = [this]() { this->adc(this->load_byte(this->addr_zero()), 3); };
    instruction_table[0x66] = [this]() { this->ror_mem(this->addr_zero(), 5); };
    instruction_table[0x67] = [this]() { this->rra(this->addr_zero(), 3, 2); };
    instruction_table[0x68] = [this]() { this->pla(); };
    instruction_table[0x69] = [this]() { this->adc(this->fetch_op(), 2); };
    instruction_table[0x6A] = [this]() { this->ror_a(); };
    instruction_table[0x6B] = [this]() { this->arr(); };
    instruction_table[0x6C] = [this]() { this->jmp_ind(); };
    instruction_table[0x6D] = [this]() { this->adc(this->load_byte(this->addr_abs()), 4); };
    instruction_table[0x6E] = [this]() { this->ror_mem(this->addr_abs(), 6); };
    instruction_table[0x6F] = [this]() { this->rra(this->addr_abs(), 4, 2); };

    instruction_table[0x70] = [this]() { this->bvs(); };
    instruction_table[0x71] = [this]() { this->adc(this->load_byte(this->addr_indy()), 5); };
    instruction_table[0x72] = [this]() { this->jam(0x72); };
    instruction_table[0x73] = [this]() { this->rra(this->addr_indy(), 5, 3); };
    instruction_table[0x74] = [this]() { this->load_byte(this->addr_zerox()); this->nop(4); };
    instruction_table[0x75] = [this]() { this->adc(this->load_byte(this->addr_zerox()), 4); };
    instruction_table[0x76] = [this]() { this->ror_mem(this->addr_zerox(), 6); };
    instruction_table[0x77] = [this]() { this->rra(this->addr_zerox(), 4, 2); };
    instruction_table[0x78] = [this]() { this->sei(); };
    instruction_table[0x79] = [this]() { this->adc(this->load_byte(this->addr_absy()), 4); };
    instruction_table[0x7A] = [this]() { this->nop(2); };
    instruction_table[0x7B] = [this]() { this->rra(this->addr_absy(), 4, 2); };
    instruction_table[0x7C] = [this]() { this->load_byte(this->addr_absx()); this->nop(4); };
    instruction_table[0x7D] = [this]() { this->adc(this->load_byte(this->addr_absx()), 4); };
    instruction_table[0x7E] = [this]() { this->ror_mem(this->addr_absx(), 7); };
    instruction_table[0x7F] = [this]() { this->rra(this->addr_absx(), 4, 2); };

    instruction_table[0x80] = [this]() { this->fetch_op(); this->nop(2); };
    instruction_table[0x81] = [this]() { this->sta(this->addr_indx(), 6); };
    instruction_table[0x82] = [this]() { this->fetch_op(); this->nop(2); };
    instruction_table[0x83] = [this]() { this->sax(this->addr_indx(), 3); };
    instruction_table[0x84] = [this]() { this->sty(this->addr_zero(), 3); };
    instruction_table[0x85] = [this]() { this->sta(this->addr_zero(), 3); };
    instruction_table[0x86] = [this]() { this->stx(this->addr_zero(), 3); };
    instruction_table[0x87] = [this]() { this->sax(this->addr_zero(), 3); };
    instruction_table[0x88] = [this]() { this->dey(); };
    instruction_table[0x89] = [this]() { this->fetch_op(); this->nop(2); };
    instruction_table[0x8A] = [this]() { this->txa(); };
    instruction_table[0x8B] = [this]() { this->tas(this->addr_abs(), 4); };
    instruction_table[0x8C] = [this]() { this->sty(this->addr_abs(), 4); };
    instruction_table[0x8D] = [this]() { this->sta(this->addr_abs(), 4); };
    instruction_table[0x8E] = [this]() { this->stx(this->addr_abs(), 4); };
    instruction_table[0x8F] = [this]() { this->sax(this->addr_abs(), 4); };

    instruction_table[0x90] = [this]() { this->bcc(); };
    instruction_table[0x91] = [this]() { this->sta(this->addr_indy(), 6); };
    instruction_table[0x92] = [this]() { this->jam(0x92); };
    instruction_table[0x93] = [this]() { this->sha(this->addr_indy(), 6); };
    instruction_table[0x94] = [this]() { this->sty(this->addr_zerox(), 4); };
    instruction_table[0x95] = [this]() { this->sta(this->addr_zerox(), 4); };
    instruction_table[0x96] = [this]() { this->stx(this->addr_zeroy(), 4); };
    instruction_table[0x97] = [this]() { this->sax(this->addr_zeroy(), 4); };
    instruction_table[0x98] = [this]() { this->tya(); };
    instruction_table[0x99] = [this]() { this->sta(this->addr_absy(), 5); };
    instruction_table[0x9A] = [this]() { this->txs(); };
    instruction_table[0x9B] = [this]() { this->tas(this->addr_absy(), 5); };
    instruction_table[0x9C] = [this]() { this->shy(this->addr_absx(), 5); };
    instruction_table[0x9D] = [this]() { this->sta(this->addr_absx(), 5); };
    instruction_table[0x9E] = [this]() { this->shx(this->addr_absy(), 5); };
    instruction_table[0x9F] = [this]() { this->sha(this->addr_absy(), 5); };

    instruction_table[0xA0] = [this]() { this->ldy(this->fetch_op(), 2); };
    instruction_table[0xA1] = [this]() { this->lda(this->load_byte(this->addr_indx()), 6); };
    instruction_table[0xA2] = [this]() { this->ldx(this->fetch_op(), 2); };
    instruction_table[0xA3] = [this]() { this->lax(this->load_byte(this->addr_indx()), 6); };
    instruction_table[0xA4] = [this]() { this->ldy(this->load_byte(this->addr_zero()), 3); };
    instruction_table[0xA5] = [this]() { this->lda(this->load_byte(this->addr_zero()), 3); };
    instruction_table[0xA6] = [this]() { this->ldx(this->load_byte(this->addr_zero()), 3); };
    instruction_table[0xA7] = [this]() { this->lax(this->load_byte(this->addr_zero()), 3); };
    instruction_table[0xA8] = [this]() { this->tay(); };
    instruction_table[0xA9] = [this]() { this->lda(this->fetch_op(), 2); };
    instruction_table[0xAA] = [this]() { this->tax(); };
    instruction_table[0xAB] = [this]() { this->lxa(this->fetch_op(), 2); };
    instruction_table[0xAC] = [this]() { this->ldy(this->load_byte(this->addr_abs()), 4); };
    instruction_table[0xAD] = [this]() { this->lda(this->load_byte(this->addr_abs()), 4); };
    instruction_table[0xAE] = [this]() { this->ldx(this->load_byte(this->addr_abs()), 4); };
    instruction_table[0xAF] = [this]() { this->lax(this->load_byte(this->addr_abs()), 4); };

    instruction_table[0xB0] = [this]() { this->bcs(); };
    instruction_table[0xB1] = [this]() { this->lda(this->load_byte(this->addr_indy()), 5); };
    instruction_table[0xB2] = [this]() { this->jam(0xB2); };
    instruction_table[0xB3] = [this]() { this->lax(this->load_byte(this->addr_indy()), 4); };
    instruction_table[0xB4] = [this]() { this->ldy(this->load_byte(this->addr_zerox()), 4); };
    instruction_table[0xB5] = [this]() { this->lda(this->load_byte(this->addr_zerox()), 4); };
    instruction_table[0xB6] = [this]() { this->ldx(this->load_byte(this->addr_zeroy()), 4); };
    instruction_table[0xB7] = [this]() { this->lax(this->load_byte(this->addr_zeroy()), 4); };
    instruction_table[0xB8] = [this]() { this->clv(); };
    instruction_table[0xB9] = [this]() { this->lda(this->load_byte(this->addr_absy()), 4); };
    instruction_table[0xBA] = [this]() { this->tsx(); };
    instruction_table[0xBB] = [this]() { this->las(this->load_byte(this->addr_absy())); };
    instruction_table[0xBC] = [this]() { this->ldy(this->load_byte(this->addr_absx()), 4); };
    instruction_table[0xBD] = [this]() { this->lda(this->load_byte(this->addr_absx()), 4); };
    instruction_table[0xBE] = [this]() { this->ldx(this->load_byte(this->addr_absy()), 4); };
    instruction_table[0xBF] = [this]() { this->lax(this->load_byte(this->addr_absy()), 4); };

    instruction_table[0xC0] = [this]() { this->cpy(this->fetch_op(), 2); };
    instruction_table[0xC1] = [this]() { this->cmp(this->load_byte(this->addr_indx()), 6); };
    instruction_table[0xC2] = [this]() { this->fetch_op(); this->nop(2); };
    instruction_table[0xC3] = [this]() { this->dcp(this->addr_indx(), 5, 3); };
    instruction_table[0xC4] = [this]() { this->cpy(this->load_byte(this->addr_zero()), 3); };
    instruction_table[0xC5] = [this]() { this->cmp(this->load_byte(this->addr_zero()), 3); };
    instruction_table[0xC6] = [this]() { this->dec(this->addr_zero(), 5); };
    instruction_table[0xC7] = [this]() { this->dcp(this->addr_zero(), 3, 2); };
    instruction_table[0xC8] = [this]() { this->iny(); };
    instruction_table[0xC9] = [this]() { this->cmp(this->fetch_op(), 2); };
    instruction_table[0xCA] = [this]() { this->dex(); };
    instruction_table[0xCB] = [this]() { this->sbx(this->fetch_op(), 2); };
    instruction_table[0xCC] = [this]() { this->cpy(this->load_byte(this->addr_abs()), 4); };
    instruction_table[0xCD] = [this]() { this->cmp(this->load_byte(this->addr_abs()), 4); };
    instruction_table[0xCE] = [this]() { this->dec(this->addr_abs(), 6); };
    instruction_table[0xCF] = [this]() { this->dcp(this->addr_abs(), 4, 2); };

    instruction_table[0xD0] = [this]() { this->bne(); };
    instruction_table[0xD1] = [this]() { this->cmp(this->load_byte(this->addr_indy()), 5); };
    instruction_table[0xD2] = [this]() { this->jam(0xD2); };
    instruction_table[0xD3] = [this]() { this->dcp(this->addr_indy(), 5, 3); };
    instruction_table[0xD4] = [this]() { this->load_byte(this->addr_zerox()); this->nop(4); };
    instruction_table[0xD5] = [this]() { this->cmp(this->load_byte(this->addr_zerox()), 4); };
    instruction_table[0xD6] = [this]() { this->dec(this->addr_zerox(), 6); };
    instruction_table[0xD7] = [this]() { this->dcp(this->addr_zerox(), 4, 2); };
    instruction_table[0xD8] = [this]() { this->cld(); };
    instruction_table[0xD9] = [this]() { this->cmp(this->load_byte(this->addr_absy()), 4); };
    instruction_table[0xDA] = [this]() { this->nop(2); };
    instruction_table[0xDB] = [this]() { this->dcp(this->addr_absy(), 4, 2); };
    instruction_table[0xDC] = [this]() { this->load_byte(this->addr_absx()); this->nop(4); };
    instruction_table[0xDD] = [this]() { this->cmp(this->load_byte(this->addr_absx()), 4); };
    instruction_table[0xDE] = [this]() { this->dec(this->addr_absx(), 7); };
    instruction_table[0xDF] = [this]() { this->dcp(this->addr_absx(), 5, 2); };

    instruction_table[0xE0] = [this]() { this->cpx(this->fetch_op(), 2); };
    instruction_table[0xE1] = [this]() { this->sbc(this->load_byte(this->addr_indx()), 6); };
    instruction_table[0xE2] = [this]() { this->fetch_op(); this->nop(2); };
    instruction_table[0xE3] = [this]() { this->isc(this->addr_indx(), 8); };
    instruction_table[0xE4] = [this]() { this->cpx(this->load_byte(this->addr_zero()), 3); };
    instruction_table[0xE5] = [this]() { this->sbc(this->load_byte(this->addr_zero()), 3); };
    instruction_table[0xE6] = [this]() { this->inc(this->addr_zero(), 5); };
    instruction_table[0xE7] = [this]() { this->isc(this->addr_zero(), 5); };
    instruction_table[0xE8] = [this]() { this->inx(); };
    instruction_table[0xE9] = [this]() { this->sbc(this->fetch_op(), 2); };
    instruction_table[0xEA] = [this]() { this->nop(2); };
    instruction_table[0xEB] = [this]() { this->sbc(this->fetch_op(), 2); };
    instruction_table[0xEC] = [this]() { this->cpx(this->load_byte(this->addr_abs()), 4); };
    instruction_table[0xED] = [this]() { this->sbc(this->load_byte(this->addr_abs()), 4); };
    instruction_table[0xEE] = [this]() { this->inc(this->addr_abs(), 6); };
    instruction_table[0xEF] = [this]() { this->isc(this->addr_abs(), 6); };

    instruction_table[0xF0] = [this]() { this->beq(); };
    instruction_table[0xF1] = [this]() { this->sbc(this->load_byte(this->addr_indy()), 5); };
    instruction_table[0xF2] = [this]() { this->jam(0xF2); };
    instruction_table[0xF3] = [this]() { this->isc(this->addr_indy(), 8); };
    instruction_table[0xF4] = [this]() { this->load_byte(this->addr_zerox()); this->nop(4); };
    instruction_table[0xF5] = [this]() { this->sbc(this->load_byte(this->addr_zerox()), 4); };
    instruction_table[0xF6] = [this]() { this->inc(this->addr_zerox(), 6); };
    instruction_table[0xF7] = [this]() { this->isc(this->addr_zerox(), 6); };
    instruction_table[0xF8] = [this]() { this->sed(); };
    instruction_table[0xF9] = [this]() { this->sbc(this->load_byte(this->addr_absy()), 4); };
    instruction_table[0xFA] = [this]() { this->nop(2); };
    instruction_table[0xFB] = [this]() { this->isc(this->addr_absy(), 6); };
    instruction_table[0xFC] = [this]() { this->load_byte(this->addr_absx()); this->nop(4); };
    instruction_table[0xFD] = [this]() { this->sbc(this->load_byte(this->addr_absx()), 4); };
    instruction_table[0xFE] = [this]() { this->inc(this->addr_absx(), 7); };
    instruction_table[0xFF] = [this]() { this->isc(this->addr_absx(), 7); };
  }
  return;
}

/**
 * @brief Execute an opcode by instruction number
 *
 * @param opcode
 */
void mos6510::execute(uint8_t opcode) {
  auto& handler = instruction_table[opcode];

  if (handler) { /* Check if the std::function has a target */
    handler();
  } else { /* Handle unknown opcode (e.g., log an error or throw a custom exception) */
    std::cerr << "Error: Unimplemented opcode " << (int)opcode << std::endl;
  }
  return;
}

CPUCLOCK __us_not_in_flash_func mos6510::cycles(void)
{
  return cycles_;
}

void __us_not_in_flash_func mos6510::cycles(CPUCLOCK v)
{
  cycles_=v;
  return;
}


/**
 * @brief Sets the callback function at cycle ticks
 *
 * @param c
 */
void mos6510::set_cycle_callback(ClockCycle c)
{
  clock_cycle = (ClockCycle)c;
  return;
}

/**
 * @brief Performs a cold reset
 *
 * https://www.c64-wiki.com/index.php/Reset_(Process)
 */
void mos6510::reset()
{
  a_ = x_ = y_ = 0;
  sp_ = 0xFD;
  _flags = 0b00110000;
  val_t pc_lo = load_byte(pAddrResetVector);
  val_t pc_hi = load_byte(pAddrResetVector+1);
  pc(pc_lo | pc_hi << 8);
  prev_cycles_ = 0, cycles_ = 6;
}

/**
 * @brief emulate a single instruction
 * @return returns false if something goes wrong
 *
 * Current limitations:
 * - Some known cpu bugs are not emulated (correctly)
 */
bool mos6510::emulate(void)
{
  bool retval = true;

  /* NOTICE: Handling interrupts _before_ the opcode breaks stuff */

  /* VICII stalled the CPU for sprite handling, do nothing */
  // if (vic_stall_cpu_) { tick(1); return retval; };

  /* fetch instruction */
  val_t insn = fetch_op();
  pb_crossed = false;

  /* execute instruction */
  execute(insn);

  if (loginstructions) { dump_regs_insn(insn); }

  /* Save state */
  last_insn = insn;
  /* Reset var */
  pb_crossed = false;

  /* NOTICE: Handling interrupts after the opcode works much better, but reaks everything else though */

  return retval;
}

/**
 * @brief emulate N cpu instructions
 * @brief Or emulate until a RTI occurs
 * @param uint32_t n_cycles ~ supply 0 to emulate
 *        until RTI
 * @return returns false if something goes wrong
 */
bool mos6510::emulate_n(tick_t n_cycles)
{
  bool retval = true;

  CPUCLOCK end = (cycles() + n_cycles);

  for (;;) {
    retval = emulate();

    if (n_cycles == 0) {
      if (last_insn == 0x40) {
        return retval;
      }
    } else {
      if (cycles() >= end) {
        return retval;
      }
    }
  }
  return retval;
}

/**
 * @brief Wrapper around write_bus pointer function
 *
 * @param addr
 * @param val
 */
void mos6510::save_byte(addr_t addr, val_t val)
{
  d_address = addr;
  write_bus(addr,val);
}

val_t mos6510::load_byte(addr_t addr)
{
  d_address = addr;
  return read_bus(addr);
}

addr_t mos6510::load_word(addr_t addr)
{
  addr_t v = load_byte(addr) | (load_byte(addr+1) << 8);
  d_address = addr;
  return v;
}

void mos6510::push(val_t v)
{
  addr_t addr = pBaseAddrStack+sp_;
  save_byte(addr,v);
  sp_--;
}

val_t mos6510::pop()
{
  addr_t addr = ++sp_+pBaseAddrStack;
  return load_byte(addr);
}

val_t mos6510::fetch_op()
{
  pc_address = pc_;
  uint_least8_t op = load_byte(pc_++);
  return op;
}

addr_t mos6510::fetch_opw()
{
  addr_t retval = load_word(pc_);
  pc_+=2;
  pc_address = pc_;
  return retval;
}

addr_t mos6510::addr_zero()
{
  addr_t addr = fetch_op();
  return addr;
}

addr_t mos6510::addr_zerox()
{
  /* wraps around the zeropage */
  addr_t addr = (fetch_op() + x()) & 0xff;
  return addr;
}

addr_t mos6510::addr_zeroy()
{
  /* wraps around the zeropage */
  addr_t addr = (fetch_op() + y()) & 0xff;

  return addr;
}

addr_t mos6510::addr_abs()
{
  addr_t addr = fetch_opw();
  return addr;
}

addr_t mos6510::addr_absy()
{
  addr_t addr = fetch_opw();
  curr_page = addr&0xff00;
  addr += y();
  if ((addr&0xff00)>curr_page) pb_crossed = true;

  return addr;
}

addr_t mos6510::addr_absx()
{
  addr_t addr = fetch_opw();
  curr_page = addr&0xff00;
  addr += x();
  if ((addr&0xff00)>curr_page) pb_crossed = true;

  return addr;
}

addr_t mos6510::addr_indx()
{
  /* wraps around the zeropage */
  addr_t addr = load_word((addr_zero() + x()) & 0xff);
  return addr;
}

addr_t mos6510::addr_indy()
{
  addr_t addr = load_word(addr_zero());
  curr_page = addr&0xff00;
  addr += y();
  if ((addr&0xff00)>curr_page) pb_crossed = true;

  return addr;
}

/* Data handling and memory operations */

/**
 * @brief STore Accumulator
 */
void mos6510::sta(addr_t addr, cycle_t cycles)
{
  save_byte(addr,a());
  tick(cycles);
}

/**
 * @brief STore X
 */
void mos6510::stx(addr_t addr, cycle_t cycles)
{
  save_byte(addr,x());
  tick(cycles);
}

/**
 * @brief STore Y
 */
void mos6510::sty(addr_t addr, cycle_t cycles)
{
  save_byte(addr,y());
  tick(cycles);
}

/**
 * @brief Transfer X to Stack pointer
 */
void mos6510::txs()
{
  sp(x());
  tick(2);
}

/**
 * @brief Transfer Stack pointer to X
 */
void mos6510::tsx()
{
  x(sp());
  SET_ZF(x());
  SET_NF(x());
  tick(2);
}

/**
 * @brief LoaD Accumulator
 */
void mos6510::lda(val_t v, cycle_t cycles)
{
  a(v);
  // SET_ZF(a());
  zf(!a());
  // SET_NF(a());
  nf((a()&0x80)!=0);
  if(pb_crossed)cycles+=1;
  tick(cycles);
}

/**
 * @brief LoaD X
 */
void mos6510::ldx(val_t v, cycle_t cycles)
{
  x(v);
  SET_ZF(x());
  SET_NF(x());
  if(pb_crossed)cycles+=1;
  tick(cycles);
}

/**
 * @brief LoaD Y
 */
void mos6510::ldy(val_t v, cycle_t cycles)
{
  y(v);
  SET_ZF(y());
  SET_NF(y());
  if(pb_crossed)cycles+=1;
  tick(cycles);
}

/**
 * @brief Transfer X to Accumulator
 */
void mos6510::txa()
{
  a(x());
  SET_ZF(a());
  SET_NF(a());
  tick(2);
}

/**
 * @brief Transfer Accumulator to X
 */
void mos6510::tax()
{
  x(a());
  SET_ZF(x());
  SET_NF(x());
  tick(2);
}

/**
 * @brief Transfer Accumulator to Y
 */
void mos6510::tay()
{
  y(a());
  SET_ZF(y());
  SET_NF(y());
  tick(2);
}

/**
 * @brief Transfer Y to Accumulator
 */
void mos6510::tya()
{
  a(y());
  SET_ZF(a());
  SET_NF(a());
  tick(2);
}

/**
 * @brief PusH Accumulator
 */
void mos6510::pha()
{
  push(a());
  tick(3);
}

/**
 * @brief PuLl Accumulator
 */
void mos6510::pla()
{
  a(pop());
  SET_ZF(a());
  SET_NF(a());
  tick(4);
}

/* Logic operations */

/**
 * @brief Logical OR on Accumulator
 */
void mos6510::ora(val_t v, cycle_t cycles)
{
  a(a()|v);
  SET_ZF(a());
  SET_NF(a());
  if(pb_crossed)cycles+=1;
  tick(cycles);
}

/**
 * @brief Logical AND
 */
void mos6510::_and(val_t v, cycle_t cycles)
{
  a(a()&v);
  SET_ZF(a());
  SET_NF(a());
  if(pb_crossed)cycles+=1;
  tick(cycles);
}

/**
 * @brief BIT test
 */
void mos6510::bit(addr_t addr, cycle_t cycles)
{
  val_t t = load_byte(addr);
  SET_NF(t);
  SET_OF(t);
  SET_ZF(t&a());
  tick(cycles);
}

/**
 * @brief ROtate Left
 */
val_t mos6510::rol(val_t v)
{
  addr_t t = (v << 1) | (val_t)cf();
  cf((t&0x100)!=0);
  // SET_CF(t); // BUG: Not working yet :-)
  SET_ZF(t);
  SET_NF(t);
  return (val_t)t;
}

/**
 * @brief ROL A register
 */
void mos6510::rol_a()
{
  a(rol(a()));
  tick(2);
}

/**
 * @brief ROL mem
 */
void mos6510::rol_mem(addr_t addr, cycle_t cycles)
{
  val_t v = load_byte(addr);
  /* see ASL doc */
  save_byte(addr,v);
  save_byte(addr,rol(v));
  tick(cycles);
}

/**
 * @brief ROtate Right
 */
val_t mos6510::ror(val_t v)
{
  addr_t t = (v >> 1) | (val_t)(cf() << 7);
  cf((v&0x1)!=0);
  SET_ZF(t);
  SET_NF(t);
  return (val_t)t;
}

/**
 * @brief ROR A register
 */
void mos6510::ror_a()
{
  a(ror(a()));
  tick(2);
}

/**
 * @brief ROR mem
 */
void mos6510::ror_mem(addr_t addr, cycle_t cycles)
{
  val_t v = load_byte(addr);
  /* see ASL doc */
  save_byte(addr,v);
  save_byte(addr,ror(v));
  tick(cycles);
}

/**
 * @brief Logic Shift Right
 */
val_t mos6510::lsr(val_t v)
{
  val_t t = v >> 1;
  cf((v&0x1)!=0);
  SET_ZF(t);
  SET_NF(t);
  return t;
}

/**
 * @brief LSR A
 */
void mos6510::lsr_a()
{
  a(lsr(a()));
  tick(2);
}

/**
 * @brief LSR mem
 */
void mos6510::lsr_mem(addr_t addr, cycle_t cycles)
{
  val_t v = load_byte(addr);
  /* see ASL doc */
  save_byte(addr,v);
  save_byte(addr,lsr(v));
  tick(cycles);
}

/**
 * @brief Arithmetic Shift Left
 */
val_t mos6510::asl(val_t v)
{
  val_t t = (v << 1) & 0xff;
  cf((v&0x80)!=0);
  SET_ZF(t);
  SET_NF(t);
  return t;
}

/**
 * @brief ASL A
 */
void mos6510::asl_a()
{
  a(asl(a()));
  tick(2);
}

/**
 * @brief ASL mem
 *
 * ASL and the other read-modify-write instructions contain a bug (wikipedia):
 *
 * --
 * The 6502's read-modify-write instructions perform one read and two write
 * cycles. First the unmodified data that was read is written back,and then
 * the modified data is written. This characteristic may cause issues by
 * twice accessing hardware that acts on a write. This anomaly continued
 * through the entire NMOS line,but was fixed in the CMOS derivatives,in
 * which the processor will do two reads and one write cycle.
 * --
 *
 * I have come across code that uses this side-effect as a feature,for
 * instance,the following instruction will acknowledge VIC interrupts
 * on the first write cycle:
 *
 * ASL $d019
 *
 * So.. we need to mimic the behaviour.
 */
void mos6510::asl_mem(addr_t addr, cycle_t cycles)
{
  val_t v = load_byte(addr);
  save_byte(addr,v);
  save_byte(addr,asl(v));
  tick(cycles);
}

/**
 * @brief Exclusive OR
 */
void mos6510::eor(val_t v, cycle_t cycles)
{
  a(a()^v);
  SET_ZF(a());
  SET_NF(a());
  if(pb_crossed)cycles+=1;
  tick(cycles);
}

/* Arithmetic operations */

/**
 * @brief INCrement
 */
void mos6510::inc(addr_t addr, cycle_t cycles)
{
  val_t v = load_byte(addr);
  /* see ASL doc */
  save_byte(addr,v);
  v++;
  save_byte(addr,v);
  SET_ZF(v);
  SET_NF(v);
  tick(cycles);
}

/**
 * @brief DECrement
 */
void mos6510::dec(addr_t addr, cycle_t cycles)
{
  val_t v = load_byte(addr);
  /* see ASL doc */
  save_byte(addr,v);
  v--;
  save_byte(addr,v);
  SET_ZF(v);
  SET_NF(v);
  tick(cycles);  // was missing
}

/**
 * @brief INcrement X
 */
void mos6510::inx()
{
  x_+=1;
  SET_ZF(x());
  SET_NF(x());
  tick(2);
}

/**
 * @brief INcrement Y
 */
void mos6510::iny()
{
  // y_+=1;
  y(y()+1);
  SET_ZF(y());
  SET_NF(y());
  tick(2);
}

/**
 * @brief DEcrement X
 */
void mos6510::dex()
{
  x_-=1;
  SET_ZF(x());
  SET_NF(x());
  tick(2);
}

/**
 * @brief DEcrement Y
 */
void mos6510::dey()
{
  y_-=1;
  // y(y()-1);
  SET_ZF(y());
  SET_NF(y());
  tick(2);
}

/**
 * @brief ADd with Carry
 */
void mos6510::adc(val_t v, cycle_t cycles)
{
  addr_t t;
  if(dmf())
  {
    t = (a()&0xf) + (v&0xf) + (cf() ? 1 : 0);
    if (t > 0x09)
      t += 0x6;
    t += (a()&0xf0) + (v&0xf0);
    if((t & 0x1f0) > 0x90)
      t += 0x60;
  }
  else
  {
    t = a() + v + (cf() ? 1 : 0);
  }
  cf(t>0xff);
  t=t&0xff;
  of(!((a()^v)&0x80) && ((a()^t) & 0x80));
  SET_ZF(t);
  SET_NF(t);
  a((val_t)t);
  if(pb_crossed)cycles+=1;
  tick(cycles);
}

/**
 * @brief SuBstract with Carry
 */
void mos6510::sbc(val_t v, cycle_t cycles)
{
  addr_t t;
  if(dmf())
  {
    t = (a()&0xf) - (v&0xf) - (cf() ? 0 : 1);
    if((t & 0x10) != 0)
      t = ((t-0x6)&0xf) | ((a()&0xf0) - (v&0xf0) - 0x10);
    else
      t = (t&0xf) | ((a()&0xf0) - (v&0xf0));
    if((t&0x100)!=0)
      t -= 0x60;
  }
  else
  {
    t = a() - v - (cf() ? 0 : 1);
  }
  cf(t<0x100);
  t=t&0xff;
  of(((a()^t)&0x80) && ((a()^v) & 0x80));
  SET_ZF(t);
  SET_NF(t);
  a((val_t)t);
  if(pb_crossed)cycles+=1;
  tick(cycles);
}

/* Flag access */

/**
 * @brief SEt Interrupt flag
 */
void mos6510::sei()
{
  idf(true);
  tick(2);
}

/**
 * @brief CLear Interrupt flag
 */
void mos6510::cli()
{
  idf(false);
  tick(2);
}

/**
 * @brief SEt Carry flag
 */
void mos6510::sec()
{
  cf(true);
  tick(2);
}

/**
 * @brief CLear Carry flag
 */
void mos6510::clc()
{
  cf(false);
  tick(2);
}

/**
 * @brief SEt Decimal flag
 */
void mos6510::sed()
{
  dmf(true);
  tick(2);
}

/**
 * @brief CLear Decimal flag
 */
void mos6510::cld()
{
  dmf(false);
  tick(2);
}

/**
 * @brief CLear oVerflow flag
 */
void mos6510::clv()
{
  of(false);
  tick(2);
}

// val_t mos6510::flags()
// {
//   val_t v=0;
//   v |= cf()  << 0;
//   v |= zf()  << 1;
//   v |= idf() << 2;
//   v |= dmf() << 3;
//   /* brk & php instructions push the bcf flag active */
//   /* v |= 1 << 4; */
//   /* unused,always set */
//   v |= 1     << 5;
//   v |= of()  << 6;
//   v |= nf()  << 7;
//   // MOSDBG("[FLAGS]() %X\n",v);
//   return v;
// }

val_t mos6510::flags()
{
  /* only brk & php instructions push the bcf flag active */
  /* bit 5 is unused and always set */
  return (_flags | (1 << 5));
}

// void mos6510::flags(val_t v)
// {
//   cf(ISSET_BIT(v,0));
//   zf(ISSET_BIT(v,1));
//   idf(ISSET_BIT(v,2));
//   dmf(ISSET_BIT(v,3));
//   of(ISSET_BIT(v,6));
//   nf(ISSET_BIT(v,7));
//   // MOSDBG("[FLAGS](v) %X\n",v);
// }

void mos6510::flags(val_t v)
{
  cf(v&0x1);
  zf(v&0x2);
  idf(v&0x4);
  dmf(v&0x8);
  of(v&0x40);
  nf(v&0x80);
  // MOSDBG("[FLAGS](v) %X\n",v);
}

/**
 * @brief PusH Processor flags
 */
void mos6510::php()
{
  push(flags()|0x10);  /* brk & php instructions push the bcf flag active */
  // push(flags());  /* brk & php instructions push the bcf flag active */
  tick(3);
}

/**
 * @brief PuLl Processor flags
 */
void mos6510::plp()
{
  flags(pop());
  tick(4);
}

/**
 * @brief Jump to SubRoutine
 *
 * Note that JSR does not push the address of the next instruction
 * to the stack but the address to the last byte of its own
 * instruction.
 */
void mos6510::jsr()
{
  addr_t addr = addr_abs();
  push(((pc()-1) >> 8) & 0xff);
  push(((pc()-1) & 0xff));
  pc(addr);
  tick(6);
}

/**
 * @brief JuMP
 */
void mos6510::jmp()
{
  addr_t addr = addr_abs();
  pc(addr);
  tick(3);
}

/**
 * @brief JuMP (indirect)
 */
void mos6510::jmp_ind()
{
  addr_t t = load_word(pc_);
  addr_t abs_ = addr_abs(); /* pc += 2 */
  addr_t addr = load_word(abs_);
  /* Introduce indirect JMP bug */
  addr = (((t&0xFF)==0xFF)?((t&0xFF00)|(addr&0xFF)):addr);
  pc(addr);
  // tick(3); // incorrect
  tick(5);
}

/**
 * @brief ReTurn from SubRoutine
 */
void mos6510::rts()
{
  // addr_t addr = (pop() + (pop() << 8)) + 1;
  // pc(addr);
  val_t lo, hi;
  lo = pop();
	hi = pop();

	pc(((hi << 8) | lo) + 1);
  tick(6);
}

/**
 * @brief CoMPare
 */
void mos6510::cmp(val_t v, cycle_t cycles)
{
  addr_t t;
  t = a() - v;
  cf(t<0x100);
  t = t&0xff;
  SET_ZF(t);
  SET_NF(t);
  if(pb_crossed)cycles+=1;
  tick(cycles);
}

/**
 * @brief CoMPare X
 */
void mos6510::cpx(val_t v, cycle_t cycles)
{
  addr_t t;
  t = x() - v;
  cf(t<0x100);
  t = t&0xff;
  SET_ZF(t);
  SET_NF(t);
  tick(cycles);
}

/**
 * @brief CoMPare Y
 */
void mos6510::cpy(val_t v, cycle_t cycles)
{
  addr_t t;
  t = y() - v;
  cf(t<0x100);
  t = t&0xff;
  SET_ZF(t);
  SET_NF(t);
  tick(cycles);
}

/**
 * @brief Branch if Not Equal
 */
void mos6510::bne()
{
  addr_t addr = (int8_t) fetch_op() + pc();
  // if(!zf()) {
  if(!GETFLAG(SR_ZERO)) {
    pc(addr);
    if(pb_crossed) tick(2);
    else tick(1);
  }
  tick(2);
}

/**
 * @brief Branch if Equal
 */
void mos6510::beq()
{
  addr_t addr = (int8_t) fetch_op();
  curr_page = addr&0xff00;
  addr += pc();
  if ((addr&0xff00)>curr_page) pb_crossed = true;
  if(zf()) {
    pc(addr);
    if(pb_crossed) tick(2);
    else tick(1);
  }
  tick(2);
}

/**
 * @brief Branch if Carry is Set
 */
void mos6510::bcs()
{
  addr_t addr = (int8_t) fetch_op();
  curr_page = addr&0xff00;
  addr += pc();
  if ((addr&0xff00)>curr_page) pb_crossed = true;
  if(cf()) {
    pc(addr);
    if(pb_crossed) tick(2);
    else tick(1);
  }
  tick(2);
}

/**
 * @brief Branch if Carry is Clear
 */
void mos6510::bcc()
{
  addr_t addr = (int8_t) fetch_op();
  curr_page = addr&0xff00;
  addr += pc();
  if ((addr&0xff00)>curr_page) pb_crossed = true;
  if(!cf()) {
    pc(addr);
    if(pb_crossed) tick(2);
    else tick(1);
  }
  tick(2);
}

/**
 * @brief,Branch if PLus
 */
void mos6510::bpl()
{
  addr_t addr = (int8_t) fetch_op();
  curr_page = addr&0xff00;
  addr += pc();
  if ((addr&0xff00)>curr_page) pb_crossed = true;
  if(!nf()) {
    pc(addr);
    if(pb_crossed) tick(2);
    else tick(1);
  }
  tick(2);
}

/**
 * @brief Branch if MInus
 */
void mos6510::bmi()
{
  addr_t addr = (int8_t) fetch_op();
  curr_page = addr&0xff00;
  addr += pc();
  if ((addr&0xff00)>curr_page) pb_crossed = true;
  if(nf()) {
    pc(addr);
    if(pb_crossed) tick(2);
    else tick(1);
  }
  tick(2);
}

/**
 * @brief Branch if oVerflow Clear
 */
void mos6510::bvc()
{
  addr_t addr = (int8_t) fetch_op();
  curr_page = addr&0xff00;
  addr += pc();
  if ((addr&0xff00)>curr_page) pb_crossed = true;
  if(!of()) {
    pc(addr);
    if(pb_crossed) tick(2);
    else tick(1);
  }
  tick(2);
}

/**
 * @brief Branch if oVerflow Set
 */
void mos6510::bvs()
{
  addr_t addr = (int8_t) fetch_op();
  curr_page = addr&0xff00;
  addr += pc();
  if ((addr&0xff00)>curr_page) pb_crossed = true;
  if(of()) {
    pc(addr);
    if(pb_crossed) tick(2);
    else tick(1);
  }
  tick(2);
}

/* Illegal instructions */

/**
 * @brief JAM / KILL / HALT
 *
 * @param insn
 */
void mos6510::jam(val_t insn)
{
  (void)insn;
  // if (1) return;
  // MOSDBG("JAM! INS: %02X PC @ %04X\nInstr %02X @ %04X\nNext Instr %02X @ %04X\nPrev Instr %02X @ %04X\nPrev Instr %02X @ %04X\nSP:%X A:%X X:%X Y:%X\n",
    // insn,pc(),
    // load_byte(pc_-1),pc_,load_byte(pc_),pc_+1,
    // load_byte(pc_-2),pc_-1,load_byte(pc_-3),pc_-2,
    // sp(),a(),x(),y()
    // );
  // dump_regs();
  // reset();
  // exit(1);
  // pc(pc_--);
  tick(1);
}

/**
 * @brief SLO (ASO) ASL oper + ORA oper
 *
 * @param addr
 * @param cycles_a
 * @param cycles_b
 */
void mos6510::slo(addr_t addr, cycle_t cycles_a,cycle_t cycles_b)
{
  asl_mem(addr,cycles_a);
  ora(load_byte(addr),cycles_b);
}

void mos6510::lxa(val_t v, cycle_t cycles)
{
  val_t t = ((a() | 0xEE) & v);
  x(t);
  a(t);
  SET_ZF(t);
  SET_NF(t);
  tick(cycles);
}

void mos6510::anc(val_t v)
{
  _and(v,2);
  if(nf()) {
    cf(true);
  } else {
    cf(false);
  }
}

void mos6510::las(val_t v)
{ /* 4 + 1 cycles if page boundry is crossed */
  val_t t = (v & sp());
  a(t);
  x(t);
  sp(t);
  SET_NF(t);
  SET_ZF(t);
  tick(4);
  if(pb_crossed) tick(1);
}

void mos6510::lax(val_t v, cycle_t cycles)
{
  lda(v,cycles);
  tax();
}

void mos6510::sax(addr_t addr, cycle_t cycles)
{
  val_t _a = a();
  val_t _x = x();
  val_t _r = (_a & _x);
  save_byte(addr,_r);
  tick(cycles);
}

void mos6510::shy(addr_t addr, cycle_t cycles)
{
  val_t t = ((addr >> 8) + 1);
  val_t y_ = y();
  save_byte(addr,(y_ & t));
  tick(cycles);
}

void mos6510::shx(addr_t addr, cycle_t cycles)
{
  val_t t = ((addr >> 8) + 1);
  val_t x_ = x();
  save_byte(addr,(x_ & t));
  tick(cycles);
}

void mos6510::sha(addr_t addr, cycle_t cycles)
{
  val_t t = ((addr >> 8) + 1);
  val_t a_ = a();
  val_t x_ = x();
  save_byte(addr,((a_ & x_) & t));
  tick(cycles);
}

void mos6510::sre(addr_t addr, cycle_t cycles_a,cycle_t cycles_b)
{
  addr_t t = addr;
  lsr_mem(t,cycles_a);
  eor(load_byte(t),cycles_b);
}

void mos6510::rla(addr_t addr, cycle_t cycles_a,cycle_t cycles_b)
{
  addr_t t = addr;
  rol_mem(t,cycles_a);
  _and(load_byte(t),cycles_b);
}

void mos6510::rla_(addr_t addr, cycle_t cycles_a,cycle_t cycles_b)
{
  // addr_t t = addr;
  // rol_mem(t,cycles_a);
  // _and(load_byte(t),cycles_b);

  // ROL_MEM
  val_t v = load_byte(addr);
  /* see ASL doc */
  save_byte(addr,v);
  // ROL
  // save_byte(addr,rol(v));
  addr_t t = (v << 1) | (val_t)cf();
  cf((t&0x100)!=0);
  SET_ZF(t);
  SET_NF(t);
  // return (val_t)t;
  save_byte(addr,(val_t)t);
  tick(cycles_a);

  // AND
  v = load_byte(addr);
  a(a()&v);
  SET_ZF(a());
  SET_NF(a());
  if(pb_crossed)cycles_b+=1;
  tick(cycles_b);
}

void mos6510::rra(addr_t addr, cycle_t cycles_a,cycle_t cycles_b)
{
  addr_t t = addr;
  ror_mem(t,cycles_a);
  adc(load_byte(t),cycles_b);
}

void mos6510::dcp(addr_t addr, cycle_t cycles_a,cycle_t cycles_b)
{
  addr_t t = addr;
  dec(t,cycles_a);
  cmp(load_byte(t),cycles_b);
}

void mos6510::tas(addr_t addr, cycle_t cycles)
{
  /* and accu, x and (highbyte + 1) of address */
  val_t v = (((a() & x()) & ((addr >> 8) + 1)));

  unsigned int tmp2 = (addr + y());
  if (((addr & 0xff) + y()) > 0xff) {
    tmp2 = ((tmp2 & 0xff) | (v << 8));
    /* write result to address */
    save_byte(tmp2, v);
  } else {
    /* write result to address */
    save_byte(addr, v);
  }
  sp(a()&x()); /* write a & x to stackpointer unchanged */
  tick(cycles);
}

void mos6510::sbx(val_t v, cycle_t cycles)
{
  val_t a_ = a();
  val_t x_ = x();
  val_t r_ = a_ & x_;
  addr_t t = r_ - v;
  cf(t<0x100);
  t = t&0xff;
  SET_ZF(t);
  SET_NF(t);
  x(t);
  tick(cycles);
}

void mos6510::isc(addr_t addr, cycle_t cycles)
{
  inc(addr,cycles-=2);
  val_t v = load_byte(addr);
  sbc(v,cycles);
}

void mos6510::arr()
{ /* Fixed code with courtesy of Vice 6510core.c */
  unsigned int tmp = (a() & (fetch_op()));
  if(dmf()) {
    int tmp2 = tmp;
    tmp2 |= ((flags() & SR_CARRY) << 8);
    tmp2 >>= 1;
    // compiler integer in boolean situation warning/error
    // nf(((flags() & SR_CARRY)?0x80:0)); /* set signed on negative flag */
    if (flags() & SR_CARRY) {
      nf(0x80); /* set signed on negative flag */
    } else {
      nf(0); /* unset signed on negative flag */
    }
    SET_ZF(tmp2);
    of((tmp2 ^ tmp) & 0x40);
    if (((tmp & 0xf) + (tmp & 0x1)) > 0x5) {
      tmp2 = (tmp2 & 0xf0) | ((tmp2 + 0x6) & 0xf);
    }
    if (((tmp & 0xf0) + (tmp & 0x10)) > 0x50) {
      tmp2 = (tmp2 & 0x0f) | ((tmp2 + 0x60) & 0xf0);
      cf(true);
    } else {
      cf(false);
    }
    a(tmp2);
  } else {
    tmp |= ((flags() & SR_CARRY) << 8);
    tmp >>= 1;
    SET_ZF(tmp);
    SET_NF(tmp);
    cf((tmp & 0x40));
    of((tmp & 0x40) ^ ((tmp & 0x20) << 1));
    a(tmp);
  }
  tick(2);
}

void mos6510::xaa(val_t v)
{
  val_t t = ((a() | ANE_MAGIC) & x() & ((val_t)(v)));
  a(t);
  SET_ZF(t);
  SET_NF(t);
  tick(2);
}

/* Miscellaneous */

/**
 * @brief No OPeration
 */
void mos6510::nop(cycle_t cycles)
{
  if(pb_crossed)cycles+=1;
  tick(cycles);
}

/* Clock logic */

/* BUG: DOESN'T WORK AS INTENDED */
void mos6510::set_irq_nmi_callback(CPUCLOCK c, int src)
{
  switch (src) {
    case 0:
      vic_rstr_irq_callback = c;
      break;
    case 1:
      cia1_tima_irq_callback = c;
      break;
    case 2:
      cia1_timb_irq_callback = c;
      break;
    case 3:
      cia2_tima_nmi_callback = c;
      break;
    case 4:
      cia2_timb_nmi_callback = c;
      break;
  }
  // MOSDBG("[CPU] Set callback @ %u for %d\n",c,src);
  return;
}

void mos6510::tick_backup(cycle_t v)
{
  do {
    cycles_++;
    v--;
    if _MOS_UNLIKELY(check_callback()) { clock_cycle(this); } /* Poor mans callback */
  } while (v != 0);
}

void mos6510::tick(cycle_t v)
{
  do {
    cycles_++;
    v--;
    if _MOS_UNLIKELY(check_callback()) { clock_cycle(this); } /* Poor mans callback */
  } while (v != 0);
}

void mos6510::backtick(cycle_t v)
{
  cycles_ -= v;
}

void mos6510::tickle_me(cycle_t v)
{
  tick(v);
}

/* Interrupt logic */

/**
 * @brief ReTurn from Interrupt
 */
void mos6510::rti()
{
  flags(pop());
  pc(pop() + (pop() << 8));
  // tick(7); /* 7?? */
  tick(6);
}

/**
 * @brief Interrupt ReQuest and Non Maskable Interrupt handler
 */
void mos6510::handle_interrupts(void) /* BUG: Makes play way too fast */
{
  // if (loginstructions) {dump_regs_irq(0, (mos6510::nmi_pending ? 1 : 3));}
  if (mos6510::nmi_pending || (mos6510::irq_pending && !idf())) {
    /* MOSDBG("[handle_interrupts] NMI:%d IRQ:%d IDF:%d\n",mos6510::nmi_pending, mos6510::irq_pending, idf()); */
    tick(2);
    push(((pc()) >> 8) & 0xff);
    push(((pc()) & 0xff));
    tick(2);
    push((flags() & 0xef));  /* push flags with bcf cleared */
    tick(1);
    // TODO: Add check for pending alarms here
    if (mos6510::nmi_pending) { /* NMI takes precedence over an IRQ */
      mos6510::nmi_pending = false;
      // if (!mos6510::irq_pending) { mos6510::pending_interrupt = false; };
      // mos6510::pending_interrupt = false;
      // idf(true);
      pc(load_word(pAddrNMIVector));
    } else if (mos6510::irq_pending && !idf()){
      mos6510::irq_pending = false;
      // if (!mos6510::nmi_pending) { mos6510::pending_interrupt = false; };
      // mos6510::pending_interrupt = false;
      idf(true);
      pc(load_word(pAddrIRQVector));
    }
    tick(1);
  }// else { mos6510::irq_pending = false; }; // TODO: TEST THIS!
}
void mos6510::nmi_(val_t source)
{
  // if (loginstructions) { dump_regs_irq(1, source); }
  mos6510::nmi_pending = mos6510::pending_interrupt = true;
}

void mos6510::irq_(val_t source)
{
  // if (loginstructions && !idf()) { dump_regs_irq(0, source); }
  mos6510::irq_pending = mos6510::pending_interrupt = true;
}

void mos6510::process_interrupts(void)
{
  handle_interrupts();
}

/**
 * @brief BReaKpoint
 */
void mos6510::brk()
{ /* ISSUE: BRK BUG DOES NOT WORK YET */
  bcf(true);
  push(((pc()+1) >> 8) & 0xff);
  push(((pc()+1) & 0xff));
  tick(4);
  push(flags()|0x10);  /* brk & php instructions push the bcf flag active */
  // push(flags());  /* brk & php instructions push the bcf flag active */
  tick(1);
  idf(true);
  pc(load_word(pAddrIRQVector));
  pc_address = pc_;
  // TODO: Add some kind of alarms processing here
  // if (mos6510::nmi_pending) {
  //   idf(true);
  //   pc(load_word(Memory::pAddrNMIVector));
  //   mos6510::nmi_pending = false;
  //   // mos6510::pending_interrupt = false;
  //   // if (!mos6510::irq_pending/) { mos6510::pending_interrupt = false; };
  // } else if (mos6510::irq_pending && !idf()) {
  //   idf(true);
  //   pc(load_word(Memory::pAddrIRQVector));
  //   mos6510::irq_pending = false;
  //   // mos6510::pending_interrupt = false;
  //   if (!mos6510::nmi_pending) { mos6510::pending_interrupt = false; };
  // } else {
  //   idf(true);
  //   pc(load_word(Memory::pAddrIRQVector));
  // }
  tick(2);
}

/**
 * @brief Interrupt ReQuest
 * @deprecated
 */
void mos6510::irq(uint_least8_t source)
{
  if(!idf())
  {
    if (loginstructions) {dump_regs_irq(0, source); }
    else { (void)source; }
    tick(2);
    push(((pc()) >> 8) & 0xff);
    push(((pc()) & 0xff));
    tick(2);
    /* push flags with bcf cleared */
    push((flags() & 0xef));
    tick(1);
    pc(load_word(pAddrIRQVector));
    idf(true);
    tick(2);
    pc_address = pc_;
    // tick(7);
  }
}

/**
 * @brief Non Maskable Interrupt
 * @deprecated
 */
void mos6510::nmi(uint_least8_t source)
{
  if (loginstructions) {dump_regs_irq(1, source); }
  else { (void)source; }
  tick(2);
  push(((pc()) >> 8) & 0xff);
  push(((pc()) & 0xff));
  tick(2);
  /* push flags with bcf cleared */
  tick(1);
  push((flags() & 0xef));
  pc(load_word(pAddrNMIVector));
  tick(2);
  pc_address = pc_;
  // tick(7);
}

/* Debug logging */

void mos6510::dump_flags()
{
  MOSDBG("FLAGS: %02X %d%d%d%d%d%d%d%d\n",
    flags(),
    (flags()&SR_NEGATIVE)>>7,
    (flags()&SR_OVERFLOW)>>6,
    (flags()&SR_UNUSED)>>5,
    (flags()&SR_BREAK)>>4,
    (flags()&SR_DECIMAL)>>3,
    (flags()&SR_INTERRUPT)>>2,
    (flags()&SR_ZERO)>>1,
    (flags()&SR_CARRY)
  );
}

void mos6510::dump_flags(val_t flags)
{
  MOSDBG("FLAGS: %02X %d%d%d%d%d%d%d%d\n",
    flags,
    (flags&SR_NEGATIVE)>>7,
    (flags&SR_OVERFLOW)>>6,
    (flags&SR_UNUSED)>>5,
    (flags&SR_BREAK)>>4,
    (flags&SR_DECIMAL)>>3,
    (flags&SR_INTERRUPT)>>2,
    (flags&SR_ZERO)>>1,
    (flags&SR_CARRY)
  );
}

void mos6510::dump_regs()
{
  std::stringstream sflags;
  if(nf())  sflags << "NF ";
  if(of())  sflags << "OF ";
  if(bcf()) sflags << "BCF ";
  if(dmf()) sflags << "DMF ";
  if(idf()) sflags << "IDF ";
  if(zf())  sflags << "ZF ";
  if(cf())  sflags << "CF ";

  // MOSDBG("[VIC:%02x%02x%02x%02x%02x%02x] ",
  //   mmu_->dma_read_ram(0xd011),dma_read_ram(0xd012),
  //   mmu_->dma_read_ram(0xd016),dma_read_ram(0xd015),
  //   mmu_->dma_read_ram(0xd019),dma_read_ram(0xd01a)
  // );

  // MOSDBG("[CIA1:%02x%02x%02x CIA2:%02x%02x%02x VIC:%02x%02x] ",
  //   mmu_->dma_read_ram(0xdc0d),dma_read_ram(0xdc0e),dma_read_ram(0xdc0f),
  //   mmu_->dma_read_ram(0xdd0d),dma_read_ram(0xdd0e),dma_read_ram(0xdd0f),
  //   mmu_->dma_read_ram(0xd019),dma_read_ram(0xd01a)
  // );
  MOSDBG("[");
  cia1->dump_irqs();
  MOSDBG("|");
  cia2->dump_irqs();
  MOSDBG("|");
  vic->dump_irqs();
  MOSDBG("] ");

  MOSDBG("PC=%04x(%04x) M=%04X A=%02x X=%02x Y=%02x SP=%02x(%04x) [NV-BDIZC %d%d-%d%d%d%d%d] FL: %s\n",
    pc(), /* PC NOW */
    (mmu_->dma_read_ram(pc_address+1) | (mmu_->dma_read_ram(pc_address+2) << 8)), /* MEM */
    (mmu_->dma_read_ram(d_address) | (mmu_->dma_read_ram(d_address+1) << 8)),     /* RAM */
    a(),x(),y(), /* A X Y */
    sp(), /* SP */
    (mmu_->dma_read_ram(pBaseAddrStack+sp()) | (mmu_->dma_read_ram((pBaseAddrStack+sp())+1) << 8)), /* SP VAL */
    (flags()&SR_NEGATIVE)>>7,
    (flags()&SR_OVERFLOW)>>6,
    /* - */
    (flags()&SR_BREAK)>>4,
    (flags()&SR_DECIMAL)>>3,
    (flags()&SR_INTERRUPT)>>2,
    (flags()&SR_ZERO)>>1,
    (flags()&SR_CARRY),
    sflags.str().c_str());

}

static unsigned long log_num = 0;

void mos6510::dump_regs_insn(val_t insn)
{
  static CPUCLOCK prev_cycles = cycles();

  MOSDBG("C%8u(#%6u) INSN=%02X '%-9s' PCADDR:$%04x ADDR:$%04x VAL:$%02x CYC=%2u ",
    cycles_,
    ++log_num,
    insn,
    opcodenames[insn],
    pc_address, /* Opcode address */
    d_address,  /* Latest read/write address address */
    mmu_->dma_read_ram(d_address), /* READ/WRITE VALUE */
    (cycles()-prev_cycles));
  dump_regs();
  prev_cycles = cycles();
}

void mos6510::dump_regs_irq(uint_least8_t type, uint_least8_t source)
{
  const char * irq_types[2] = { "IRQ", "NMI" };
  const char * interrupt_sources[4] = {
    "CIA1", "CIA2", "VIC", "TMR"
  };
  MOSDBG("C%8u(#%6u) INSN=%02X '%-4s%-5s' PCADDR:$%04x ADDR:$%04x VAL:$%02x CYC=%2u ",
    cycles_,
    ++log_num,
    type,
    irq_types[type],
    interrupt_sources[source],
    pc_address, /* Opcode address */
    d_address,  /* Latest read/write address address */
    mmu_->dma_read_ram(d_address), /* READ/WRITE VALUE */
    7);
  dump_regs();
}

void mos6510::dump_regs_json()
{
  MOSDBG("{");
  MOSDBG("\"pc\":%d,",pc());
  MOSDBG("\"a\":%d,",a());
  MOSDBG("\"x\":%d,",x());
  MOSDBG("\"y\":%d,",y());
  MOSDBG("\"sp\":%d",sp());
  MOSDBG("}\n");
}

void mos6510::dbg()
{
  MOSDBG("INS %02X: %02X %02X %04X\n",load_byte(pc_-1),load_byte(pc_),load_byte(pc_+1),pc_);
  // MOSDBG("INS-1 %02X: %02X %02X %04X\n",load_byte(pc_-2),load_byte(pc_-1),load_byte(pc_+2),pc_-1);
  // MOSDBG("INS-2 %02X: %02X %02X %04X\n",load_byte(pc_-3),load_byte(pc_-2),load_byte(pc_+3),pc_-2);
}

void mos6510::dbg_a()
{
  dump_regs();
  dbg();
}

void mos6510::dbg_b()
{
  dbg();
  dump_regs();
}
