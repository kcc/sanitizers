// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>

static const size_t kMaxNumThreads = 32;

struct Alloc {
  void *Ptr;
  size_t Size;
  size_t Hash;
};

__attribute__((noinline))
size_t ComputeHash(void *Ptr, size_t Size) {
  uintptr_t *Words = reinterpret_cast<uintptr_t*>(Ptr);
  size_t NumWords = Size / sizeof(*Words);
  // Check only first few words.
  if (NumWords > 8) NumWords = 8;
  size_t Res = 0;
  for (size_t W = 0; W < NumWords; W++) {
    // fprintf(stderr, "  [%zd] %zx\n", W, Words[W]);
    Res ^= Words[W] * (W + 1);
  }
  return Res;
}

static volatile int kZero = 0;

void Worker() {
  static const size_t NumAlloc = 1024;
  Alloc *P = new Alloc[NumAlloc];
  for (int j = 0; j < 10000; j++) {
    size_t LargeSize = j % 1024 ? 100 : (1 << 18);  // large size sometimes.
    int *LargeP = new int[LargeSize];
    // fprintf(stderr, "%p\n", LargeP);
    LargeP[kZero + 42] = 42;
    for (size_t i = 0; i < NumAlloc; i++) {
      size_t Size = 8 + 8 * (i % 512);
      void *Ptr = malloc(Size);
      size_t NumWords = Size / 8;
      void **Words = reinterpret_cast<void**>(Ptr);
      // populate memory with some pointers.
      for (size_t W = i % 8; W < NumWords; W += Size / 8)
        Words[W] = P[(W + i + j) % NumAlloc].Ptr;
      size_t Hash = ComputeHash(Ptr, Size);
      P[i] = {Ptr, Size, Hash};
      // fprintf(stderr, "malloc %p %zd %zx\n", Ptr, Size, Hash);
    }
    for (size_t i = 0; i < NumAlloc; i++) {
      void *Ptr = P[i].Ptr;
      size_t Size = P[i].Size;
      size_t Hash = ComputeHash(Ptr, Size);
      //fprintf(stderr, "free   %p %zd %zx\n", Ptr, Size, Hash);
      assert(P[i].Hash == Hash);
      free(Ptr);
    }
    assert(LargeP[kZero + 42] == 42);
    free(LargeP);
  }
  delete []P;
}

int main(int argc, char **argv) {
  size_t NumThreads = kMaxNumThreads;
  if (argc >= 2)
    NumThreads = atoi(argv[1]);
  std::thread *T[kMaxNumThreads];
  for (size_t i = 0; i < NumThreads; i++)
    T[i] = new std::thread(Worker);
  for (size_t i = 0; i < NumThreads; i++) {
    T[i]->join();
    delete T[i];
  }
}
