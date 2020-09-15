// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef __MTMALLOC_SHADOW__
#define __MTMALLOC_SHADOW__

#include <sys/mman.h>

#include <cstdint>

namespace MTMalloc {
template <uintptr_t kShadowBeg, uintptr_t kBeg, uintptr_t kSize,
          uintptr_t kGranularity>
struct FixedShadow {
  static const uintptr_t kShadowSize = kSize / kGranularity;
  static void Init() {
    void *Res =
        mmap((void *)kShadowBeg, kShadowSize, PROT_READ | PROT_WRITE,
             MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    if (Res != (void *)kShadowBeg) __builtin_trap();
  }
  static bool IsMine(uintptr_t Val) {
    return Val >= kBeg && Val < kBeg + kSize;
  }
  static uint8_t Get(uintptr_t Val) { return *GetShadowPtr(Val); }
  static void Set(uintptr_t Val, uint8_t Shadow) {
    Check(Val);
    *GetShadowPtr(Val) = Shadow;
    reinterpret_cast<uint8_t *>(kShadowBeg)[(Val - kBeg) / kGranularity] =
        Shadow;
  }
  static void SetRange(uintptr_t Beg, uintptr_t Size, uint8_t ShadowVal) {
    Check(Beg);
    Check(Size);
    uint8_t *ShadowPos = GetShadowPtr(Beg);
    uint8_t *ShadowEnd = ShadowPos + Size / kGranularity;
    for (; ShadowPos < ShadowEnd; ShadowPos++) *ShadowPos = ShadowVal;
  }

  static uint8_t *GetShadowPtr(uintptr_t Val) {
    return reinterpret_cast<uint8_t *>(kShadowBeg +
                                       (Val - kBeg) / kGranularity);
  }
  static void Check(uintptr_t Val) {
    if (Val % kGranularity) __builtin_trap();
  }
};
};

#endif  // __MTMALLOC_SHADOW__

