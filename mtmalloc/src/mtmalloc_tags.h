// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef __MTMALLOC_TAGS_H__
#define __MTMALLOC_TAGS_H__
#include "mtmalloc_config.h"
#include "mtmalloc_shadow.h"
namespace MTMalloc {

template <size_t kAllocatorSpace, size_t kAllocatorSize,
          size_t kSizeAlignmentForSecondRange>
class AddressAndMemoryTags {
 public:
  void Init() {
    LargeShadow.Init();
    SmallShadow.Init();
  }
  void SetMemoryTag(void *Addr, uintptr_t Size, uint8_t Tag) {
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
    if (!Config.UseAliases) return Addr;
    uintptr_t Ptr = reinterpret_cast<uintptr_t>(Addr);
    uintptr_t Tag = AddrTag & 15;
    Tag <<= 40;
    uintptr_t Mask = 15;
    Mask <<= 40;
    // fprintf(stderr, "Ptr %016zx\n", Ptr);
    // fprintf(stderr, "Tag %016zx\n", Tag);
    Addr = reinterpret_cast<uint8_t *>((Ptr & ~Mask) | Tag);
    return Addr;
  }

  uint8_t GetAddressTag(void *Addr) {
    return (reinterpret_cast<uintptr_t>(Addr) >> 40) & 15;
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
