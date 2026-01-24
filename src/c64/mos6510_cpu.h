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
 * mos6510_cpu.h
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

#ifndef _MOS6510_H
#define _MOS6510_H


#include <cstdint>
#include <functional>
#include <iostream>
#include <ios>
#include <sstream>

#include <types.h>
#include <constants.h>


/* These define the position of the status
 * flags in the status (flags) register
 */
#define SR_NEGATIVE      0x80
#define SR_SIGN          0x80
#define SR_OVERFLOW      0x40
#define SR_UNUSED        0x20
#define SR_BREAK         0x10
#define SR_DECIMAL       0x08
#define SR_INTERRUPT     0x04
#define SR_ZERO          0x02
#define SR_CARRY         0x01

#define ANE_MAGIC        0xef

/* macro helpers */
#define SETFLAG(flag, cond) \
    if (cond) _flags |= (val_t)flag; \
    else _flags &= ~(val_t)flag;
#define GETFLAG(flag) (_flags & flag)

#define SET_ZF(val) \
  do { \
    if (!(val_t)(val)) { \
      SETFLAG(SR_ZERO, 1) \
    } else { \
      SETFLAG(SR_ZERO, 0 ) \
    } \
  } while(0)
#define SET_OF(val) \
  do { \
    if (((val_t)(val)&0x40)!=0) { \
      SETFLAG(SR_OVERFLOW, 1) \
    } else { \
      SETFLAG(SR_OVERFLOW, 0) \
    } \
  } while(0)
#define SET_NF(val) do { if (((val_t)(val)&0x80)!=0) { SETFLAG(SR_NEGATIVE, 1) } else { SETFLAG(SR_NEGATIVE, 0) } } while(0)
#define SET_CF(val) do { if (((val_t)(val)&0x100)!=0) { SETFLAG(SR_CARRY, 1) } else { SETFLAG(SR_CARRY, 0) } } while(0)


class mmu;
class mos6560_6561;
class mos6526;


/**
 * @brief MOS 6510 microprocessor
 */
class mos6510
{
  protected:
    const char * opcodenames[0x100] = { /* For debug logging */
      "BRK impl", "ORA X,ind", "JAM", "SLO X,ind", "NOP zpg", "ORA zpg", "ASL zpg", "SLO zpg", "PHP impl", "ORA #", "ASL A", "ANC #", "NOP abs", "ORA abs", "ASL abs", "SLO abs",
      "BPL rel", "ORA ind,Y", "JAM", "SLO ind,Y", "NOP zpg,X", "ORA zpg,X", "ASL zpg,X", "SLO zpg,X", "CLC impl", "ORA abs,Y", "NOP impl", "SLO abs,Y", "NOP abs,X", "ORA abs,X", "ASL abs,X", "SLO abs,X",
      "JSR abs", "AND X,ind", "JAM", "RLA X,ind", "BIT zpg", "AND zpg", "ROL zpg", "RLA zpg", "PLP impl", "AND #", "ROL A", "ANC #", "BIT abs", "AND abs", "ROL abs", "RLA abs",
      "BMI rel", "AND ind,Y", "JAM", "RLA ind,Y", "NOP zpg,X", "AND zpg,X", "ROL zpg,X", "RLA zpg,X", "SEC impl", "AND abs,Y", "NOP impl", "RLA abs,Y", "NOP abs,X", "AND abs,X", "ROL abs,X", "RLA abs,X",
      "RTI impl", "EOR X,ind", "JAM", "SRE X,ind", "NOP zpg", "EOR zpg", "LSR zpg", "SRE zpg", "PHA impl", "EOR #", "LSR A", "ALR #", "JMP abs", "EOR abs", "LSR abs", "SRE abs",
      "BVC rel", "EOR ind,Y", "JAM", "SRE ind,Y", "NOP zpg,X", "EOR zpg,X", "LSR zpg,X", "SRE zpg,X", "CLI impl", "EOR abs,Y", "NOP impl", "SRE abs,Y", "NOP abs,X", "EOR abs,X", "LSR abs,X", "SRE abs,X",
      "RTS impl", "ADC X,ind", "JAM", "RRA X,ind", "NOP zpg", "ADC zpg", "ROR zpg", "RRA zpg", "PLA impl", "ADC #", "ROR A", "ARR #", "JMP ind", "ADC abs", "ROR abs", "RRA abs",
      "BVS rel", "ADC ind,Y", "JAM", "RRA ind,Y", "NOP zpg,X", "ADC zpg,X", "ROR zpg,X", "RRA zpg,X", "SEI impl", "ADC abs,Y", "NOP impl", "RRA abs,Y", "NOP abs,X", "ADC abs,X", "ROR abs,X", "RRA abs,X",
      "NOP #", "STA X,ind", "NOP #", "SAX X,ind", "STY zpg", "STA zpg", "STX zpg", "SAX zpg", "DEY impl", "NOP #", "TXA impl", "ANE #", "STY abs", "STA abs", "STX abs", "SAX abs",
      "BCC rel", "STA ind,Y", "JAM", "SHA ind,Y", "STY zpg,X", "STA zpg,X", "STX zpg,Y", "SAX zpg,Y", "TYA impl", "STA abs,Y", "TXS impl", "TAS abs,Y", "SHY abs,X", "STA abs,X", "SHX abs,Y", "SHA abs,Y",
      "LDY #", "LDA X,ind", "LDX #", "LAX X,ind", "LDY zpg", "LDA zpg", "LDX zpg", "LAX zpg", "TAY impl", "LDA #", "TAX impl", "LXA #", "LDY abs", "LDA abs", "LDX abs", "LAX abs",
      "BCS rel", "LDA ind,Y", "JAM", "LAX ind,Y", "LDY zpg,X", "LDA zpg,X", "LDX zpg,Y", "LAX zpg,Y", "CLV impl", "LDA abs,Y", "TSX impl", "LAS abs,Y", "LDY abs,X", "LDA abs,X", "LDX abs,Y", "LAX abs,Y",
      "CPY #", "CMP X,ind", "NOP #", "DCP X,ind", "CPY zpg", "CMP zpg", "DEC zpg", "DCP zpg", "INY impl", "CMP #", "DEX impl", "SBX #", "CPY abs", "CMP abs", "DEC abs", "DCP abs",
      "BNE rel", "CMP ind,Y", "JAM", "DCP ind,Y", "NOP zpg,X", "CMP zpg,X", "DEC zpg,X", "DCP zpg,X", "CLD impl", "CMP abs,Y", "NOP impl", "DCP abs,Y", "NOP abs,X", "CMP abs,X", "DEC abs,X", "DCP abs,X",
      "CPX #", "SBC X,ind", "NOP #", "ISC X,ind", "CPX zpg", "SBC zpg", "INC zpg", "ISC zpg", "INX impl", "SBC #", "NOP impl", "USBC #", "CPX abs", "SBC abs", "INC abs", "ISC abs",
      "BEQ rel", "SBC ind,Y", "JAM", "ISC ind,Y", "NOP zpg,X", "SBC zpg,X", "INC zpg,X", "ISC zpg,X", "SED impl", "SBC abs,Y", "NOP impl", "ISC abs,Y", "NOP abs,X", "SBC abs,X", "INC abs,X", "ISC abs, X",
    };

    /* Define the type for the lookup table entries
       Force all entries to a uniform call signature: void(),
       binding any necessary parameters internally. */
    using InstructionHandler = std::function<void()>;

    /* The lookup table for 256 opcodes */
    std::array<InstructionHandler, 256> instruction_table;
  private: /* TODO: Group instructions by memory type */
    /* Glue */
    mmu * mmu_;
    mos6560_6561 * vic;
    mos6526 * cia1;
    mos6526 * cia2;

    /* registers */
    addr_t pc_;
    val_t sp_, a_, x_, y_;

    /* flags (p/status reg) */
    /*   nf, of, -, bcf, dmf, idf, zf, cf */
    /* 0b 1   1  1    1    1    1   1   1 */
    val_t _flags = 0b11111111;

    /* c64->memory and clock */
    static CPUCLOCK cycles_;
    static CPUCLOCK prev_cycles_;

    /* helpers */
    addr_t curr_page; /* current page at start of cpu emulation */
    bool pb_crossed;    /* true if page boundary crossed */

    inline void save_byte(addr_t addr, val_t val);
    inline val_t load_byte(addr_t addr);
    inline addr_t load_word(addr_t addr);
    inline void push(val_t v);
    inline val_t pop();
    inline val_t fetch_op();
    inline addr_t fetch_opw();
    inline addr_t addr_zero();
    inline addr_t addr_zerox();
    inline addr_t addr_zeroy();
    inline addr_t addr_abs();
    inline addr_t addr_absy();
    inline addr_t addr_absx();
    inline addr_t addr_indx();
    inline addr_t addr_indy();

    inline val_t rol(val_t v);
    inline val_t ror(val_t v);
    inline val_t lsr(val_t v);
    inline val_t asl(val_t v);
    /* instructions : data handling and memory operations */
    inline void sta(addr_t addr, cycle_t cycles);
    inline void stx(addr_t addr, cycle_t cycles);
    inline void sty(addr_t addr, cycle_t cycles);
    inline void lda(val_t v, cycle_t cycles);
    inline void ldx(val_t v, cycle_t cycles);
    inline void ldy(val_t v, cycle_t cycles);
    inline void txs();
    inline void tsx();
    inline void tax();
    inline void txa();
    inline void tay();
    inline void tya();
    inline void pha();
    inline void pla();
    /* instructions: logic operations */
    inline void ora(val_t v, cycle_t cycles);
    inline void _and(val_t v, cycle_t cycles);
    inline void bit(addr_t addr, cycle_t cycles);
    inline void rol_a();
    inline void rol_mem(addr_t addr, cycle_t cycles);
    inline void ror_a();
    inline void ror_mem(addr_t addr, cycle_t cycles);
    inline void asl_a();
    inline void asl_mem(addr_t addr, cycle_t cycles);
    inline void lsr_a();
    inline void lsr_mem(addr_t addr, cycle_t cycles);
    inline void eor(val_t v, cycle_t cycles);
    /* instructions: arithmetic operations */
    inline void inc(addr_t addr, cycle_t cycles);
    inline void dec(addr_t addr, cycle_t cycles);
    inline void inx();
    inline void iny();
    inline void dex();
    inline void dey();
    inline void adc(val_t v, cycle_t cycles);
    inline void sbc(val_t v, cycle_t cycles);
    /* instructions: flag access */
    inline void sei();
    inline void cli();
    inline void sec();
    inline void clc();
    inline void sed();
    inline void cld();
    inline void clv();
    inline void php();
    inline void plp();
    /* instructions: control flow */
    inline void cmp(val_t v, cycle_t cycles);
    inline void cpx(val_t v, cycle_t cycles);
    inline void cpy(val_t v, cycle_t cycles);
    inline void rts();
    inline void jsr();
    inline void bne();
    inline void beq();
    inline void bcs();
    inline void bcc();
    inline void bpl();
    inline void bmi();
    inline void bvc();
    inline void bvs();
    inline void jmp();
    inline void jmp_ind();
    /* instructions: misc */
    inline void nop(cycle_t cycles);
    inline void brk();
    inline void rti();
    /* Instructions: illegal */
    inline void jam(val_t insn);
    inline void slo(addr_t addr, cycle_t cycles_a, cycle_t cycles_b);
    inline void lxa(val_t v, cycle_t cycles);
    inline void anc(val_t v);
    inline void las(val_t v);
    inline void lax(val_t v, cycle_t cycles);
    inline void shy(addr_t addr, cycle_t cycles);
    inline void shx(addr_t addr, cycle_t cycles);
    inline void sha(addr_t addr, cycle_t cycles);
    inline void sax(addr_t addr, cycle_t cycles);
    inline void sre(addr_t addr, cycle_t cycles_a, cycle_t cycles_b);
    inline void rla(addr_t addr, cycle_t cycles_a, cycle_t cycles_b);
    inline void rla_(addr_t addr, cycle_t cycles_a, cycle_t cycles_b);
    inline void rra(addr_t addr, cycle_t cycles_a, cycle_t cycles_b);
    inline void dcp(addr_t addr, cycle_t cycles_a, cycle_t cycles_b);
    inline void tas(addr_t addr, cycle_t cycles);
    inline void sbx(val_t v, cycle_t cycles);
    inline void isc(addr_t addr, cycle_t cycles);
    inline void arr();
    inline void xaa(val_t v);

    typedef void (*BusWrite)(addr_t, val_t);
    typedef val_t (*BusRead)(addr_t);
    typedef void (*ClockCycle)(mos6510*);

    bool vic_stall_cpu_ = false;

    CPUCLOCK vic_rstr_irq_callback  = 0;
    CPUCLOCK cia1_tima_irq_callback = 0;
    CPUCLOCK cia1_timb_irq_callback = 0;
    CPUCLOCK cia2_tima_nmi_callback = 0;
    CPUCLOCK cia2_timb_nmi_callback = 0;
  public:
    mos6510(BusRead r, BusWrite w);
    ~mos6510(void);
    void set_cycle_callback(ClockCycle c);
    void set_irq_nmi_callback(CPUCLOCK c, int src); /* ISSUE: Does _NOT_ work, do _NOT_ use */

    void glue_c64(mmu *_mmu, mos6560_6561 *_vic, mos6526 *_cia1, mos6526 *_cia2);

    /* Write / Read / Cycle callbacks */
    BusRead read_bus;
    BusWrite write_bus;
    ClockCycle clock_cycle = nullptr;
    /* https://stackoverflow.com/questions/16418242/checking-whether-callback-is-set-by-the-client-in-c */
    bool check_callback(void) { return (clock_cycle != nullptr); };

    /* Static variables */
    static bool pending_interrupt;
    static bool irq_pending;
    static bool nmi_pending;

    /* cpu state */
    void reset(void);
    bool emulate(void);
    bool emulate_n(tick_t n_cycles);
    inline void stall_cpu(bool stall) { vic_stall_cpu_ = stall; };

    /* register access */
    inline addr_t pc() { return pc_; };
    inline void pc(addr_t v) { pc_=v; pc_address = pc_; };
    inline val_t sp() { return sp_; };
    inline void sp(val_t v) { sp_=v; };
    inline val_t a() { return a_; };
    inline void a(val_t v) { a_=v; };
    inline val_t x() { return x_; };
    inline void x(val_t v) { x_=v; };
    inline val_t y() { return y_; };
    inline void y(val_t v) { y_=v; };

    /*   nf, of, -, bcf, dmf, idf, zf, cf */
    /* 0b 1   1  1    1    1    1   1   1 */
    inline bool cf(void)    { return GETFLAG(SR_CARRY); }; /* cf_ */
    inline void cf(bool v)  { SETFLAG(SR_CARRY, v); }; /* cf_=v */
    inline bool zf(void)    { return GETFLAG(SR_ZERO); }; /* zf_ */
    inline void zf(bool v)  { SETFLAG(SR_ZERO, v); }; /* zf_=v */
    inline bool idf(void)   { return GETFLAG(SR_INTERRUPT); }; /* idf_ */
    inline void idf(bool v) { SETFLAG(SR_INTERRUPT, v); }; /* idf_=v */
    inline bool dmf(void)   { return GETFLAG(SR_DECIMAL); }; /* dmf_ */
    inline void dmf(bool v) { SETFLAG(SR_DECIMAL, v); }; /* dmf_=v */
    inline bool bcf(void)   { return GETFLAG(SR_BREAK); }; /* bcf_ */
    inline void bcf(bool v) { SETFLAG(SR_BREAK, v); }; /* bcf_=v */
    inline bool of(void)    { return GETFLAG(SR_OVERFLOW); }; /* of_ */
    inline void of(bool v)  { SETFLAG(SR_OVERFLOW, v); }; /* of_=v */
    inline bool nf(void)    { return GETFLAG(SR_NEGATIVE); }; /* nf_ */
    inline void nf(bool v)  { SETFLAG(SR_NEGATIVE, v); }; /* nf_=v */

    /* clock */
    CPUCLOCK cycles(){return cycles_;};
    void cycles(CPUCLOCK v){cycles_=v;};
    void tickle_me(cycle_t v);
  private:
    bool initialized = false;
    void fill_instruction_table(void);
    void execute(uint8_t opcode);

    inline void tick_backup(cycle_t v);
    inline void tick(cycle_t v);
    inline void backtick(cycle_t v);

    inline val_t flags();
    inline void flags(val_t v);

    inline void handle_interrupts(void);

  public:
    void nmi_(val_t source);
    void irq_(val_t source);
    void nmi(val_t source);
    void irq(val_t source);
    void process_interrupts(void);

    /* debug */
    static bool loginstructions;
    static val_t last_insn;
    static addr_t pc_address; /* debug printing helper */
    static addr_t d_address;  /* debug printing helper */
    void dump_flags();
    void dump_flags(val_t flags);
    void dump_regs();
    void dump_regs_insn(val_t insn);
    void dump_regs_irq(val_t type, val_t source);
    void dump_regs_json();
    void dbg();
    void dbg_a();
    void dbg_b();

};


#endif /* _MOS6510_H */
