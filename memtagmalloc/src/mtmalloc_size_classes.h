// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef __MTMALLOC_SIZE_CLASSES_H__
#define __MTMALLOC_SIZE_CLASSES_H__

#include <stdint.h>
#include <stddef.h>

namespace MTMalloc {

// All size classes are 0 mod 16.
// Contains all multiples of 16 from 16 to 256.
// All size classes satisfy IsCorrectDivToMul.
constexpr size_t SCArray[] = {
    1 * 16, 2 * 16,  3 * 16,  4 * 16,  5 * 16,  6 * 16,  7 * 16,  8 * 16,
    9 * 16, 10 * 16, 11 * 16, 12 * 16, 13 * 16, 14 * 16, 15 * 16, 16 * 16,
    272,    288,     336,     368,     448,     480,     512,     576,
    640,    704,     768,     896,     1024,    1152,    1280,    1408,
    1536,   1792,    2048,    2304,    2688,    2816,    3200,    3456,
    3584,   4096,    4736,    5376,    6144,    6528,    7168,    8192,
    9216,   10240,   12288,   14336,   16384,   20480,   24576,   28672,
    32768,  40960,   49152,   57344,   65536,   73728,   81920,   98304,
    131072,  172032,  262144,
};

static constexpr size_t kNumSizeClasses = sizeof(SCArray) / sizeof(SCArray[0]);
static constexpr size_t kMaxSizeClass = SCArray[kNumSizeClasses - 1];

}  // namespace MTMalloc

#endif  // __MTMALLOC_SIZE_CLASSES_H__
