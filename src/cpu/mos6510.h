/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * mos6510.h
 * Cycle exact MOS 6510 core. One tick() is one bus cycle, never an
 * instruction: every read, every write and every dummy access happens in the
 * cycle the real chip would do it in.
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

#pragma once
#ifndef _US_CPU_MOS6510_H_
#define _US_CPU_MOS6510_H_

#include "constants.h"
#include "core/bus.h"
#include "memory.h"
#include "mmu.h"
#include "types.h"

namespace usbsid {

/* Status register bits */
enum : data_t {
  kFlagC = 0x01, /* carry */
  kFlagZ = 0x02, /* zero */
  kFlagI = 0x04, /* interrupt disable */
  kFlagD = 0x08, /* decimal */
  kFlagB = 0x10, /* break, only ever seen on the stack */
  kFlagU = 0x20, /* unused, always reads as 1 */
  kFlagV = 0x40, /* overflow */
  kFlagN = 0x80, /* negative */
};

/* Addressing mode of an opcode */
enum class Mode : uint8_t {
  Imp,   /* implied */
  Acc,   /* accumulator */
  Imm,   /* immediate */
  Zp,    /* zero page */
  ZpX,   /* zero page,X */
  ZpY,   /* zero page,Y */
  Abs,   /* absolute */
  AbsX,  /* absolute,X */
  AbsY,  /* absolute,Y */
  IndX,  /* (indirect,X) */
  IndY,  /* (indirect),Y */
  Rel,   /* relative, branches */
  Ind,   /* (indirect), JMP only */
  Stack, /* stack operations */
};

/* What the instruction does with the addressed byte */
enum class Kind : uint8_t {
  Implied, /* no operand access at all */
  Read,    /* reads the operand */
  Write,   /* writes the operand */
  Rmw,     /* read, dummy write, write */
  Branch,
  Jump,    /* JMP abs / JMP (ind) */
  Jsr,
  Rts,
  Rti,
  Brk,
  Push,
  Pull,
  Kil,     /* the JAM/KIL opcodes, the CPU stops dead */
};

/* The operation itself */
enum class Op : uint8_t {
  None,
  /* loads and stores */
  Lda, Ldx, Ldy, Sta, Stx, Sty,
  /* transfers */
  Tax, Tay, Txa, Tya, Tsx, Txs,
  /* stack */
  Pha, Php, Pla, Plp,
  /* logic */
  And, Eor, Ora, Bit,
  /* arithmetic */
  Adc, Sbc, Cmp, Cpx, Cpy,
  /* increment and decrement */
  Inc, Dec, Inx, Iny, Dex, Dey,
  /* shifts and rotates */
  Asl, Lsr, Rol, Ror,
  /* flags */
  Clc, Sec, Cli, Sei, Clv, Cld, Sed,
  /* control */
  Jmp, Jsr, Rts, Rti, Brk, Nop,
  /* branches */
  Bpl, Bmi, Bvc, Bvs, Bcc, Bcs, Bne, Beq,
  /* undocumented: combined read-modify-write plus accumulator operation */
  Slo, Rla, Sre, Rra, Dcp, Isc,
  /* undocumented: loads and stores */
  Lax, Sax, Las,
  /* undocumented: immediate only */
  Anc, Alr, Arr, Ane, Lxa, Sbx,
  /* undocumented: unstable stores, value depends on the address high byte */
  Sha, Shx, Shy, Tas,
};

/* One decode table entry */
struct OpcodeInfo {
  Mode mode;
  Kind kind;
  Op   op;
};

/**
 * @brief MOS 6510 microprocessor, cycle exact.
 */
class Mos6510 final : public ClockedDevice
{
  public:
    Mos6510(Memory & memory, Bus & bus);
    ~Mos6510(void) override = default;

    /* One bus cycle */
    void tick(void) override US_RAM_ATTR;
    void reset(void) override;


    /* Registers */
    US_ALWAYS_INLINE addr_t pc(void) const { return pc_; }
    US_ALWAYS_INLINE void pc(addr_t v) { pc_ = v; }
    US_ALWAYS_INLINE data_t sp(void) const { return sp_; }
    US_ALWAYS_INLINE void sp(data_t v) { sp_ = v; }
    US_ALWAYS_INLINE data_t a(void) const { return a_; }
    US_ALWAYS_INLINE void a(data_t v) { a_ = v; }
    US_ALWAYS_INLINE data_t x(void) const { return x_; }
    US_ALWAYS_INLINE void x(data_t v) { x_ = v; }
    US_ALWAYS_INLINE data_t y(void) const { return y_; }
    US_ALWAYS_INLINE void y(data_t v) { y_ = v; }
    US_ALWAYS_INLINE data_t p(void) const { return p_ | kFlagU; }
    US_ALWAYS_INLINE void p(data_t v) { p_ = (v | kFlagU) & ~kFlagB; }

    US_ALWAYS_INLINE bool flag(data_t mask) const { return (p_ & mask) != 0; }

    /**
     * @brief A "hot" reset: the registers, not the machine.
     *
     * A, X and Y cleared, the stack pointer back to $fd, and the flags to $30,
     * which most importantly clears **I**. The memory, the chips and the pc are
     * left alone, so this is not `reset()`.
     *
     * It exists for one caller: switching song by jumping into the driver's
     * "load another song" entry. That jump happens from wherever the tune was,
     * usually inside its own interrupt, and without this it inherits that
     * interrupt's stack and its interrupt-disable. Old player ~
     * src/c64/mos6510_cpu.cpp `hot_reset()`, which the working player calls at
     * exactly this point; leaving it out is why a new song used to carry on from
     * the middle of the old one instead of starting.
     */
    US_ALWAYS_INLINE void hot_reset(void)
    {
      p_ = 0x30;
      a_ = x_ = y_ = 0;
      sp_ = 0xfd;
    }

    /* True while the next tick would fetch a new opcode, which is also the
     * only moment at which the register set is architecturally visible. */
    US_ALWAYS_INLINE bool instruction_done(void) const { return state_ == State::Fetch; }
    /* Address the current instruction was fetched from */
    US_ALWAYS_INLINE addr_t instruction_pc(void) const { return insn_pc_; }
    /* Opcode currently executing */
    US_ALWAYS_INLINE data_t opcode(void) const { return opcode_; }
    /* Set by an undocumented opcode until step 2.3 implements them */
    US_ALWAYS_INLINE bool jammed(void) const { return jammed_; }
    /* Cycles spent in the current instruction so far */
    US_ALWAYS_INLINE unsigned insn_cycle(void) const { return cycle_; }

    /** @brief Hand the CPU its MMU by type, so the per cycle access is direct. */
    void attach_mmu(Mmu * mmu) { mmu_ = mmu; }

    /* Start a reset sequence (7 cycles, reads the reset vector) */
    void trigger_reset(void);

    static const OpcodeInfo & decode(data_t opcode) US_RAM_ATTR;
  private:
    /* Every state is exactly one bus cycle */
    enum class State : uint8_t {
      Fetch,        /* opcode fetch */
      ImpliedCycle, /* implied / accumulator dummy read */
      Immediate,
      ZpAddr,
      ZpiBase,
      ZpiAdd,
      AbsLo,
      AbsHi,
      AbsiLo,
      AbsiHi,
      AbsiFix,
      IndxPtr,
      IndxAdd,
      IndxLo,
      IndxHi,
      IndyPtr,
      IndyLo,
      IndyHi,
      IndyFix,
      ReadOperand,
      WriteOperand,
      RmwRead,
      RmwWriteOld,
      RmwWriteNew,
      BranchOffset,
      BranchTaken,
      BranchFix,
      JmpIndPtrLo,
      JmpIndPtrHi,
      JmpIndVecLo,
      JmpIndVecHi,
      JsrLo,
      JsrDummy,
      JsrPushHi,
      JsrPushLo,
      JsrHi,
      RtsDummy,
      RtsStackDummy,
      RtsPullLo,
      RtsPullHi,
      RtsBump,
      RtiDummy,
      RtiStackDummy,
      RtiPullP,
      RtiPullLo,
      RtiPullHi,
      PushDummy,
      PushWrite,
      PullDummy,
      PullStackDummy,
      PullRead,
      IntrDummy1,
      IntrDummy2,
      IntrPushHi,
      IntrPushLo,
      IntrPushP,
      IntrVecLo,
      IntrVecHi,
    };

    /* What kind of interrupt sequence is running */
    enum class IntrType : uint8_t { Brk, Irq, Nmi, Reset };

    /* bus helpers, every one of these is a real cycle */
    /* Every cycle of every instruction goes through one of these, so the
     * indirect call the Memory interface would cost is worth avoiding. A
     * whole machine hands over its MMU by type and the calls become direct;
     * the tests, which run the CPU against a flat 64 KB, keep the interface. */
    US_ALWAYS_INLINE data_t read(addr_t addr)
    {
      if (US_LIKELY(mmu_ != nullptr)) return mmu_->read(addr);
      return memory_.read(addr);
    }
    US_ALWAYS_INLINE void write(addr_t addr, data_t value)
    {
      if (US_LIKELY(mmu_ != nullptr)) { mmu_->write(addr, value); return; }
      memory_.write(addr, value);
    }
    US_ALWAYS_INLINE data_t pull(void) { return read(kBaseAddrStack | ++sp_); }
    US_ALWAYS_INLINE void push(data_t v) { write(kBaseAddrStack | sp_--, v); }

    /* flag helpers */
    US_ALWAYS_INLINE void set_flag(data_t mask, bool on)
    {
      if (on) p_ |= mask; else p_ &= static_cast<data_t>(~mask);
    }
    US_ALWAYS_INLINE void set_nz(data_t v)
    {
      set_flag(kFlagZ, v == 0);
      set_flag(kFlagN, (v & 0x80) != 0);
    }

    /* state machine */
    void decode_opcode(data_t opcode) US_RAM_ATTR;
    void start_operand_access(void) US_RAM_ATTR;
    void finish_read(data_t value) US_RAM_ATTR;
    data_t write_value(void) US_RAM_ATTR;
    data_t rmw_value(data_t value) US_RAM_ATTR;
    bool branch_taken(void) const US_RAM_ATTR;
    void end_instruction(void) US_RAM_ATTR;
    void start_interrupt(IntrType type) US_RAM_ATTR;
    bool ba_allows_cycle(void) US_RAM_ATTR;
    void sample_lines(void) US_RAM_ATTR;
    static bool state_writes(State s) US_RAM_ATTR;
    /* operations */
    void exec_implied(void) US_RAM_ATTR;
    void adc(data_t value) US_RAM_ATTR;
    void sbc(data_t value) US_RAM_ATTR;
    void arr(data_t value) US_RAM_ATTR;
    static bool is_unstable_store(Op op) US_RAM_ATTR;
    void compare(data_t reg, data_t value) US_RAM_ATTR;
    Memory & memory_;
    Mmu * mmu_ = nullptr;   /* the same memory, by type, when there is one */
    Bus & bus_;

    /* registers */
    addr_t pc_ = 0;
    data_t sp_ = 0xfd;
    data_t a_ = 0, x_ = 0, y_ = 0;
    data_t p_ = kFlagU | kFlagI;

    /* instruction state */
    State state_ = State::Fetch;
    data_t opcode_ = 0;
    Mode mode_ = Mode::Imp;
    Kind kind_ = Kind::Implied;
    Op   op_ = Op::None;
    addr_t insn_pc_ = 0;
    addr_t addr_ = 0;   /* effective address */
    addr_t ptr_ = 0;    /* pointer / branch target scratch */
    data_t data_ = 0;   /* operand */
    data_t base_hi_ = 0; /* high byte of the unindexed address, for SHA/SHX/SHY/TAS */
    data_t index_ = 0;  /* index register used by the current mode */
    bool page_crossed_ = false;
    unsigned cycle_ = 0;

    /* interrupts. The levels are read off the bus once per cycle. */
    IntrType intr_type_ = IntrType::Irq;
    bool irq_sampled_ = false; /* level as sampled one cycle ago */
    bool nmi_sampled_ = false;  /* edge as latched by the bus, one cycle ago */
    bool take_interrupt_ = false;
    bool nmi_hijack_ = false; /* an NMI stole the vector of a BRK or IRQ */
    bool jammed_ = false;

    /* BA / RDY. The VIC pulls BA low, the 6510 keeps going for three more
     * cycles and then only stalls on read cycles. The line itself is read
     * from the bus, only the grace counter is CPU state. */
    uint8_t ba_credit_ = 3;
};

} /* namespace usbsid */

#endif /* _US_CPU_MOS6510_H_ */
