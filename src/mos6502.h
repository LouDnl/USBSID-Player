//============================================================================
// Name        : mos6502
// Author      : Gianluca Ghettini
// Additions   : LouD
// Version     : 1.1
// Copyright   :
// Description : A MOS 6502 CPU emulator written in C++
//============================================================================

#pragma once
#include <stdint.h>
#include <stdio.h>
#include <sstream>

class mos6502
{
private:
	const char * opcodenames[0x100] = { /* Added for debug logging */
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
	// register reset values
	uint8_t reset_A;
	uint8_t reset_X;
	uint8_t reset_Y;
	uint8_t reset_sp;
	uint8_t reset_status;

	// registers
	uint8_t A; // accumulator
	uint8_t X; // X-index
	uint8_t Y; // Y-index

	// stack pointer
	uint8_t sp;

	// program counter
	uint16_t pc;

	// status register
	uint8_t status;

	typedef void (mos6502::*CodeExec)(uint16_t);
	typedef uint16_t (mos6502::*AddrExec)();

	struct Instr
	{
		AddrExec addr;
		CodeExec code;
		uint8_t cycles;
	};

	static Instr InstrTable[256];

	void Exec(Instr i);

	bool illegalOpcode;

	// addressing modes
	uint16_t Addr_ACC(); // ACCUMULATOR
	uint16_t Addr_IMM(); // IMMEDIATE
	uint16_t Addr_ABS(); // ABSOLUTE
	uint16_t Addr_ZER(); // ZERO PAGE
	uint16_t Addr_ZEX(); // INDEXED-X ZERO PAGE
	uint16_t Addr_ZEY(); // INDEXED-Y ZERO PAGE
	uint16_t Addr_ABX(); // INDEXED-X ABSOLUTE
	uint16_t Addr_ABY(); // INDEXED-Y ABSOLUTE
	uint16_t Addr_IMP(); // IMPLIED
	uint16_t Addr_REL(); // RELATIVE
	uint16_t Addr_INX(); // INDEXED-X INDIRECT
	uint16_t Addr_INY(); // INDEXED-Y INDIRECT
	uint16_t Addr_ABI(); // ABSOLUTE INDIRECT

	// opcodes (grouped as per datasheet)
	void Op_ADC(uint16_t src);
	void Op_AND(uint16_t src);
	void Op_ASL(uint16_t src); 	void Op_ASL_ACC(uint16_t src);
	void Op_BCC(uint16_t src);
	void Op_BCS(uint16_t src);

	void Op_BEQ(uint16_t src);
	void Op_BIT(uint16_t src);
	void Op_BMI(uint16_t src);
	void Op_BNE(uint16_t src);
	void Op_BPL(uint16_t src);

	void Op_BRK(uint16_t src);
	void Op_BVC(uint16_t src);
	void Op_BVS(uint16_t src);
	void Op_CLC(uint16_t src);
	void Op_CLD(uint16_t src);

	void Op_CLI(uint16_t src);
	void Op_CLV(uint16_t src);
	void Op_CMP(uint16_t src);
	void Op_CPX(uint16_t src);
	void Op_CPY(uint16_t src);

	void Op_DEC(uint16_t src);
	void Op_DEX(uint16_t src);
	void Op_DEY(uint16_t src);
	void Op_EOR(uint16_t src);
	void Op_INC(uint16_t src);

	void Op_INX(uint16_t src);
	void Op_INY(uint16_t src);
	void Op_JMP(uint16_t src);
	void Op_JSR(uint16_t src);
	void Op_LDA(uint16_t src);

	void Op_LDX(uint16_t src);
	void Op_LDY(uint16_t src);
	void Op_LSR(uint16_t src); 	void Op_LSR_ACC(uint16_t src);
	void Op_NOP(uint16_t src);
	void Op_ORA(uint16_t src);

	void Op_PHA(uint16_t src);
	void Op_PHP(uint16_t src);
	void Op_PLA(uint16_t src);
	void Op_PLP(uint16_t src);
	void Op_ROL(uint16_t src); 	void Op_ROL_ACC(uint16_t src);

	void Op_ROR(uint16_t src);	void Op_ROR_ACC(uint16_t src);
	void Op_RTI(uint16_t src);
	void Op_RTS(uint16_t src);
	void Op_SBC(uint16_t src);
	void Op_SEC(uint16_t src);
	void Op_SED(uint16_t src);

	void Op_SEI(uint16_t src);
	void Op_STA(uint16_t src);
	void Op_STX(uint16_t src);
	void Op_STY(uint16_t src);
	void Op_TAX(uint16_t src);

	void Op_TAY(uint16_t src);
	void Op_TSX(uint16_t src);
	void Op_TXA(uint16_t src);
	void Op_TXS(uint16_t src);
	void Op_TYA(uint16_t src);

	void Op_ILLEGAL(uint16_t src);

	// IRQ, reset, NMI vectors
	static const uint16_t irqVectorH = 0xFFFF;
	static const uint16_t irqVectorL = 0xFFFE;
	static const uint16_t rstVectorH = 0xFFFD;
	static const uint16_t rstVectorL = 0xFFFC;
	static const uint16_t nmiVectorH = 0xFFFB;
	static const uint16_t nmiVectorL = 0xFFFA;

	// read/write/clock-cycle callbacks
	typedef void (*BusWrite)(uint16_t, uint8_t);
	typedef uint8_t (*BusRead)(uint16_t);
	typedef void (*ClockCycle)(mos6502*);
	BusRead Read;
	BusWrite Write;
	ClockCycle Cycle;

	// stack operations
	inline void StackPush(uint8_t byte);
	inline uint8_t StackPop();

public:
	enum CycleMethod {
		INST_COUNT,
		CYCLE_COUNT,
	};
	mos6502(BusRead r, BusWrite w, ClockCycle c = nullptr);
	void NMI();
	void IRQ();
	void Reset();
	void Run(
		int32_t cycles,
		uint64_t& cycleCount,
		CycleMethod cycleMethod = CYCLE_COUNT);
	void RunEternally(); // until it encounters a illegal opcode
						 // useful when running e.g. WOZ Monitor
						 // no need to worry about cycle exhaus-
						 // tion
	void RunN(uint32_t n, uint64_t& cycleCount);
    uint16_t GetPC();
    uint8_t GetS();
    uint8_t GetP();
    uint8_t GetA();
    uint8_t GetX();
    uint8_t GetY();
    void SetResetS(uint8_t value);
    void SetResetP(uint8_t value);
    void SetResetA(uint8_t value);
    void SetResetX(uint8_t value);
    void SetResetY(uint8_t value);
    uint8_t GetResetS();
    uint8_t GetResetP();
    uint8_t GetResetA();
    uint8_t GetResetX();
    uint8_t GetResetY();

		static unsigned short d_address; /* debug printing helper */
		static bool loginstructions;
		void dump_regs();
		void dump_regs_insn(uint8_t insn, uint8_t cycles);
};
