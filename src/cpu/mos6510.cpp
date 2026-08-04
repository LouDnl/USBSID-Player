/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * mos6510.cpp
 * Cycle exact MOS 6510 core. Every state of the state machine below is
 * exactly one bus cycle, including the dummy reads and the dummy write of a
 * read-modify-write, because those are what the SID write timestamps and the
 * IO side effects depend on.
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
 *
 */

#include <array>

#include "constants.h"
#include "mos6510.h"

#include "mmu.h"

#include "util/logging.h"

namespace usbsid {

namespace {

/* Interrupt vectors */
constexpr addr_t kVectorNmi   = kAddrNmiVector;
constexpr addr_t kVectorReset = kAddrResetVector;
constexpr addr_t kVectorIrq   = kAddrIrqVector;

/* The ANE/LXA "magic constant". On real hardware the value depends on the
 * chip and the temperature; $ee is the value most commonly measured, and the
 * Acid800 LXA test calibrates itself against whatever the CPU produces. */
constexpr data_t kAneMagic = 0xee;

/**
 * @brief The 256 entry decode table.
 *
 * All 256 opcodes are decoded: the 151 documented ones and the undocumented
 * ones a 6510 really executes. Only the twelve JAM/KIL opcodes stop the CPU.
 */
constexpr std::array<OpcodeInfo, 256> make_opcode_table(void)
{
  std::array<OpcodeInfo, 256> t{};
  for (size_t i = 0; i < t.size(); i++) {
    t[i] = OpcodeInfo{ Mode::Imp, Kind::Kil, Op::None };
  }

  auto set = [&t](uint8_t opcode, Mode m, Kind k, Op o) {
    t[opcode] = OpcodeInfo{ m, k, o };
  };

  /* control */
  set(0x00, Mode::Imp,   Kind::Brk,     Op::Brk);
  set(0x20, Mode::Abs,   Kind::Jsr,     Op::Jsr);
  set(0x40, Mode::Stack, Kind::Rti,     Op::Rti);
  set(0x60, Mode::Stack, Kind::Rts,     Op::Rts);
  set(0x4c, Mode::Abs,   Kind::Jump,    Op::Jmp);
  set(0x6c, Mode::Ind,   Kind::Jump,    Op::Jmp);
  set(0xea, Mode::Imp,   Kind::Implied, Op::Nop);

  /* stack */
  set(0x08, Mode::Stack, Kind::Push, Op::Php);
  set(0x28, Mode::Stack, Kind::Pull, Op::Plp);
  set(0x48, Mode::Stack, Kind::Push, Op::Pha);
  set(0x68, Mode::Stack, Kind::Pull, Op::Pla);

  /* flags */
  set(0x18, Mode::Imp, Kind::Implied, Op::Clc);
  set(0x38, Mode::Imp, Kind::Implied, Op::Sec);
  set(0x58, Mode::Imp, Kind::Implied, Op::Cli);
  set(0x78, Mode::Imp, Kind::Implied, Op::Sei);
  set(0xb8, Mode::Imp, Kind::Implied, Op::Clv);
  set(0xd8, Mode::Imp, Kind::Implied, Op::Cld);
  set(0xf8, Mode::Imp, Kind::Implied, Op::Sed);

  /* transfers and register arithmetic */
  set(0x88, Mode::Imp, Kind::Implied, Op::Dey);
  set(0x8a, Mode::Imp, Kind::Implied, Op::Txa);
  set(0x98, Mode::Imp, Kind::Implied, Op::Tya);
  set(0x9a, Mode::Imp, Kind::Implied, Op::Txs);
  set(0xa8, Mode::Imp, Kind::Implied, Op::Tay);
  set(0xaa, Mode::Imp, Kind::Implied, Op::Tax);
  set(0xba, Mode::Imp, Kind::Implied, Op::Tsx);
  set(0xc8, Mode::Imp, Kind::Implied, Op::Iny);
  set(0xca, Mode::Imp, Kind::Implied, Op::Dex);
  set(0xe8, Mode::Imp, Kind::Implied, Op::Inx);

  /* branches */
  set(0x10, Mode::Rel, Kind::Branch, Op::Bpl);
  set(0x30, Mode::Rel, Kind::Branch, Op::Bmi);
  set(0x50, Mode::Rel, Kind::Branch, Op::Bvc);
  set(0x70, Mode::Rel, Kind::Branch, Op::Bvs);
  set(0x90, Mode::Rel, Kind::Branch, Op::Bcc);
  set(0xb0, Mode::Rel, Kind::Branch, Op::Bcs);
  set(0xd0, Mode::Rel, Kind::Branch, Op::Bne);
  set(0xf0, Mode::Rel, Kind::Branch, Op::Beq);

  /* accumulator shifts */
  set(0x0a, Mode::Acc, Kind::Implied, Op::Asl);
  set(0x2a, Mode::Acc, Kind::Implied, Op::Rol);
  set(0x4a, Mode::Acc, Kind::Implied, Op::Lsr);
  set(0x6a, Mode::Acc, Kind::Implied, Op::Ror);

  /* the regular ALU column: ORA AND EOR ADC STA LDA CMP SBC */
  struct AluRow { uint8_t base; Op op; bool store; };
  constexpr AluRow alu[] = {
    { 0x00, Op::Ora, false },
    { 0x20, Op::And, false },
    { 0x40, Op::Eor, false },
    { 0x60, Op::Adc, false },
    { 0x80, Op::Sta, true  },
    { 0xa0, Op::Lda, false },
    { 0xc0, Op::Cmp, false },
    { 0xe0, Op::Sbc, false },
  };
  for (const AluRow & r : alu) {
    const Kind k = r.store ? Kind::Write : Kind::Read;
    set(r.base | 0x01, Mode::IndX, k, r.op);
    set(r.base | 0x05, Mode::Zp,   k, r.op);
    set(r.base | 0x09, Mode::Imm,  Kind::Read, r.op); /* STA # does not exist */
    set(r.base | 0x0d, Mode::Abs,  k, r.op);
    set(r.base | 0x11, Mode::IndY, k, r.op);
    set(r.base | 0x15, Mode::ZpX,  k, r.op);
    set(r.base | 0x19, Mode::AbsY, k, r.op);
    set(r.base | 0x1d, Mode::AbsX, k, r.op);
  }
  /* $89 is not STA immediate, it is an undocumented NOP # */
  set(0x89, Mode::Imm, Kind::Read, Op::Nop);

  /* shifts and rotates on memory */
  struct RmwRow { uint8_t base; Op op; };
  constexpr RmwRow rmw[] = {
    { 0x00, Op::Asl },
    { 0x20, Op::Rol },
    { 0x40, Op::Lsr },
    { 0x60, Op::Ror },
  };
  for (const RmwRow & r : rmw) {
    set(r.base | 0x06, Mode::Zp,   Kind::Rmw, r.op);
    set(r.base | 0x0e, Mode::Abs,  Kind::Rmw, r.op);
    set(r.base | 0x16, Mode::ZpX,  Kind::Rmw, r.op);
    set(r.base | 0x1e, Mode::AbsX, Kind::Rmw, r.op);
  }

  /* increment and decrement memory */
  set(0xc6, Mode::Zp,   Kind::Rmw, Op::Dec);
  set(0xce, Mode::Abs,  Kind::Rmw, Op::Dec);
  set(0xd6, Mode::ZpX,  Kind::Rmw, Op::Dec);
  set(0xde, Mode::AbsX, Kind::Rmw, Op::Dec);
  set(0xe6, Mode::Zp,   Kind::Rmw, Op::Inc);
  set(0xee, Mode::Abs,  Kind::Rmw, Op::Inc);
  set(0xf6, Mode::ZpX,  Kind::Rmw, Op::Inc);
  set(0xfe, Mode::AbsX, Kind::Rmw, Op::Inc);

  /* index register loads and stores */
  set(0xa2, Mode::Imm,  Kind::Read,  Op::Ldx);
  set(0xa6, Mode::Zp,   Kind::Read,  Op::Ldx);
  set(0xae, Mode::Abs,  Kind::Read,  Op::Ldx);
  set(0xb6, Mode::ZpY,  Kind::Read,  Op::Ldx);
  set(0xbe, Mode::AbsY, Kind::Read,  Op::Ldx);
  set(0xa0, Mode::Imm,  Kind::Read,  Op::Ldy);
  set(0xa4, Mode::Zp,   Kind::Read,  Op::Ldy);
  set(0xac, Mode::Abs,  Kind::Read,  Op::Ldy);
  set(0xb4, Mode::ZpX,  Kind::Read,  Op::Ldy);
  set(0xbc, Mode::AbsX, Kind::Read,  Op::Ldy);
  set(0x86, Mode::Zp,   Kind::Write, Op::Stx);
  set(0x8e, Mode::Abs,  Kind::Write, Op::Stx);
  set(0x96, Mode::ZpY,  Kind::Write, Op::Stx);
  set(0x84, Mode::Zp,   Kind::Write, Op::Sty);
  set(0x8c, Mode::Abs,  Kind::Write, Op::Sty);
  set(0x94, Mode::ZpX,  Kind::Write, Op::Sty);

  /* compares and BIT */
  set(0xc0, Mode::Imm, Kind::Read, Op::Cpy);
  set(0xc4, Mode::Zp,  Kind::Read, Op::Cpy);
  set(0xcc, Mode::Abs, Kind::Read, Op::Cpy);
  set(0xe0, Mode::Imm, Kind::Read, Op::Cpx);
  set(0xe4, Mode::Zp,  Kind::Read, Op::Cpx);
  set(0xec, Mode::Abs, Kind::Read, Op::Cpx);
  set(0x24, Mode::Zp,  Kind::Read, Op::Bit);
  set(0x2c, Mode::Abs, Kind::Read, Op::Bit);

  /* ------------------------------------------------------------------ *
   * Undocumented opcodes. A 6510 executes all of these, and SID tunes
   * and PRGs in the test suite do use them.
   * ------------------------------------------------------------------ */

  /* combined read-modify-write plus accumulator operation */
  struct IllegalRmwRow { uint8_t base; Op op; };
  constexpr IllegalRmwRow irmw[] = {
    { 0x00, Op::Slo },  /* ASL + ORA */
    { 0x20, Op::Rla },  /* ROL + AND */
    { 0x40, Op::Sre },  /* LSR + EOR */
    { 0x60, Op::Rra },  /* ROR + ADC */
    { 0xc0, Op::Dcp },  /* DEC + CMP */
    { 0xe0, Op::Isc },  /* INC + SBC */
  };
  for (const IllegalRmwRow & r : irmw) {
    set(r.base | 0x03, Mode::IndX, Kind::Rmw, r.op);
    set(r.base | 0x07, Mode::Zp,   Kind::Rmw, r.op);
    set(r.base | 0x0f, Mode::Abs,  Kind::Rmw, r.op);
    set(r.base | 0x13, Mode::IndY, Kind::Rmw, r.op);
    set(r.base | 0x17, Mode::ZpX,  Kind::Rmw, r.op);
    set(r.base | 0x1b, Mode::AbsY, Kind::Rmw, r.op);
    set(r.base | 0x1f, Mode::AbsX, Kind::Rmw, r.op);
  }

  /* SAX: store A AND X */
  set(0x83, Mode::IndX, Kind::Write, Op::Sax);
  set(0x87, Mode::Zp,   Kind::Write, Op::Sax);
  set(0x8f, Mode::Abs,  Kind::Write, Op::Sax);
  set(0x97, Mode::ZpY,  Kind::Write, Op::Sax);

  /* LAX: load A and X at once */
  set(0xa3, Mode::IndX, Kind::Read, Op::Lax);
  set(0xa7, Mode::Zp,   Kind::Read, Op::Lax);
  set(0xaf, Mode::Abs,  Kind::Read, Op::Lax);
  set(0xb3, Mode::IndY, Kind::Read, Op::Lax);
  set(0xb7, Mode::ZpY,  Kind::Read, Op::Lax);
  set(0xbf, Mode::AbsY, Kind::Read, Op::Lax);

  /* immediate only oddities */
  set(0x0b, Mode::Imm, Kind::Read, Op::Anc); /* AND # then C = N */
  set(0x2b, Mode::Imm, Kind::Read, Op::Anc);
  set(0x4b, Mode::Imm, Kind::Read, Op::Alr); /* AND # then LSR A */
  set(0x6b, Mode::Imm, Kind::Read, Op::Arr); /* AND # then ROR A */
  set(0x8b, Mode::Imm, Kind::Read, Op::Ane); /* (A | magic) & X & # */
  set(0xab, Mode::Imm, Kind::Read, Op::Lxa); /* (A | magic) & # -> A,X */
  set(0xcb, Mode::Imm, Kind::Read, Op::Sbx); /* X = (A & X) - # */
  set(0xeb, Mode::Imm, Kind::Read, Op::Sbc); /* a second SBC # */

  /* unstable stores: the value depends on the address high byte */
  set(0x93, Mode::IndY, Kind::Write, Op::Sha);
  set(0x9f, Mode::AbsY, Kind::Write, Op::Sha);
  set(0x9e, Mode::AbsY, Kind::Write, Op::Shx);
  set(0x9c, Mode::AbsX, Kind::Write, Op::Shy);
  set(0x9b, Mode::AbsY, Kind::Write, Op::Tas); /* SP = A & X, then SHA */
  set(0xbb, Mode::AbsY, Kind::Read,  Op::Las); /* A = X = SP = mem & SP */

  /* undocumented NOPs. They still read their operand, page crossing
   * included, which is why they are Kind::Read and not Kind::Implied. */
  set(0x1a, Mode::Imp, Kind::Implied, Op::Nop);
  set(0x3a, Mode::Imp, Kind::Implied, Op::Nop);
  set(0x5a, Mode::Imp, Kind::Implied, Op::Nop);
  set(0x7a, Mode::Imp, Kind::Implied, Op::Nop);
  set(0xda, Mode::Imp, Kind::Implied, Op::Nop);
  set(0xfa, Mode::Imp, Kind::Implied, Op::Nop);
  set(0x80, Mode::Imm, Kind::Read, Op::Nop);
  set(0x82, Mode::Imm, Kind::Read, Op::Nop);
  set(0xc2, Mode::Imm, Kind::Read, Op::Nop);
  set(0xe2, Mode::Imm, Kind::Read, Op::Nop);
  set(0x04, Mode::Zp,  Kind::Read, Op::Nop);
  set(0x44, Mode::Zp,  Kind::Read, Op::Nop);
  set(0x64, Mode::Zp,  Kind::Read, Op::Nop);
  set(0x14, Mode::ZpX, Kind::Read, Op::Nop);
  set(0x34, Mode::ZpX, Kind::Read, Op::Nop);
  set(0x54, Mode::ZpX, Kind::Read, Op::Nop);
  set(0x74, Mode::ZpX, Kind::Read, Op::Nop);
  set(0xd4, Mode::ZpX, Kind::Read, Op::Nop);
  set(0xf4, Mode::ZpX, Kind::Read, Op::Nop);
  set(0x0c, Mode::Abs, Kind::Read, Op::Nop);
  set(0x1c, Mode::AbsX, Kind::Read, Op::Nop);
  set(0x3c, Mode::AbsX, Kind::Read, Op::Nop);
  set(0x5c, Mode::AbsX, Kind::Read, Op::Nop);
  set(0x7c, Mode::AbsX, Kind::Read, Op::Nop);
  set(0xdc, Mode::AbsX, Kind::Read, Op::Nop);
  set(0xfc, Mode::AbsX, Kind::Read, Op::Nop);

  /* the JAM/KIL opcodes are left as Kind::Kil by the default fill:
   * $02 $12 $22 $32 $42 $52 $62 $72 $92 $b2 $d2 $f2 */

  return t;
}

constexpr std::array<OpcodeInfo, 256> kOpcodes = make_opcode_table();

} /* namespace */

const OpcodeInfo & Mos6510::decode(data_t opcode)
{
  return kOpcodes[opcode];
}

Mos6510::Mos6510(Memory & memory, Bus & bus)
  : memory_(memory), bus_(bus)
{
}

void Mos6510::reset(void)
{
  a_ = x_ = y_ = 0;
  sp_ = 0xfd;
  p_ = kFlagU | kFlagI;
  state_ = State::Fetch;
  cycle_ = 0;
  jammed_ = false;
  irq_sampled_ = nmi_sampled_ = take_interrupt_ = false;
  nmi_hijack_ = false;
  ba_credit_ = 3;

  /* Vector fetch without spending cycles. The cycle exact variant is
   * trigger_reset(), which a real machine reset uses. */
  const data_t lo = read(kVectorReset);
  const data_t hi = read(kVectorReset + 1);
  pc_ = static_cast<addr_t>((hi << 8) | lo);
  insn_pc_ = pc_;
}

void Mos6510::trigger_reset(void)
{
  jammed_ = false;
  start_interrupt(IntrType::Reset);
  state_ = State::IntrDummy1;
}

bool Mos6510::state_writes(State s)
{
  switch (s) {
    case State::WriteOperand:
    case State::RmwWriteOld:
    case State::RmwWriteNew:
    case State::JsrPushHi:
    case State::JsrPushLo:
    case State::PushWrite:
    case State::IntrPushHi:
    case State::IntrPushLo:
    case State::IntrPushP:
      return true;
    default:
      return false;
  }
}

bool Mos6510::ba_allows_cycle(void)
{
  /* BA is read off the bus, and the three cycle grace restarts every time
   * the VIC lets go of the bus again. */
  const bool ba = bus_.ba();
  if (US_LIKELY(ba)) { ba_credit_ = 3; return true; }
  /* A 6510 keeps running for three cycles after BA drops */
  if (ba_credit_ > 0) { --ba_credit_; return true; }
  /* After that only write cycles get through, reads are held off */
  return state_writes(state_);
}

void Mos6510::sample_lines(void)
{
  /* Levels as of the end of this cycle, read straight off the bus.
   * end_instruction() looks at these, which is one cycle old by then, so an
   * interrupt asserted in the last cycle of an instruction is correctly taken
   * only after the next one. */
  irq_sampled_ = bus_.irq_asserted() && !flag(kFlagI);
  nmi_sampled_ = bus_.nmi_edge();
}

void Mos6510::end_instruction(void)
{
  state_ = State::Fetch;
  take_interrupt_ = irq_sampled_ || nmi_sampled_;
}

void Mos6510::start_interrupt(IntrType type)
{
  intr_type_ = type;
  insn_pc_ = pc_;
  cycle_ = 1;
  if (type == IntrType::Nmi) bus_.clear_nmi_edge();
}

void Mos6510::decode_opcode(data_t opcode)
{
  const OpcodeInfo & info = kOpcodes[opcode];
  mode_ = info.mode;
  kind_ = info.kind;
  op_   = info.op;
  page_crossed_ = false;

  switch (kind_) {
    case Kind::Kil:
      jammed_ = true;
      state_ = State::Fetch;
      return;
    case Kind::Brk:
      start_interrupt(IntrType::Brk);
      insn_pc_ = static_cast<addr_t>(pc_ - 1);
      state_ = State::IntrDummy2;
      return;
    case Kind::Push:   state_ = State::PushDummy;    return;
    case Kind::Pull:   state_ = State::PullDummy;    return;
    case Kind::Rts:    state_ = State::RtsDummy;     return;
    case Kind::Rti:    state_ = State::RtiDummy;     return;
    case Kind::Jsr:    state_ = State::JsrLo;        return;
    case Kind::Branch: state_ = State::BranchOffset; return;
    default: break;
  }

  switch (mode_) {
    case Mode::Imp:
    case Mode::Acc:  state_ = State::ImpliedCycle; break;
    case Mode::Imm:  state_ = State::Immediate;    break;
    case Mode::Zp:   state_ = State::ZpAddr;       break;
    case Mode::ZpX:  index_ = x_; state_ = State::ZpiBase; break;
    case Mode::ZpY:  index_ = y_; state_ = State::ZpiBase; break;
    case Mode::Abs:  state_ = State::AbsLo;        break;
    case Mode::AbsX: index_ = x_; state_ = State::AbsiLo;  break;
    case Mode::AbsY: index_ = y_; state_ = State::AbsiLo;  break;
    case Mode::IndX: state_ = State::IndxPtr;      break;
    case Mode::IndY: state_ = State::IndyPtr;      break;
    case Mode::Ind:  state_ = State::JmpIndPtrLo;  break;
    default:         state_ = State::ImpliedCycle; break;
  }
}

void Mos6510::start_operand_access(void)
{
  switch (kind_) {
    case Kind::Read:  state_ = State::ReadOperand;  break;
    case Kind::Write: state_ = State::WriteOperand; break;
    case Kind::Rmw:   state_ = State::RmwRead;      break;
    case Kind::Jump:  pc_ = addr_; end_instruction(); break;
    default:          end_instruction();            break;
  }
}

data_t Mos6510::write_value(void)
{
  switch (op_) {
    case Op::Sta: return a_;
    case Op::Stx: return x_;
    case Op::Sty: return y_;
    case Op::Sax: return static_cast<data_t>(a_ & x_);
    /* The unstable stores AND the register with the high byte of the
     * unindexed address plus one. That is why they are unusable in practice
     * and why they need base_hi_ rather than the effective address. */
    case Op::Sha:
      return static_cast<data_t>(a_ & x_ & (base_hi_ + 1));
    case Op::Shx:
      return static_cast<data_t>(x_ & (base_hi_ + 1));
    case Op::Shy:
      return static_cast<data_t>(y_ & (base_hi_ + 1));
    case Op::Tas:
      sp_ = static_cast<data_t>(a_ & x_);
      return static_cast<data_t>(sp_ & (base_hi_ + 1));
    default:      return data_;
  }
}

bool Mos6510::is_unstable_store(Op op)
{
  return op == Op::Sha || op == Op::Shx || op == Op::Shy || op == Op::Tas;
}

data_t Mos6510::rmw_value(data_t value)
{
  data_t result = value;
  switch (op_) {
    case Op::Asl:
      set_flag(kFlagC, (value & 0x80) != 0);
      result = static_cast<data_t>(value << 1);
      break;
    case Op::Lsr:
      set_flag(kFlagC, (value & 0x01) != 0);
      result = static_cast<data_t>(value >> 1);
      break;
    case Op::Rol: {
      const data_t carry = flag(kFlagC) ? 1 : 0;
      set_flag(kFlagC, (value & 0x80) != 0);
      result = static_cast<data_t>((value << 1) | carry);
      break;
    }
    case Op::Ror: {
      const data_t carry = flag(kFlagC) ? 0x80 : 0;
      set_flag(kFlagC, (value & 0x01) != 0);
      result = static_cast<data_t>((value >> 1) | carry);
      break;
    }
    case Op::Inc: result = static_cast<data_t>(value + 1); break;
    case Op::Dec: result = static_cast<data_t>(value - 1); break;

    /* The combined undocumented operations take their N and Z from the
     * accumulator side, not from the value written back. */
    case Op::Slo:
      set_flag(kFlagC, (value & 0x80) != 0);
      result = static_cast<data_t>(value << 1);
      a_ = static_cast<data_t>(a_ | result);
      set_nz(a_);
      return result;
    case Op::Rla: {
      const data_t carry = flag(kFlagC) ? 1 : 0;
      set_flag(kFlagC, (value & 0x80) != 0);
      result = static_cast<data_t>((value << 1) | carry);
      a_ = static_cast<data_t>(a_ & result);
      set_nz(a_);
      return result;
    }
    case Op::Sre:
      set_flag(kFlagC, (value & 0x01) != 0);
      result = static_cast<data_t>(value >> 1);
      a_ = static_cast<data_t>(a_ ^ result);
      set_nz(a_);
      return result;
    case Op::Rra: {
      const data_t carry = flag(kFlagC) ? 0x80 : 0;
      set_flag(kFlagC, (value & 0x01) != 0);
      result = static_cast<data_t>((value >> 1) | carry);
      adc(result);
      return result;
    }
    case Op::Dcp:
      result = static_cast<data_t>(value - 1);
      compare(a_, result);
      return result;
    case Op::Isc:
      result = static_cast<data_t>(value + 1);
      sbc(result);
      return result;

    default: break;
  }
  set_nz(result);
  return result;
}

void Mos6510::compare(data_t reg, data_t value)
{
  const uint16_t result = static_cast<uint16_t>(reg) - value;
  set_flag(kFlagC, reg >= value);
  set_nz(static_cast<data_t>(result & 0xff));
}

/**
 * @brief ADC, binary and BCD.
 *
 * The decimal path follows the documented 6502 behaviour: the carry comes
 * from the decimal adjusted result, Z comes from the binary result, N and V
 * come from the intermediate before the final +$60 correction.
 */
void Mos6510::adc(data_t value)
{
  const uint16_t carry = flag(kFlagC) ? 1 : 0;

  if (!flag(kFlagD)) {
    const uint16_t sum = static_cast<uint16_t>(a_) + value + carry;
    set_flag(kFlagC, sum > 0xff);
    set_flag(kFlagV, ((a_ ^ sum) & (value ^ sum) & 0x80) != 0);
    a_ = static_cast<data_t>(sum & 0xff);
    set_nz(a_);
    return;
  }

  uint16_t tmp = static_cast<uint16_t>(a_ & 0x0f) + (value & 0x0f) + carry;
  if (tmp > 0x09) tmp += 0x06;
  if (tmp <= 0x0f) {
    tmp = (tmp & 0x0f) + (a_ & 0xf0) + (value & 0xf0);
  } else {
    tmp = (tmp & 0x0f) + (a_ & 0xf0) + (value & 0xf0) + 0x10;
  }

  set_flag(kFlagZ, ((a_ + value + carry) & 0xff) == 0);
  set_flag(kFlagN, (tmp & 0x80) != 0);
  set_flag(kFlagV, (((a_ ^ tmp) & 0x80) != 0) && (((a_ ^ value) & 0x80) == 0));

  if ((tmp & 0x1f0) > 0x90) tmp += 0x60;
  set_flag(kFlagC, (tmp & 0xff0) > 0xf0);
  a_ = static_cast<data_t>(tmp & 0xff);
}

/**
 * @brief SBC, binary and BCD.
 *
 * All flags come from the binary result, which is what the 6502 does; only
 * the accumulator value is decimal adjusted.
 */
void Mos6510::sbc(data_t value)
{
  const uint16_t borrow = flag(kFlagC) ? 0 : 1;
  const uint16_t bin = static_cast<uint16_t>(a_) - value - borrow;

  set_flag(kFlagC, bin < 0x100);
  set_flag(kFlagV, ((a_ ^ bin) & (a_ ^ value) & 0x80) != 0);
  set_nz(static_cast<data_t>(bin & 0xff));

  if (!flag(kFlagD)) {
    a_ = static_cast<data_t>(bin & 0xff);
    return;
  }

  uint16_t lo = static_cast<uint16_t>(a_ & 0x0f) - (value & 0x0f) - borrow;
  uint16_t result;
  if (lo & 0x10) {
    result = ((lo - 0x06) & 0x0f) |
             static_cast<uint16_t>((a_ & 0xf0) - (value & 0xf0) - 0x10);
  } else {
    result = (lo & 0x0f) | static_cast<uint16_t>((a_ & 0xf0) - (value & 0xf0));
  }
  if (result & 0x100) result -= 0x60;
  a_ = static_cast<data_t>(result & 0xff);
}

void Mos6510::finish_read(data_t value)
{
  switch (op_) {
    case Op::Lda: a_ = value; set_nz(a_); break;
    case Op::Ldx: x_ = value; set_nz(x_); break;
    case Op::Ldy: y_ = value; set_nz(y_); break;
    case Op::And: a_ = static_cast<data_t>(a_ & value); set_nz(a_); break;
    case Op::Ora: a_ = static_cast<data_t>(a_ | value); set_nz(a_); break;
    case Op::Eor: a_ = static_cast<data_t>(a_ ^ value); set_nz(a_); break;
    case Op::Adc: adc(value); break;
    case Op::Sbc: sbc(value); break;
    case Op::Cmp: compare(a_, value); break;
    case Op::Cpx: compare(x_, value); break;
    case Op::Cpy: compare(y_, value); break;
    case Op::Bit:
      set_flag(kFlagZ, (a_ & value) == 0);
      set_flag(kFlagN, (value & 0x80) != 0);
      set_flag(kFlagV, (value & 0x40) != 0);
      break;

    /* undocumented */
    case Op::Lax: a_ = value; x_ = value; set_nz(a_); break;
    case Op::Las:
      a_ = x_ = sp_ = static_cast<data_t>(value & sp_);
      set_nz(a_);
      break;
    case Op::Anc:
      a_ = static_cast<data_t>(a_ & value);
      set_nz(a_);
      /* the carry ends up as a copy of the sign bit */
      set_flag(kFlagC, (a_ & 0x80) != 0);
      break;
    case Op::Alr:
      a_ = static_cast<data_t>(a_ & value);
      set_flag(kFlagC, (a_ & 0x01) != 0);
      a_ = static_cast<data_t>(a_ >> 1);
      set_nz(a_);
      break;
    case Op::Arr: arr(value); break;
    case Op::Ane:
      a_ = static_cast<data_t>((a_ | kAneMagic) & x_ & value);
      set_nz(a_);
      break;
    case Op::Lxa:
      a_ = static_cast<data_t>((a_ | kAneMagic) & value);
      x_ = a_;
      set_nz(a_);
      break;
    case Op::Sbx: {
      const data_t base = static_cast<data_t>(a_ & x_);
      set_flag(kFlagC, base >= value);
      x_ = static_cast<data_t>(base - value);
      set_nz(x_);
      break;
    }
    default: break;
  }
}

/**
 * @brief ARR, AND immediate followed by a rotate right with unusual flags.
 *
 * Binary mode: C is bit 6 of the result and V is bit 6 xor bit 5.
 * Decimal mode: the nibbles are corrected separately and C comes from the
 * high nibble, which is the behaviour VICE and Altirra both implement.
 */
void Mos6510::arr(data_t value)
{
  const data_t carry_in = flag(kFlagC) ? 0x80 : 0x00;
  const data_t t = static_cast<data_t>(a_ & value);

  if (!flag(kFlagD)) {
    a_ = static_cast<data_t>((t >> 1) | carry_in);
    set_nz(a_);
    set_flag(kFlagC, (a_ & 0x40) != 0);
    set_flag(kFlagV, (((a_ >> 6) ^ (a_ >> 5)) & 0x01) != 0);
    return;
  }

  const data_t ah = static_cast<data_t>(t >> 4);
  const data_t al = static_cast<data_t>(t & 0x0f);

  data_t result = static_cast<data_t>((t >> 1) | carry_in);
  set_nz(result);
  set_flag(kFlagN, carry_in != 0);
  set_flag(kFlagV, ((t ^ result) & 0x40) != 0);

  if ((al + (al & 1)) > 5) {
    result = static_cast<data_t>((result & 0xf0) | ((result + 6) & 0x0f));
  }
  const bool carry_out = (ah + (ah & 1)) > 5;
  set_flag(kFlagC, carry_out);
  if (carry_out) result = static_cast<data_t>(result + 0x60);

  a_ = result;
}

bool Mos6510::branch_taken(void) const
{
  switch (op_) {
    case Op::Bpl: return !flag(kFlagN);
    case Op::Bmi: return  flag(kFlagN);
    case Op::Bvc: return !flag(kFlagV);
    case Op::Bvs: return  flag(kFlagV);
    case Op::Bcc: return !flag(kFlagC);
    case Op::Bcs: return  flag(kFlagC);
    case Op::Bne: return !flag(kFlagZ);
    case Op::Beq: return  flag(kFlagZ);
    default: return false;
  }
}

void Mos6510::exec_implied(void)
{
  switch (op_) {
    case Op::Tax: x_ = a_; set_nz(x_); break;
    case Op::Tay: y_ = a_; set_nz(y_); break;
    case Op::Txa: a_ = x_; set_nz(a_); break;
    case Op::Tya: a_ = y_; set_nz(a_); break;
    case Op::Tsx: x_ = sp_; set_nz(x_); break;
    case Op::Txs: sp_ = x_; break; /* TXS touches no flags */
    case Op::Inx: x_ = static_cast<data_t>(x_ + 1); set_nz(x_); break;
    case Op::Iny: y_ = static_cast<data_t>(y_ + 1); set_nz(y_); break;
    case Op::Dex: x_ = static_cast<data_t>(x_ - 1); set_nz(x_); break;
    case Op::Dey: y_ = static_cast<data_t>(y_ - 1); set_nz(y_); break;
    case Op::Clc: set_flag(kFlagC, false); break;
    case Op::Sec: set_flag(kFlagC, true);  break;
    case Op::Cli: set_flag(kFlagI, false); break;
    case Op::Sei: set_flag(kFlagI, true);  break;
    case Op::Clv: set_flag(kFlagV, false); break;
    case Op::Cld: set_flag(kFlagD, false); break;
    case Op::Sed: set_flag(kFlagD, true);  break;
    case Op::Asl: case Op::Lsr: case Op::Rol: case Op::Ror:
      a_ = rmw_value(a_);
      break;
    case Op::Nop: break;
    default: break;
  }
}

void Mos6510::tick(void)
{
  if (US_UNLIKELY(jammed_)) return;
  if (US_UNLIKELY(!ba_allows_cycle())) return;

  ++cycle_;

  switch (state_) {

    /* ---- opcode fetch ------------------------------------------------- */
    case State::Fetch:
      if (US_UNLIKELY(take_interrupt_)) {
        take_interrupt_ = false;
        start_interrupt(nmi_sampled_ ? IntrType::Nmi : IntrType::Irq);
        read(pc_); /* dummy */
        state_ = State::IntrDummy2;
        break;
      }
      insn_pc_ = pc_;
      cycle_ = 1;
      opcode_ = read(pc_++);
      US_LOG_IF(instructions,
                "[CPU] $%04x op $%02x a:%02x x:%02x y:%02x sp:%02x p:%02x "
                "[C]%llu\n",
                insn_pc_, opcode_, a_, x_, y_, sp_, p_,
                static_cast<unsigned long long>(bus_.cycles()));
      decode_opcode(opcode_);
      break;

    /* ---- addressing --------------------------------------------------- */
    case State::ImpliedCycle:
      read(pc_); /* dummy read of the next byte, pc does not move */
      exec_implied();
      end_instruction();
      break;

    case State::Immediate:
      data_ = read(pc_++);
      finish_read(data_);
      end_instruction();
      break;

    case State::ZpAddr:
      addr_ = read(pc_++);
      start_operand_access();
      break;

    case State::ZpiBase:
      addr_ = read(pc_++);
      state_ = State::ZpiAdd;
      break;

    case State::ZpiAdd:
      read(addr_); /* dummy read of the unindexed address */
      addr_ = static_cast<addr_t>((addr_ + index_) & 0xff);
      start_operand_access();
      break;

    case State::AbsLo:
      addr_ = read(pc_++);
      state_ = State::AbsHi;
      break;

    case State::AbsHi:
      addr_ = static_cast<addr_t>(addr_ | (read(pc_++) << 8));
      if (kind_ == Kind::Jump) {
        pc_ = addr_;
        end_instruction();
      } else {
        start_operand_access();
      }
      break;

    case State::AbsiLo:
      ptr_ = read(pc_++);
      state_ = State::AbsiHi;
      break;

    case State::AbsiHi: {
      const uint16_t hi = read(pc_++);
      base_hi_ = static_cast<data_t>(hi); /* the unstable stores need this */
      const uint16_t t = static_cast<uint16_t>(ptr_ + index_);
      page_crossed_ = (t > 0xff);
      addr_ = static_cast<addr_t>((hi << 8) | (t & 0xff));
      state_ = State::AbsiFix;
      break;
    }

    case State::AbsiFix:
      /* The 6502 always reads from the not yet corrected address here */
      data_ = read(addr_);
      if (page_crossed_) addr_ = static_cast<addr_t>(addr_ + 0x100);
      if (kind_ == Kind::Read && !page_crossed_) {
        finish_read(data_);
        end_instruction();
      } else {
        start_operand_access();
      }
      break;

    case State::IndxPtr:
      ptr_ = read(pc_++);
      state_ = State::IndxAdd;
      break;

    case State::IndxAdd:
      read(ptr_); /* dummy */
      ptr_ = static_cast<addr_t>((ptr_ + x_) & 0xff);
      state_ = State::IndxLo;
      break;

    case State::IndxLo:
      addr_ = read(static_cast<addr_t>(ptr_));
      state_ = State::IndxHi;
      break;

    case State::IndxHi:
      addr_ = static_cast<addr_t>(
        addr_ | (read(static_cast<addr_t>((ptr_ + 1) & 0xff)) << 8));
      start_operand_access();
      break;

    case State::IndyPtr:
      ptr_ = read(pc_++);
      state_ = State::IndyLo;
      break;

    case State::IndyLo:
      addr_ = read(static_cast<addr_t>(ptr_));
      state_ = State::IndyHi;
      break;

    case State::IndyHi: {
      const uint16_t hi = read(static_cast<addr_t>((ptr_ + 1) & 0xff));
      base_hi_ = static_cast<data_t>(hi);
      const uint16_t t = static_cast<uint16_t>(addr_ + y_);
      page_crossed_ = (t > 0xff);
      addr_ = static_cast<addr_t>((hi << 8) | (t & 0xff));
      state_ = State::IndyFix;
      break;
    }

    case State::IndyFix:
      data_ = read(addr_);
      if (page_crossed_) addr_ = static_cast<addr_t>(addr_ + 0x100);
      if (kind_ == Kind::Read && !page_crossed_) {
        finish_read(data_);
        end_instruction();
      } else {
        start_operand_access();
      }
      break;

    /* ---- operand access ------------------------------------------------ */
    case State::ReadOperand:
      data_ = read(addr_);
      finish_read(data_);
      end_instruction();
      break;

    case State::WriteOperand: {
      const data_t value = write_value();
      /* When an unstable store crosses a page the stored value also replaces
       * the high byte of the address it lands on. */
      if (US_UNLIKELY(is_unstable_store(op_) && page_crossed_)) {
        addr_ = static_cast<addr_t>((value << 8) | (addr_ & 0x00ff));
      }
      write(addr_, value);
      end_instruction();
      break;
    }

    case State::RmwRead:
      data_ = read(addr_);
      state_ = State::RmwWriteOld;
      break;

    case State::RmwWriteOld:
      /* The real chip writes the unmodified value back first. IO chips see
       * that write, which is what makes INC $D019 style tricks work. */
      write(addr_, data_);
      data_ = rmw_value(data_);
      state_ = State::RmwWriteNew;
      break;

    case State::RmwWriteNew:
      write(addr_, data_);
      end_instruction();
      break;

    /* ---- branches ------------------------------------------------------ */
    case State::BranchOffset:
      data_ = read(pc_++);
      if (!branch_taken()) {
        end_instruction();
      } else {
        state_ = State::BranchTaken;
      }
      break;

    case State::BranchTaken: {
      read(pc_); /* dummy read at the not yet adjusted pc */
      const addr_t target =
        static_cast<addr_t>(pc_ + static_cast<int8_t>(data_));
      if ((target & 0xff00) == (pc_ & 0xff00)) {
        pc_ = target;
        end_instruction();
      } else {
        ptr_ = target;
        pc_ = static_cast<addr_t>((pc_ & 0xff00) | (target & 0x00ff));
        state_ = State::BranchFix;
      }
      break;
    }

    case State::BranchFix:
      read(pc_); /* dummy read at the wrong high byte */
      pc_ = ptr_;
      end_instruction();
      break;

    /* ---- JMP (indirect) ------------------------------------------------ */
    case State::JmpIndPtrLo:
      ptr_ = read(pc_++);
      state_ = State::JmpIndPtrHi;
      break;

    case State::JmpIndPtrHi:
      ptr_ = static_cast<addr_t>(ptr_ | (read(pc_++) << 8));
      state_ = State::JmpIndVecLo;
      break;

    case State::JmpIndVecLo:
      addr_ = read(ptr_);
      state_ = State::JmpIndVecHi;
      break;

    case State::JmpIndVecHi:
      /* The famous page wrap bug: the high byte comes from the same page */
      addr_ = static_cast<addr_t>(
        addr_ | (read(static_cast<addr_t>((ptr_ & 0xff00) |
                                          ((ptr_ + 1) & 0x00ff))) << 8));
      pc_ = addr_;
      end_instruction();
      break;

    /* ---- JSR ----------------------------------------------------------- */
    case State::JsrLo:
      addr_ = read(pc_++);
      state_ = State::JsrDummy;
      break;

    case State::JsrDummy:
      read(static_cast<addr_t>(kBaseAddrStack | sp_)); /* dummy stack read */
      state_ = State::JsrPushHi;
      break;

    case State::JsrPushHi:
      push(static_cast<data_t>(pc_ >> 8));
      state_ = State::JsrPushLo;
      break;

    case State::JsrPushLo:
      push(static_cast<data_t>(pc_ & 0xff));
      state_ = State::JsrHi;
      break;

    case State::JsrHi:
      addr_ = static_cast<addr_t>(addr_ | (read(pc_) << 8));
      pc_ = addr_;
      end_instruction();
      break;

    /* ---- RTS ----------------------------------------------------------- */
    case State::RtsDummy:
      read(pc_);
      state_ = State::RtsStackDummy;
      break;

    case State::RtsStackDummy:
      read(static_cast<addr_t>(kBaseAddrStack | sp_));
      state_ = State::RtsPullLo;
      break;

    case State::RtsPullLo:
      addr_ = pull();
      state_ = State::RtsPullHi;
      break;

    case State::RtsPullHi:
      addr_ = static_cast<addr_t>(addr_ | (pull() << 8));
      pc_ = addr_;
      state_ = State::RtsBump;
      break;

    case State::RtsBump:
      read(pc_);
      pc_++;
      end_instruction();
      break;

    /* ---- RTI ----------------------------------------------------------- */
    case State::RtiDummy:
      read(pc_);
      state_ = State::RtiStackDummy;
      break;

    case State::RtiStackDummy:
      read(static_cast<addr_t>(kBaseAddrStack | sp_));
      state_ = State::RtiPullP;
      break;

    case State::RtiPullP:
      p(pull());
      state_ = State::RtiPullLo;
      break;

    case State::RtiPullLo:
      addr_ = pull();
      state_ = State::RtiPullHi;
      break;

    case State::RtiPullHi:
      addr_ = static_cast<addr_t>(addr_ | (pull() << 8));
      pc_ = addr_;
      end_instruction();
      break;

    /* ---- stack --------------------------------------------------------- */
    case State::PushDummy:
      read(pc_);
      state_ = State::PushWrite;
      break;

    case State::PushWrite:
      /* PHP always pushes with B and the unused bit set */
      push(op_ == Op::Pha
             ? a_
             : static_cast<data_t>(p_ | kFlagU | kFlagB));
      end_instruction();
      break;

    case State::PullDummy:
      read(pc_);
      state_ = State::PullStackDummy;
      break;

    case State::PullStackDummy:
      read(static_cast<addr_t>(kBaseAddrStack | sp_));
      state_ = State::PullRead;
      break;

    case State::PullRead: {
      const data_t v = pull();
      if (op_ == Op::Pla) {
        a_ = v;
        set_nz(a_);
      } else {
        p(v); /* PLP: B is ignored, the unused bit stays set */
      }
      end_instruction();
      break;
    }

    /* ---- interrupts and BRK -------------------------------------------- */
    case State::IntrDummy1:
      read(pc_);
      state_ = State::IntrDummy2;
      break;

    case State::IntrDummy2:
      if (intr_type_ == IntrType::Brk) {
        read(pc_++); /* BRK skips the signature byte */
      } else {
        read(pc_);
      }
      state_ = State::IntrPushHi;
      break;

    case State::IntrPushHi:
      if (intr_type_ == IntrType::Reset) {
        read(static_cast<addr_t>(kBaseAddrStack | sp_--));
      } else {
        push(static_cast<data_t>(pc_ >> 8));
      }
      state_ = State::IntrPushLo;
      break;

    case State::IntrPushLo:
      if (intr_type_ == IntrType::Reset) {
        read(static_cast<addr_t>(kBaseAddrStack | sp_--));
      } else {
        push(static_cast<data_t>(pc_ & 0xff));
      }
      state_ = State::IntrPushP;
      break;

    case State::IntrPushP:
      if (intr_type_ == IntrType::Reset) {
        read(static_cast<addr_t>(kBaseAddrStack | sp_--));
      } else {
        /* B is only set for BRK, a hardware interrupt pushes it clear */
        push(static_cast<data_t>(
          (p_ | kFlagU) |
          (intr_type_ == IntrType::Brk ? kFlagB : 0)));
      }
      set_flag(kFlagI, true);
      /* An NMI that arrives before the vector fetch steals the vector from a
       * BRK or an IRQ. The flags already pushed keep the original B value,
       * which is the well known "BRK swallowed by NMI" behaviour. */
      if (intr_type_ != IntrType::Nmi && intr_type_ != IntrType::Reset &&
          bus_.nmi_edge()) {
        nmi_hijack_ = true;
        bus_.clear_nmi_edge();
      }
      state_ = State::IntrVecLo;
      break;

    case State::IntrVecLo: {
      const addr_t vec = (nmi_hijack_ ||
                          intr_type_ == IntrType::Nmi)    ? kVectorNmi
                       : (intr_type_ == IntrType::Reset)  ? kVectorReset
                                                          : kVectorIrq;
      addr_ = vec;
      data_ = read(vec);
      state_ = State::IntrVecHi;
      break;
    }

    case State::IntrVecHi:
      pc_ = static_cast<addr_t>(
        data_ | (read(static_cast<addr_t>(addr_ + 1)) << 8));
      nmi_hijack_ = false;
      end_instruction();
      break;
  }

  sample_lines();
}

} /* namespace usbsid */
