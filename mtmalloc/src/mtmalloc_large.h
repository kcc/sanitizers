// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef __MTMCLLOC_LARGE_H
#define __MTMCLLOC_LARGE_H

#include <pthread.h>

// Super-simple allocator for large memory regions.
// It need to be enhanced in multiple dimensions.
namespace MTMalloc {
class LargeAllocator {
  static constexpr size_t kCpuPageSize = 1 << 12;
  static constexpr size_t kLeftHeaderMagic =  0x039C823525B0237EULL;
  static constexpr size_t kRightHeaderMagic = 0x1C2C5300098D85ADULL;
 public:

  void *Allocate(size_t Size) {
    size_t RoundedSize = RoundUpTo(Size, kCpuPageSize);
    size_t SizeWithHeader = RoundedSize + kCpuPageSize;
    uintptr_t *Header = reinterpret_cast<uintptr_t *>(
        mmap(0, SizeWithHeader, PROT_READ | PROT_WRITE,
             MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0));
    if (Config.LargeAllocVerbose)
      fprintf(stderr, "LargeAllocator::Allocate:   %p %zd\n", Header,
              SizeWithHeader);
    if ((void*)Header == (void*)-1) TRAP();
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
    if (Header[0] != kLeftHeaderMagic && Header[2] != kRightHeaderMagic) TRAP();
    return Header;
  }
};

}  // namespace MTMalloc

#endif  // __MTMCLLOC_LARGE_H
