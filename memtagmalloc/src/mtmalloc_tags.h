// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef __MTMALLOC_TAGS_H__
#define __MTMALLOC_TAGS_H__
#include "mtmalloc_config.h"
#include "mtmalloc_shadow.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/prctl.h>

#ifdef __aarch64__
static const bool kUseArmTBI = 1;
#else
static const bool kUseArmTBI = 0;
#endif

#ifdef __ARM_FEATURE_MEMORY_TAGGING
#include <arm_acle.h>
inline void EnableSyncMTE() {
  // TODO: find the right definitions for these constants.
  if (0 > prctl(PR_SET_TAGGED_ADDR_CTRL,
                PR_TAGGED_ADDR_ENABLE | 2 | (0xfffe << 3),
                0, 0, 0)) {
    perror("EnableSyncMTE failed");
    exit(1);
  }
}
#else
inline void __arm_mte_set_tag(void *){}
inline void *__arm_mte_get_tag(void *P) { return P; }
inline void EnableSyncMTE() { __builtin_trap(); }
#endif


namespace MTMalloc {

template <size_t kAllocatorSpace, size_t kAllocatorSize,
          size_t kSizeAlignmentForSecondRange>
class AddressAndMemoryTags {
 public:
  void Init() {
    if (Config.UseShadow) {
      LargeShadow.Init();
      SmallShadow.Init();
    } else if (Config.UseMTE) {
      EnableSyncMTE();
    }
  }
  void SetMemoryTag(void *Addr, uintptr_t Size, uint8_t Tag) {

    if (Config.UseMTE) {
      // TODO: use IRG instead of generating Tag in software.
      Addr = ApplyAddressTag(Addr, Tag % 15);
      uintptr_t Ptr = reinterpret_cast<uintptr_t>(Addr);
      assert((Size % 16) == 0 && (Ptr % 16) == 0);
      for (size_t I = 0; I < Size; I += 16)
        __arm_mte_set_tag(reinterpret_cast<void*>(Ptr + I));
      return;
    }

    uintptr_t Ptr = reinterpret_cast<uintptr_t>(Addr);
    if (!Config.UseShadow) return;
    if (SmallShadow.IsMine(Ptr))
      SmallShadow.SetRange(Ptr, Size, Tag);
    else if (LargeShadow.IsMine(Ptr))
      LargeShadow.SetRange(Ptr, Size, Tag);
    else
      __builtin_trap();
  }

  uint8_t GetMemoryTag(void *Addr) {
    if (Config.UseMTE)
      return (reinterpret_cast<uint64_t>(__arm_mte_get_tag(Addr)) >> 56) & 15;
    uintptr_t Ptr = reinterpret_cast<uintptr_t>(Addr);
    if (!Config.UseShadow) return 0;
    if (SmallShadow.IsMine(Ptr))
      return SmallShadow.Get(Ptr);
    else if (LargeShadow.IsMine(Ptr))
      return LargeShadow.Get(Ptr);
    else
      __builtin_trap();
  }

  void *ApplyAddressTag(void *Addr, uint8_t AddrTag) {
    if (kUseArmTBI) {
      uintptr_t Ptr = reinterpret_cast<uintptr_t>(Addr);
      uintptr_t Tag = AddrTag;
      Tag <<= 56;
      uintptr_t Mask = 255;
      Mask <<= 56;
      Addr = reinterpret_cast<uint8_t *>((Ptr & ~Mask) | Tag);
      return Addr;
    } else {
      if (!Config.UseAliases) return Addr;
      uintptr_t Ptr = reinterpret_cast<uintptr_t>(Addr);
      uintptr_t Tag = AddrTag & ((1 << Config.UseAliases) - 1);
      Tag <<= 37;
      uintptr_t Mask = 15;
      Mask <<= 37;
      Addr = reinterpret_cast<uint8_t *>((Ptr & ~Mask) | Tag);
      return Addr;
    }
  }

  uint8_t GetAddressTag(void *Addr) {
    if (kUseArmTBI)
      return (reinterpret_cast<uintptr_t>(Addr) >> 56);
    return (reinterpret_cast<uintptr_t>(Addr) >> 37) &
           ((1 << Config.UseAliases) - 1);
  }

  int ProtMTE() {
    if (Config.UseMTE)
      return 0x20;   // TODO: find a better definition for PROT_MTE.
    return 0;
  }
 private:
  // HWASAN-like Shadow. One with 16-byte granularity, one with 1k granularity.
  static const size_t kSmallMemoryTagSpace = 0x300000000000ULL;
  static const size_t kLargeMemoryTagSpace = 0x400000000000ULL;
  FixedShadow<kSmallMemoryTagSpace, kAllocatorSpace, kAllocatorSize / 2, 16>
      SmallShadow;
  FixedShadow<kLargeMemoryTagSpace, kAllocatorSpace + kAllocatorSize / 2,
              kAllocatorSize / 2, kSizeAlignmentForSecondRange>
      LargeShadow;
};

};  // namespace MTMalloc
#endif  // __MTMALLOC_TAGS_H__
