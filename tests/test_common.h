/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * test_common.h
 * Minimal assert helpers. No external test framework on purpose, so the same
 * test sources can be built for the embedded target later.
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
#ifndef _US_TEST_COMMON_H_
#define _US_TEST_COMMON_H_

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

/* Per translation unit failure counter */
static int us_test_failures = 0;
static int us_test_checks   = 0;

/**
 * @brief What listing a directory found.
 *
 * The distinction matters: a machine that simply has no tune collection must
 * still be able to run the suite, and a collection that is there but yields
 * nothing is a broken harness pretending to pass.
 */
enum class UsDir {
  Missing,  /**< no such directory: this machine has no collection */
  Empty,    /**< the directory is there and has nothing matching in it */
  Listed,   /**< files, sorted */
};

/**
 * @brief Every file in `dir` ending in `ext`, sorted.
 *
 * `std::filesystem` and not `popen("ls ...")`, which is what the two sweeps used
 * to do. Three runs in a row reported 160, then 62, then 0 tunes while every
 * file was readable from a plain program: a pipe to `ls` can fail, be truncated
 * or be cut short by the shell without any of it being visible to the caller,
 * and the sweeps then reported "skipped" and **passed**. A regression net that
 * quietly tests nothing is worse than no net.
 *
 * Sorted, so a failure names the same tune on every machine and a bisect can be
 * repeated.
 *
 * @param dir  directory to read
 * @param ext  extension to keep, with the dot, compared case insensitively
 * @param out  appended to, not cleared
 */
static UsDir us_list_dir(const char * dir, const char * ext,
                         std::vector<std::string> & out)
{
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return UsDir::Missing;

  const size_t before = out.size();
  const std::string want(ext);
  for (const auto & entry : fs::directory_iterator(dir, ec)) {
    if (ec) break;
    if (!entry.is_regular_file(ec)) continue;
    std::string name = entry.path().filename().string();
    if (name.size() <= want.size()) continue;
    std::string tail = name.substr(name.size() - want.size());
    std::transform(tail.begin(), tail.end(), tail.begin(),
                   [](unsigned char c) { return static_cast<char>(tolower(c)); });
    if (tail != want) continue;
    out.push_back(entry.path().string());
  }
  std::sort(out.begin() + static_cast<long>(before), out.end());
  return (out.size() == before) ? UsDir::Empty : UsDir::Listed;
}

#define US_CHECK(cond, ...)                                     \
  do {                                                          \
    ++us_test_checks;                                           \
    if (!(cond)) {                                              \
      ++us_test_failures;                                       \
      printf("  FAIL %s:%d: ", __FILE__, __LINE__);             \
      printf(__VA_ARGS__);                                      \
      printf("\n");                                             \
    }                                                           \
  } while (0)

#define US_CHECK_EQ_U(got, want, what)                          \
  do {                                                          \
    ++us_test_checks;                                           \
    unsigned long long g_ = (unsigned long long)(got);          \
    unsigned long long w_ = (unsigned long long)(want);         \
    if (g_ != w_) {                                             \
      ++us_test_failures;                                       \
      printf("  FAIL %s:%d: %s got %llu want %llu\n",           \
             __FILE__, __LINE__, (what), g_, w_);               \
    }                                                           \
  } while (0)

#define US_CHECK_EQ_STR(got, want, what)                        \
  do {                                                          \
    ++us_test_checks;                                           \
    if (strcmp((got), (want)) != 0) {                           \
      ++us_test_failures;                                       \
      printf("  FAIL %s:%d: %s got \"%s\" want \"%s\"\n",        \
             __FILE__, __LINE__, (what), (got), (want));        \
    }                                                           \
  } while (0)

#define US_TEST_BEGIN(name) printf("[TEST] %s\n", (name))

#define US_TEST_END(name)                                       \
  do {                                                          \
    printf("[%s] %s (%d checks, %d failures)\n",                \
           (us_test_failures == 0 ? " OK " : "FAIL"),           \
           (name), us_test_checks, us_test_failures);           \
    return us_test_failures;                                    \
  } while (0)

#endif /* _US_TEST_COMMON_H_ */
