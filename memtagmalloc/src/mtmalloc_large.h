// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef __MTMCLLOC_LARGE_H
#define __MTMCLLOC_LARGE_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

#include "mtmalloc_config.h"
#include "mtmalloc_util.h"

// Super-simple allocator for large memory regions.
// It need to be enhanced in multiple dimensions.
namespace MTMalloc {
class LargeAllocator {
  static constexpr size_t kCpuPageSize = 1 << 12;
  static constexpr size_t kLeftHeaderMagic =  0x039C823525B0237EULL;
  static constexpr size_t kRightHeaderMagic = 0x1C2C5300098D85ADULL;
 public:

  void *Allocate(size_t Size, size_t Alignment = kCpuPageSize) {
    if (Alignment < kCpuPageSize) Alignment = kCpuPageSize;
    size_t RoundedSize = RoundUpTo(Size, kCpuPageSize);
    size_t SizeWithHeader = RoundedSize + kCpuPageSize;
    size_t SizeWithSlackForAlignment = SizeWithHeader;
    if (Alignment > kCpuPageSize)
      SizeWithSlackForAlignment += Alignment;
    uintptr_t Map = reinterpret_cast<uintptr_t>(
        mmap(0, SizeWithSlackForAlignment, PROT_READ | PROT_WRITE,
             MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0));
    uintptr_t EndMap = Map + SizeWithSlackForAlignment;
    if (Map == -1) __builtin_trap();
    uintptr_t Ret = RoundUpTo(Map + 1, Alignment);
    uintptr_t End = Ret + RoundedSize;
    assert(Ret > Map);
    assert(End <= EndMap);
    uintptr_t Hdr = Ret - kCpuPageSize;
    assert(Hdr >= Map);
    if (Map < Hdr)  // deallocate left slack.
      munmap(reinterpret_cast<void*>(Map), Hdr - Map);
    if (End < EndMap)  // deallocate right slack.
      munmap(reinterpret_cast<void*>(End), EndMap - End);
    uintptr_t *Header = reinterpret_cast<uintptr_t*>(Hdr);
    if (Config.LargeAllocVerbose)
      fprintf(
          stderr,
          "LargeAllocator::Allocate:   %p SizeWithHeader %zd Alignment %zd\n",
          Header, SizeWithHeader, Alignment);
    Header[0] = kLeftHeaderMagic;
    Header[1] = SizeWithHeader;
    Header[2] = kRightHeaderMagic;
    return Header + kCpuPageSize / sizeof(Header[0]);
  }

  size_t GetPtrChunkSize(void *Ptr) {
    auto Header = GetHeader(Ptr);
    return Header[1] - kCpuPageSize;
  }

  void Deallocate(void *Ptr, bool Protect) {
    auto Header = GetHeader(Ptr);
    size_t MmapSize = Header[1];
    if (Config.LargeAllocVerbose)
      fprintf(stderr, "LargeAllocator::Deallocate: %p %zd %s\n", Header,
              MmapSize, Protect ? "protect" : "recycle");
    if (Protect)
      mmap(Header, MmapSize, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED,
           -1, 0);
    else
      munmap(Header, MmapSize);
  }
 private:
  uintptr_t *GetHeader(void *Ptr) {
    uintptr_t *Header =
        reinterpret_cast<uintptr_t *>(Ptr) - kCpuPageSize / sizeof(Header[0]);
    if (Header[0] != kLeftHeaderMagic && Header[2] != kRightHeaderMagic)
      __builtin_trap();
    return Header;
  }
};

}  // namespace MTMalloc

#endif  // __MTMCLLOC_LARGE_H
