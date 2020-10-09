// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "gtest/gtest.h"
#include "mtmalloc.h"
#include "mtmalloc_large.h"
#include <set>
#include <thread>

using MTMalloc::Allocator;
using MTMalloc::TLS;
using MTMalloc::SizeToSizeClass;
using MTMalloc::GetRss;
using MTMalloc::SizeClassDescr;
namespace MTMalloc {
MallocConfig Config;
Allocator *Allocator::SingletonSelf;

pthread_key_t Allocator::TSDKey;
pthread_once_t Allocator::TSDOKeyOnce = PTHREAD_ONCE_INIT;
}

TEST(Allocator, Allocate1) {
  Allocator A;
  memset(&A, 0, sizeof(A));
  memset(&TLS, 0, sizeof(TLS));
  size_t beg = 0;
  size_t n = 10000;
  std::set<void *> Set;
  for (size_t i = beg; i < n; i++) {
    void *Res = A.Allocate(i + 8);
    memset(Res, 42, i + 8);
    Set.insert(Res);
    //if (i < 5 || i > n - 5)
    // fprintf(stderr, "Res %p sz %zd\n", Res, i+8);
  }
  EXPECT_EQ(Set.size(), n - beg);
  for (void *P : Set)
    A.Deallocate(P);
  //fprintf(stderr, "\n");
  std::set<void*> NewSet;
  std::set<size_t> Sizes;
  for (size_t i = beg; i < n; i++)
    Sizes.insert(i + 8);

  while (NewSet.size() < Set.size()) {
    for (auto size: Sizes)
      if (void *Res = A.Allocate(size))
        if (Set.count(Res)) {
          NewSet.insert(Res);
          Sizes.erase(size);
          break;
        }
  }
  fprintf(stderr, "\n");
}

TEST(Allocator, Allocate2) {
  Allocator A;
  memset(&A, 0, sizeof(A));
  memset(&TLS, 0, sizeof(TLS));
  for (int i = 0; i < 10; i++)
    for (size_t Sz : {10, 100, 2000, 65536, 16384}) {
      std::set<void *> Set;
      size_t Num = (1 << 20) / Sz;
      for (size_t j = 0; j < Num; j++) {
        void *Res = A.Allocate(Sz);
        memset(Res, 0x42, Sz);
        Set.insert(Res);
      }
      for (void *P : Set)
        A.Deallocate(P);
    }
}
TEST(Allocator, DoubleFree) {
  Allocator A;
  memset(&A, 0, sizeof(A));
  memset(&TLS, 0, sizeof(TLS));
  void *P = A.Allocate(42);
  A.Deallocate(P);
  EXPECT_DEATH(A.Deallocate(P), "DoubleFree");
  P = A.Allocate(66);
  A.Quarantine(P);
  EXPECT_DEATH(A.Quarantine(P), "DoubleFree");
}

TEST(Allocate, Quarantine) {
  Allocator A;
  memset(&A, 0, sizeof(A));
  memset(&TLS, 0, sizeof(TLS));
  size_t n = 5000;
  std::set<void *> Set;
  size_t TotalSize = 0;
  size_t TotalRoundedSize = 0;
  // Allocate a few chunks.
  for (size_t i = 0; i < n; i++) {
    size_t Size = i + 8;
    void *Res = A.Allocate(Size);
    memset(Res, 42, Size);
    Set.insert(Res);
    TotalSize += Size;
    SizeClassDescr SCD;
    TotalRoundedSize += SizeClassToSize(SizeToSizeClass(Size, SCD));
    // if (i < 5 || i > n - 5) fprintf(stderr, "Res %p\n", Res);
  }
  EXPECT_EQ(Set.size(), n);
  // Put them all into an infinite Quarantine.
  for (void *P : Set)
    A.Quarantine(P);
  EXPECT_LE(TotalSize, TLS.LocalQuarantineSize);
  EXPECT_EQ(TotalRoundedSize, TLS.LocalQuarantineSize);

  A.Scan();
  EXPECT_EQ(A.BytesInQuarantine, 0);

  std::set<void*> NewSet;
  std::set<size_t> Sizes;
  for (size_t i = 0; i < n; i++)
    Sizes.insert(i + 8);

  while (NewSet.size() < Set.size()) {
    for (auto size: Sizes)
      if (void *Res = A.Allocate(size))
        if (Set.count(Res)) {
          NewSet.insert(Res);
          Sizes.erase(size);
          break;
        }
  }

  fprintf(stderr, "\n");

  // Allocate something else, and actually use it.
  uintptr_t *P1 = reinterpret_cast<uintptr_t*>(A.Allocate(100));
  uintptr_t *P2 = reinterpret_cast<uintptr_t*>(A.Allocate(1000));
  *P1 = reinterpret_cast<uintptr_t>(P2);

  fprintf(stderr, "Dangling pointer %p inside %p\n", P2, P1);

  // Put everything but P1/P2 into quarantine.
  for (void *P : Set)
    A.Quarantine(P);
  // Also put P2 into Quarantine, but leave P1 pointing to P2.
  A.Quarantine(P2);
  A.Scan();
  EXPECT_EQ(A.BytesInQuarantine, 1024);
  // Remove the reference to P2 from P1.
  *P1 = 0xDEADBEEF;
  A.Scan();
  EXPECT_EQ(A.BytesInQuarantine, 0);
}


void Worker(Allocator &A) {
  uintptr_t PrevPtr = 0;
  for (int i = 0; i < 100000; i++) {
    size_t SizeBytes = 16 + 8 * (i % 2048);
    uintptr_t *Ptr = reinterpret_cast<uintptr_t*>(A.Allocate(SizeBytes));
    // fprintf(stderr, "T%lu [%d] %p\n", pthread_self(), i, Ptr);
    size_t SizeWords = SizeBytes / sizeof(uintptr_t);
    for (size_t j = 0; j < SizeWords; j++)
      Ptr[j] = PrevPtr;
    PrevPtr = reinterpret_cast<uintptr_t>(Ptr);
    A.QuarantineAndMaybeScan(Ptr, 1 << 28);
  }
}

TEST(Allocate, Threads1) {
  Allocator A;
  memset(&A, 0, sizeof(A));
  auto CB = [&]() { Worker(A); };
  std::thread t1(CB);
  std::thread t2(CB);
  t1.join();
  t2.join();
  EXPECT_GT(A.NumScans, 5);
}

void UnusedPagesWorker(Allocator &A) {
  size_t kAllocationPerSize = 16 << 20;
  std::vector<void *> V;
  size_t MinSize = 1024;
  size_t MaxSize = 1024 * 16;
  V.reserve(kAllocationPerSize / MinSize);
  size_t OldRss = GetRss();
  for (size_t Size = 128; Size <= MaxSize; Size *= 2) {
    V.resize(0);
    for (size_t Allocated = 0; Allocated < kAllocationPerSize; Allocated += Size) {
      void *P = A.Allocate(Size);
      memset(P, 0x42, Size);
      V.push_back(P);
    }
    for (void *P : V)
      A.Deallocate(P);
    size_t NewRss = GetRss();
    fprintf(stderr, "Size: %zd RSSDelta %zdM\n", Size, (NewRss - OldRss) >> 20);
    OldRss = NewRss;
  }
}

TEST(Allocate, UnusedPages) {
  Allocator A;
  memset(&A, 0, sizeof(A));
  auto CB = [&]() { UnusedPagesWorker(A); };
  std::thread t1(CB);
  t1.join();
}

// Test the implementation: make sure the first few allocations
// start allocating from kAllocatorSpace[1].
TEST(Allocate, FirstAllocationTest) {
  Allocator A;
  memset(&A, 0, sizeof(A));
  memset(&TLS, 0, sizeof(TLS));
  size_t kSize = 1 << 15;
  for (size_t i = 0; i <= 15; i++) {
    void *P = A.Allocate(kSize);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(P), MTMalloc::kFirstSuperPage[1] + i * kSize);
  }
  for (size_t i = 0; i <= 15; i++) {
    void *P = A.Allocate(kSize);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(P),
              MTMalloc::kFirstSuperPage[1] + MTMalloc::kSuperPageSize + i * kSize);
  }

  void *Small = A.Allocate(16);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(Small), MTMalloc::kFirstSuperPage[0]);
}

TEST(LargeAllocator, SimpleTest) {
  MTMalloc::LargeAllocator A;
  size_t Size1 = 1 << 20;
  size_t Size2 = 1 << 21;
  void *P1 = A.Allocate(Size1);
  void *P2 = A.Allocate(Size2);
  memset(P1, 1, Size1);
  memset(P2, 2, Size2);
  EXPECT_EQ(A.GetPtrChunkSize(P1), Size1);
  EXPECT_EQ(A.GetPtrChunkSize(P2), Size2);
  fprintf(stderr, "P1 %p\n", P1);
  fprintf(stderr, "P2 %p\n", P2);
  EXPECT_DEATH(A.Deallocate(reinterpret_cast<char*>(P1) + 4096, false), "");
  A.Deallocate(P2, false);
  EXPECT_DEATH(A.Deallocate(P2, false), "");
  A.Deallocate(P1, false);

  auto P3 = A.Allocate(Size1);
  A.Deallocate(P3, false);
  auto P4 = A.Allocate(Size1);
  EXPECT_EQ(P3, P4);  // we actually don't guarantee this.
  A.Deallocate(P4, true);
  auto P5 = A.Allocate(Size1);
  EXPECT_NE(P4, P5);  // must be different.
  EXPECT_DEATH(memset(P4, 1, 1), "");  // must be protected.
}

TEST(Signals, NullDeref) {
  Allocator A;
  memset(&A, 0, sizeof(A));
  memset(&TLS, 0, sizeof(TLS));
  A.Allocate(100);  // triggers sig handler init.
  volatile int *p = reinterpret_cast<int*>(0x42UL);
  EXPECT_DEATH(*p = 0, "MTMalloc: SEGV si_addr: 0x42");
}
