/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * test_bus.cpp
 * Step 2.1 gate: the bus ticks every device exactly once per PHI2 cycle, in a
 * fixed order, and carries the IRQ, NMI and BA lines.
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

#include <string>

#include "core/bus.h"
#include "test_common.h"
#include "tests.h"

using namespace usbsid;

namespace {

/* A device that appends its id to a shared log on every tick */
class FakeDevice : public ClockedDevice
{
  public:
    /* tick_id lands in the log on tick, reset_id on reset */
    FakeDevice(char tick_id, char reset_id, std::string * log)
      : tick_id_(tick_id), reset_id_(reset_id), log_(log) {}

    void tick(void) override { log_->push_back(tick_id_); ++ticks; }
    void reset(void) override { log_->push_back(reset_id_); ++resets; }

    unsigned ticks  = 0;
    unsigned resets = 0;

  protected:
    char tick_id_;
    char reset_id_;
    std::string * log_;
};

/* A CIA-like device that asserts its IRQ source during its own tick */
class FakeIrqDevice : public FakeDevice
{
  public:
    FakeIrqDevice(char tick_id, char reset_id, std::string * log,
                  Bus * bus, IrqSource src)
      : FakeDevice(tick_id, reset_id, log), bus_(bus), src_(src) {}

    void tick(void) override
    {
      FakeDevice::tick();
      if (assert_at_tick != 0 && ticks == assert_at_tick) {
        bus_->set_irq(src_, true);
      }
      if (release_at_tick != 0 && ticks == release_at_tick) {
        bus_->set_irq(src_, false);
      }
    }

    unsigned assert_at_tick  = 0;
    unsigned release_at_tick = 0;

  private:
    Bus * bus_;
    IrqSource src_;
};

/* A CPU that records the line levels it sees during each of its own cycles.
 * The real CPU reads the lines the same way, off the bus, which is why the
 * fake does not implement any setters. */
class FakeCpu : public ClockedDevice
{
  public:
    FakeCpu(std::string * log, Bus * bus) : log_(log), bus_(bus) {}

    void tick(void) override
    {
      log_->push_back('C');
      ++ticks;
      /* stored as '0'/'1' so a failing history prints readably */
      irq_history.push_back(bus_->irq_asserted() ? '1' : '0');
      nmi_history.push_back(bus_->nmi_asserted() ? '1' : '0');
      ba_history.push_back(bus_->ba() ? '1' : '0');
    }
    void reset(void) override { log_->push_back('c'); ++resets; }

    unsigned ticks  = 0;
    unsigned resets = 0;
    std::string irq_history;
    std::string nmi_history;
    std::string ba_history;

  private:
    std::string * log_;
    Bus * bus_;
};

} /* namespace */

int us_test_bus(void)
{
  US_TEST_BEGIN("core/bus");

  /* ---- tick order and cycle counter ---------------------------------- */
  {
    std::string log;
    Bus bus;
    FakeDevice vic('V', 'v', &log);
    FakeDevice cia1('1', 'x', &log);
    FakeDevice cia2('2', 'y', &log);
    FakeCpu cpu(&log, &bus);

    bus.attach_vic(&vic);
    bus.attach_cia1(&cia1);
    bus.attach_cia2(&cia2);
    bus.attach_cpu(&cpu);

    US_CHECK_EQ_U(bus.cycles(), 0u, "cycle counter starts at 0");

    bus.run(3);

    US_CHECK_EQ_STR(log.c_str(), "V12CV12CV12C", "tick order over 3 cycles");
    US_CHECK_EQ_U(bus.cycles(), 3u, "cycle counter after run(3)");
    US_CHECK_EQ_U(vic.ticks, 3u, "vic ticks");
    US_CHECK_EQ_U(cia1.ticks, 3u, "cia1 ticks");
    US_CHECK_EQ_U(cia2.ticks, 3u, "cia2 ticks");
    US_CHECK_EQ_U(cpu.ticks, 3u, "cpu ticks");

    /* one tick is exactly one cycle, no catch up */
    bus.tick();
    US_CHECK_EQ_U(bus.cycles(), 4u, "single tick advances 1 cycle");
    US_CHECK_EQ_U(vic.ticks, 4u, "vic advanced by exactly 1");
  }

  /* ---- empty slots ---------------------------------------------------- */
  {
    Bus bus;
    bus.run(10);
    US_CHECK_EQ_U(bus.cycles(), 10u, "bus with no devices still keeps time");

    std::string log;
    FakeCpu cpu(&log, &bus);
    bus.attach_cpu(&cpu);
    bus.run(2);
    US_CHECK_EQ_STR(log.c_str(), "CC", "cpu only machine ticks the cpu");
    US_CHECK_EQ_U(bus.cycles(), 12u, "cycle counter keeps running");
  }

  /* ---- reset ---------------------------------------------------------- */
  {
    std::string log;
    Bus bus;
    FakeDevice vic('V', 'v', &log);
    FakeDevice cia1('1', 'x', &log);
    FakeDevice cia2('2', 'y', &log);
    FakeCpu cpu(&log, &bus);
    bus.attach_vic(&vic);
    bus.attach_cia1(&cia1);
    bus.attach_cia2(&cia2);
    bus.attach_cpu(&cpu);

    bus.run(5);
    bus.set_irq(IrqSource::Vic, true);
    bus.set_nmi(NmiSource::Cia2, true);
    bus.set_ba(false);

    log.clear();
    bus.reset();

    US_CHECK_EQ_STR(log.c_str(), "vxyc", "reset order matches tick order");
    US_CHECK_EQ_U(bus.cycles(), 0u, "reset clears the cycle counter");
    US_CHECK(bus.irq_asserted() == false, "reset releases irq");
    US_CHECK(bus.nmi_asserted() == false, "reset releases nmi");
    US_CHECK(bus.ba() == true, "reset returns ba high");
    US_CHECK_EQ_U(vic.resets, 1u, "vic reset once");
    US_CHECK_EQ_U(cpu.resets, 1u, "cpu reset once");
  }

  /* ---- wired-or interrupt lines --------------------------------------- */
  {
    Bus bus;
    US_CHECK(bus.irq_asserted() == false, "irq starts released");

    bus.set_irq(IrqSource::Cia1, true);
    US_CHECK(bus.irq_asserted() == true, "cia1 asserts irq");
    bus.set_irq(IrqSource::Vic, true);
    US_CHECK_EQ_U(bus.irq_sources(), 0x03u, "two irq sources set");

    bus.set_irq(IrqSource::Cia1, false);
    US_CHECK(bus.irq_asserted() == true, "irq stays low while vic holds it");
    bus.set_irq(IrqSource::Vic, false);
    US_CHECK(bus.irq_asserted() == false, "irq released when last source clears");

    /* releasing a source that was never set must not disturb the others */
    bus.set_irq(IrqSource::Cia2, true);
    bus.set_irq(IrqSource::Expansion, false);
    US_CHECK(bus.irq_asserted() == true, "unrelated release does not clear irq");

    bus.set_nmi(NmiSource::Cia2, true);
    bus.set_nmi(NmiSource::Restore, true);
    US_CHECK_EQ_U(bus.nmi_sources(), 0x03u, "two nmi sources set");
    bus.set_nmi(NmiSource::Cia2, false);
    US_CHECK(bus.nmi_asserted() == true, "nmi held by restore");
    bus.set_nmi(NmiSource::Restore, false);
    US_CHECK(bus.nmi_asserted() == false, "nmi released");
  }

  /* ---- line levels reach the cpu in the same cycle --------------------- */
  {
    std::string log;
    Bus bus;
    FakeIrqDevice cia1('1', 'x', &log, &bus, IrqSource::Cia1);
    FakeCpu cpu(&log, &bus);
    bus.attach_cia1(&cia1);
    bus.attach_cpu(&cpu);

    cia1.assert_at_tick  = 2; /* asserts during cycle 2 */
    cia1.release_at_tick = 4; /* releases during cycle 4 */

    bus.run(5);

    /* cycle:            1    2    3    4    5
     * cia1 action:      -    set  -    clr  -
     * cpu sees irq:     0    1    1    0    0
     * The CIA ticks before the CPU in the same cycle, so an interrupt raised
     * in cycle N is visible to the CPU in cycle N. The extra sampling delay
     * of a real 6510 belongs in the CPU, not here. */
    US_CHECK_EQ_STR(cpu.irq_history.c_str(), "01100", "irq level per cycle");
  }

  /* ---- BA is handed to the cpu every cycle ---------------------------- */
  {
    std::string log;
    Bus bus;
    FakeCpu cpu(&log, &bus);
    bus.attach_cpu(&cpu);

    bus.tick();          /* ba high */
    bus.set_ba(false);
    bus.tick();          /* ba low */
    bus.tick();          /* still low */
    bus.set_ba(true);
    bus.tick();          /* high again */

    US_CHECK_EQ_STR(cpu.ba_history.c_str(), "1001", "ba level per cycle");
    US_CHECK_EQ_STR(cpu.nmi_history.c_str(), "0000", "nmi stayed released");
  }

  US_TEST_END("core/bus");
}

US_TEST_MAIN(us_test_bus)
