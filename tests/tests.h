/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * tests.h
 * Entry point of every component test. Each test is both a standalone
 * executable (for CTest) and a subcommand of the aggregate usbsid-test binary.
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
#ifndef _US_TESTS_H_
#define _US_TESTS_H_

/* Every entry returns the number of failed checks, 0 means pass */
int us_test_bus(void);
int us_test_ram(void);
int us_test_cpu(void);
int us_test_mmu(void);
int us_test_cia(void);
int us_test_vic(void);
int us_test_sid(void);
int us_test_player(void);
int us_test_embedded(void);
int us_test_prg(void);
int us_test_keyboard(void);
int us_test_web(void);

/* Standalone main, defined by each test source under US_TEST_STANDALONE */
#ifdef US_TEST_STANDALONE
#define US_TEST_MAIN(fn) int main(void) { return (fn)() == 0 ? 0 : 1; }
#else
#define US_TEST_MAIN(fn)
#endif

#endif /* _US_TESTS_H_ */
