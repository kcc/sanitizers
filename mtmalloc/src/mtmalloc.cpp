// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// malloc/free replacement lib.
// Most functionality is in mtmalloc.h.
// clang++ -O2 -g -fPIC -shared  -o allocator.so ../allocator.cpp

#include "mtmalloc.h"
#include "mtmalloc_large.h"
#include <stdlib.h>

#define ALIAS(x) __attribute__((alias(x)))


namespace MTMalloc {
MallocConfig Config;
Allocator *Allocator::SingletonSelf;
pthread_key_t Allocator::TSDKey;
pthread_once_t Allocator::TSDOKeyOnce = PTHREAD_ONCE_INIT;
}

static MTMalloc::Allocator allocator;
static MTMalloc::LargeAllocator large;

struct InitAndExit {
  ~InitAndExit() {
    if (MTMalloc::Config.PrintStats)
      allocator.PrintAll();
  }
};
static InitAndExit at_exit;

extern "C" {

// tsan callbacks, use with -fsanitize=thread -mllvm -tsan-instrument-atomics=0
void __mtm_access(void *Ptr) {
  allocator.CountAccess(Ptr);
  if (allocator.IsMine(Ptr)) {
    uint8_t AddressTag = MTMalloc::GetAddressTag(Ptr);
    uint8_t MemoryTag =
        MTMalloc::GetMemoryTag(MTMalloc::ApplyAddressTag(Ptr, 0));
    MemoryTag &= 15;
    if (AddressTag != MemoryTag) {
      fprintf(stderr, "ERROR: address-memory-tag-mismatch %p %x %x\n", Ptr,
              (int)AddressTag, (int)MemoryTag);
      __builtin_trap();
    }
  }
}

void __tsan_init() {}
void __tsan_func_entry() {}
void __tsan_func_exit() {}

void __tsan_read1(void *p) { __mtm_access(p); }
void __tsan_read2(void *p) { __mtm_access(p); }
void __tsan_read4(void *p) { __mtm_access(p); }
void __tsan_read8(void *p) { __mtm_access(p); }
void __tsan_write1(void *p) { __mtm_access(p); }
void __tsan_write2(void *p) { __mtm_access(p); }
void __tsan_write4(void *p) { __mtm_access(p); }
void __tsan_write8(void *p) { __mtm_access(p); }

// TODO: implement these hooks.
// They are relatively infrequent.
void __tsan_unaligned_read16(void *p) {}
void __tsan_unaligned_write16(void *p) {}
void __tsan_unaligned_read8(void *p) {}
void __tsan_unaligned_write8(void *p) {}
void __tsan_unaligned_read4(void *p) {}
void __tsan_unaligned_write4(void *p) {}
void __tsan_unaligned_read2(void *p) {}
void __tsan_unaligned_write2(void *p) {}
void __sanitizer_unaligned_load32(){}
void __sanitizer_unaligned_load64(){}
void __sanitizer_unaligned_load16(){}
void __sanitizer_unaligned_store64(){}
void __tsan_read16(void *p) {}
void __tsan_write16(void *p) {}
void __tsan_vptr_read() {}
void __tsan_vptr_update() {}

// TODO: do we need these?
void __tsan_read_range() {}
void __tsan_write_range() {}


void __bsa_dataonly_scope(int scope_level) {
  allocator.DataOnlyScope(scope_level);
}

void *malloc(size_t size) {
  if (size < 8) size = 1;
  if (size > MTMalloc::kMaxSizeClass) {
    MTMalloc::TLS.Stats.LargeAllocs++;
    return large.Allocate(size);
  }
  void *res = allocator.Allocate(size);
  //fprintf(stderr, "malloc %zd %p\n", size, res);
  return res;
}

void free(void *p) {
  if (!p) return;
  auto QuarantineSize = MTMalloc::Config.QuarantineSize;
  if (allocator.IsMine(p))
    if (QuarantineSize == 0)
      allocator.Deallocate(p);
    else
     allocator.QuarantineAndMaybeScan(p, QuarantineSize << 20);
  else
    return large.Deallocate(p, MTMalloc::Config.LargeAllocFence);
}

void *calloc(size_t nmemb, size_t size) {
  void *res = malloc(nmemb * size);
  memset(res, 0, nmemb * size);
  return res;
}

void *realloc(void *p, size_t size) {
  // TODO: implement a better realloc?
  // TODO: properly test realloc.
  if (!p)
    return malloc(size);
  size_t OldSize = allocator.IsMine(p) ? allocator.GetPtrChunkSize(p)
                                       : large.GetPtrChunkSize(p);
  void *NewPtr = malloc(size);
  memcpy(NewPtr, p, size < OldSize ? size : OldSize);
  free(p);
  return NewPtr;
}

void *memalign(size_t alignment, size_t size) {
  assert(0);
  return nullptr;
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
  if (alignment <= 16) {
    *memptr = malloc(size);
    return 0;
  }
  if (alignment <= 4096) {
    size = MTMalloc::RoundDownTo(size, alignment);
    *memptr = malloc(size);
    return 0;
  }
  fprintf(stderr, "posix_memalign %zd %zd\n", alignment, size);
  assert(0);
  return 0;
}

void *valloc(size_t size) {
  assert(0);
  return nullptr;
}

void cfree(void *p) ALIAS("free");
void *pvalloc(size_t size) ALIAS("valloc");
// void *__libc_memalign(size_t alignment, size_t size) ALIAS("memalign");

void malloc_usable_size() {
}

void mallinfo() {
}

void mallopt() {
}
}  // extern "C"



namespace std {
  struct nothrow_t;
}

void *operator new(size_t size) ALIAS("malloc");
void *operator new[](size_t size) ALIAS("malloc");
void *operator new(size_t size, std::nothrow_t const&) noexcept ALIAS("malloc");
void *operator new[](size_t size, std::nothrow_t const&) noexcept ALIAS("malloc");
void operator delete(void *ptr) throw() ALIAS("free");
void operator delete[](void *ptr) throw() ALIAS("free");
void operator delete(void *ptr, std::nothrow_t const&) ALIAS("free");
void operator delete[](void *ptr, std::nothrow_t const&) ALIAS("free");
