// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// MTMalloc - a heap allocator for memory tagging architectures.
// In the current stage it's a prototype suitable for initial performance
// experiments.
//
// See here for related discussion:
// github.com/google/sanitizers/blob/master/hwaddress-sanitizer/MarkUs-GC.md
//
// Main ideas:
//   * Allocation happens from super pages of fixed size and alignment,
//   each super page contains chunks of a single size class.
//   * The metadata for a super page is a byte array, one byte per chunk.
//   This byte array is also stored in the super page.
//   * Each byte indicates whether the chunk is available for allocation,
//   allocated, is in quarantine, or is marked by GC.
//   * State transition for every chunk
//   (available=>allocated=>quarantine=>marked=>{quarantine,available}
//   is a single 1-byte store.
//
//   The GC scan is currently full stop-the-world.
//
//
// TODOs:
// TODO: clang: zero freed pointers to reduce the number of dangling pointers.
// TODO: Adjust size classes to reduce slack.
// TODO: Store metadata for large size classes outside of SuperPage.
// TODO: Handle small, medium, and huge size classes differently.
// TODO: Privatize SuperPages (maybe only for small size classes) to avoid CAS.
// TODO: Recycle unused SuperPages via madvise MADV_DONTNEED.
// TODO: Implement concurrent scan.
// TODO: Scan stacks and globals.
// TODO: split into more files.

#ifndef __MTMALLOC_H__
#define __MTMALLOC_H__

#include "mtmalloc_config.h"
#include "mtmalloc_util.h"
#include "mtmalloc_size_classes.h"
#include "mtmalloc_shadow.h"
#include "mtmalloc_tags.h"

#include <type_traits>
#include <sys/mman.h>
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <dirent.h>
#include <signal.h>
#include <algorithm>

#include <immintrin.h>

namespace MTMalloc {

static const size_t kSuperPageSize = 1 << 19; // 2Mb
// Even better is to have super pages of different sizes.

const size_t kMaxThreads = 1 << 12;

const size_t kAllocatorSpace = 0x600000000000ULL;
const size_t kAllocatorSize  =  0x10000000000ULL;  // 1T.

const size_t kPrimaryMetaSpace = 0x700000000000ULL;
const size_t kPrimaryMetaSize = kAllocatorSize / kSuperPageSize;
FixedShadow<kPrimaryMetaSpace, kAllocatorSpace, kAllocatorSize, kSuperPageSize>
    SuperPageMetadata;

const size_t kSecondRangeMeta = 0x710000000000ULL;

// One range contains size classes specific to that range.
// Dividing size classes allows to do quicker search for relevant SuperPages,
// and allows to have range-specific metadata implementation.
// The extreme case would be to dedicate one range to every size class,
// like the Sanitizer allocator or Scudo malloc do, but that hits hard on TLB
// and it will make the range checks in the GC scan more complicated.
static constexpr size_t kNumSizeClassRanges = 2;
static constexpr size_t kSizeAlignmentForSecondRange = 1024;

const size_t kMeta[kNumSizeClassRanges] = {
    kPrimaryMetaSpace,
    kPrimaryMetaSpace + kPrimaryMetaSize / 2};

constexpr size_t kFirstSuperPage[kNumSizeClassRanges] = {
    kAllocatorSpace, kAllocatorSpace + kAllocatorSize / 2};

FixedShadow<kSecondRangeMeta, kFirstSuperPage[1], kAllocatorSize / 2,
            kSuperPageSize, kSuperPageSize / kSizeAlignmentForSecondRange>
    SecondRangeMeta;

static AddressAndMemoryTags<kAllocatorSpace, kAllocatorSize,
                            kSizeAlignmentForSecondRange>
    Tags;

const size_t kSizeOfLocalQuarantine = 1 << 20;

struct SizeClass {
  uint8_t v;
};

SizeClass GetSizeClass(uintptr_t Ptr) {
  if (!SuperPageMetadata.IsMine(Ptr)) TRAP();
  return {SuperPageMetadata.Get(Ptr)};
}

void SetSizeClass(uintptr_t Ptr, SizeClass SC) {
  if (!SuperPageMetadata.IsMine(Ptr)) TRAP();
  SuperPageMetadata.Set(Ptr, SC.v);
}

struct SizeClassDescr {
  uint64_t RangeNum : 1;
  uint64_t NumChunks : 15;
  uint64_t ChunkSizeDiv16 : 16;
  uint64_t ChunkSizeMulDiv : 32;
  constexpr size_t ChunkSize() const { return ChunkSizeDiv16 * 16; }
};

SizeClassDescr SCDescr[kNumSizeClasses];

size_t super_pages[kNumSizeClasses];  // TODO: remove this.

constexpr SizeClass SizeToSizeClass(size_t Size, SizeClassDescr &SCD) {
  static_assert(SCArray[15] == 256);
  if (Size <= 256) {
    SizeClass SC = {uint8_t((Size + 15) / 16 - 1)};
    SCD = SCDescr[SC.v];
    return SC;
  }
  for (uint8_t Idx = 0; Idx < kNumSizeClasses; Idx++)
    if (Size <= SCDescr[Idx].ChunkSize()) {
      SCD = SCDescr[Idx];
      return {Idx};
    }
  SCD = SCDescr[0];
  return {0};  // may happen on the first call.
}

constexpr size_t SizeClassToSize(SizeClass sc) {
  return SCDescr[sc.v].ChunkSize();
}

// Factoid: a division by a constant can be replaced with
// a multiplication by a constant followed by a shift.
// Compilers do it all the time.
// Factoid: when computing Left / Div, where Left is in [0,kSuperPageSize)
// and Div is in [16, MaxSizeClass] the division can be replaced by
// a multiplication followed by a right shift by 35 for *most* values of Div.
// We choose the size classes such that this works for every size class.
// So, instead of dividing by the size in the hot spot, we multiply by a
// specially prepared constant, see ComputeMulForDiv().
// Related reading: https://arxiv.org/pdf/1902.01961.pdf

static constexpr uint32_t kDivMulShift = 35;

uint32_t ComputeMulForDiv(uint32_t Div, uint32_t Shift) {
  uint32_t Mul = (1ULL << Shift) / Div;
  if (Div & (Div - 1)) Mul++;
  return Mul;
}

bool IsCorrectDivToMul(uint32_t Div, uint32_t Mul, uint32_t Shift,
                       uint32_t MaxLeft) {
  for (uint64_t Left = 1; Left <= MaxLeft; Left++) {
    uint32_t D1 = Left / Div;
    uint32_t D2 = (Left * Mul) >> Shift;
    if (D1 != D2) return false;
  }
  return true;
}

static uint32_t DivBySizeViaMul(uint32_t Left, uint32_t DivMul) {
  uint64_t T = Left;
  return (T * DivMul) >> kDivMulShift;
}


static constexpr size_t kStateArrayAlignment = 32;

constexpr size_t SizeOfInlineMeta(size_t NumChunks, size_t RangeNum) {
  if (RangeNum == 1) return 0;
  return //kStateArrayAlignment +
      RoundUpTo(NumChunks, kStateArrayAlignment);
}

constexpr size_t ComputeNumChunks(size_t ChunkSize, size_t RangeNum) {
  size_t Approx = kSuperPageSize / ChunkSize;
  for (size_t NumChunks = Approx; NumChunks > 0; NumChunks--)
    if (SizeOfInlineMeta(NumChunks, RangeNum) + NumChunks * ChunkSize <=
        kSuperPageSize)
      return NumChunks;
  __builtin_trap();
}

template <class CallBack>
size_t FindByte_Plain(uint8_t *Bytes, uint8_t Value, size_t N,
                    size_t StartPosHint, CallBack CB) {
  if (StartPosHint > N) TRAP();
  for (size_t I = 0; I < N; I++) {
    size_t Idx = I + StartPosHint;
    if (Idx >= N) Idx -= N;
    if (Bytes[Idx] == Value)
      if (CB(Idx))
        return Idx;
  }
  return -1;
}

template <class CallBack>
size_t FindByte_PEXT(uint8_t *Bytes, uint8_t Value, size_t N,
                    size_t StartPosHint, CallBack CB) {
  assert(Value == 0);  // so that we can use _pext_u64. Others must be odd.
  size_t NRounded = RoundUpTo(N, 8);
  size_t Hint = RoundDownTo(StartPosHint, 8);
  if (StartPosHint > N) TRAP();
  for (size_t I = 0; I < N; I += 8) {
    size_t Idx = I + Hint;
    if (Idx >= NRounded) Idx -= NRounded;
    uint64_t Tuple = *reinterpret_cast<uint64_t*>(&Bytes[Idx]);
    uint64_t Mask = _pext_u64(Tuple, 0x0101010101010101ULL);
    Mask = (~Mask) & 0xFF;
    if (0)
      fprintf(stderr, "  PEXT: bytes %p Idx %zd tuple %016zx mask %zx\n", Bytes,
              Idx, Tuple, Mask);
    while (Mask) {
      size_t BitIdx = __builtin_ctz(Mask);
      Mask &= ~(1ULL << BitIdx);
      size_t Pos = Idx + BitIdx;
      if (Pos >= N) break;
      if (CB(Pos)) return Pos;
    }
  }
  return -1;
}

template <class CallBack>
size_t FindByte_AVX256(uint8_t *Bytes, uint8_t Value, size_t N,
                       size_t StartPosHint, CallBack CB) {
  size_t NRounded = RoundUpTo(N, 32);
  assert(Value == 0);  // so that we can use _mm512_testn_epi8_mask.
  if (StartPosHint > N) TRAP();
  size_t Hint = RoundDownTo(StartPosHint, 32);
  for (size_t I = 0; I < NRounded; I += 32) {
    size_t Idx = I + Hint;
    if (Idx >= NRounded) Idx -= NRounded;
    auto Tuple = _mm256_load_si256((const __m256i *)&Bytes[Idx]);
    auto Mask = _mm256_testn_epi8_mask(Tuple, Tuple);
    while (Mask) {
      size_t BitIdx = __builtin_ctz(Mask);
      Mask &= ~(1ULL << BitIdx);
      size_t Pos = Idx + BitIdx;
      if (CB(Pos)) return Pos;
    }
  }
  return -1;
}

struct SuperPage {

  enum state_t {
    AVAILABLE = 0,  // must be 0 for the _mm512_testn_epi8_mask hasck to work.
    USED_MIXED = 1, // This and others need to be odd for _pext_u64 to work.
    USED_DATA  = 3,
    QUARANTINED = 5,
    MARKED      = 7,
    RELEASING   = 255,
  };

  uintptr_t This() const { return reinterpret_cast<uintptr_t>(this); }
  uintptr_t End() const { return This() + kSuperPageSize; }
  uintptr_t *LastBS() const {
    return reinterpret_cast<uintptr_t *>(End() - 16);
  }

  SizeClass GetSC() { return GetSizeClass(This()); }
  SizeClassDescr GetSCD() { return SCDescr[GetSizeClass(This()).v]; }
  // uint32_t &LastIdxHint() { return *reinterpret_cast<uint32_t *>(End() - 16); }
  uint8_t *State(size_t NumChunks, size_t RangeNum) {
    if (RangeNum == 1)
      return SecondRangeMeta.GetShadowPtr(This());
    return reinterpret_cast<uint8_t *>(End() - SizeOfInlineMeta(NumChunks, 0));
  }

  uint8_t *AddressOfChunk(size_t Idx, SizeClassDescr SCD) {
    return (uint8_t*)this + Idx * SCD.ChunkSize();
  }

  template <typename Callback>
  void IterateStates(Callback CB) {
    auto SCD = GetSCD();
    size_t N = SCD.NumChunks;
    size_t RangeNum = SCD.RangeNum;
    uint8_t *S = State(N, RangeNum);
    for (size_t i = 0; i < N; i++)
      CB(S[i]);
  }

  size_t CountStates(state_t State) {
    size_t Res = 0;
    IterateStates([&](uint8_t S) {
      if (S == State) Res++;
    });
    return Res;
  }


  static void PrintSizes(SizeClass SC) {
    size_t Size = SizeClassToSize(SC);
    SizeClassDescr SCD = SCDescr[SC.v];
    size_t NumChunks = ComputeNumChunks(SizeClassToSize(SC), SCD.RangeNum);
    size_t MetaSize = SizeOfInlineMeta(NumChunks, SCD.RangeNum);
    size_t Slack = kSuperPageSize - Size * NumChunks - MetaSize;
    fprintf(stderr, "sc %d r %d sz %zd chunks %zd meta %zd slack %zd\tss %zd\n",
            (int)SC.v, (int)SCD.RangeNum, Size, NumChunks, MetaSize, Slack,
            super_pages[SC.v]);
  }

  void Print() {
    auto SCD = GetSCD();
    int RangeNum = SCD.RangeNum;
    size_t Ava = CountStates(AVAILABLE);
    size_t Qua = CountStates(QUARANTINED);
    size_t Mar = CountStates(MARKED);
    size_t Uti =
        (SCD.NumChunks - Ava - Qua) * SCD.ChunkSize() * 100 / kSuperPageSize;
    fprintf(stderr,
            "SP r %d %zd %p sc %d Size %zd Num %d Ava %zd Qua %zd Mar %zd Uti "
            "%zd %s\n",
            RangeNum,
            (reinterpret_cast<uintptr_t>(this) - kFirstSuperPage[RangeNum]) /
                kSuperPageSize,
            this, (int)GetSC().v, SCD.ChunkSize(), (int)SCD.NumChunks, Ava, Qua,
            Mar, Uti, (Ava + Qua == SCD.NumChunks) ? "unused" : "");
  }

  bool AllAvailable() {
    return CountStates(AVAILABLE) == GetSCD().NumChunks;
  }


  // __attribute__((noinline))
  __attribute__((always_inline))
  void *TryAllocate(bool DataOnly, SizeClassDescr SCD, size_t *HintPtr) {
    // fprintf(stderr, "TryAllocate %p %d\n", this, SCD.NumChunks);
    // We use LastIdxHint to start the search from the index that was used last
    // and is thus likely non-zero. This is an important performance
    // optimization but it also means that within the SuperPage chunks will
    // be recycled less frequently.
    size_t Hint = *HintPtr; // LastIdxHint();
    size_t NumChunks = SCD.NumChunks;
    uint8_t *S = State(NumChunks, SCD.RangeNum);
    uint8_t NewState = DataOnly ? USED_DATA : USED_MIXED;

    auto TryPos = [&](size_t Pos) -> bool {
      uint8_t ExpectedState = AVAILABLE;
      // CAS! Alternative is to privatise a SuperPage.
      if (!__atomic_compare_exchange_n(&S[Pos], &ExpectedState, NewState,
                                       true, __ATOMIC_RELAXED,
                                       __ATOMIC_RELAXED))
        return false;
      return true;
    };

    size_t Pos = FindByte_PEXT(S, AVAILABLE, NumChunks, Hint, TryPos);
    if (Pos == (size_t)-1) return nullptr;
    if (Pos >= NumChunks) {
      fprintf(stderr, "Pos %zd NumChunks %zd ChunkSize %zd Hint %zd\n", Pos,
              NumChunks, SCD.ChunkSize(), Hint);
      Print();
      TRAP();
    }
    *HintPtr = Pos + 1; // RoundDownTo(Pos, 32);
    void *Res = AddressOfChunk(Pos, SCD);
    Res = Tags.ApplyAddressTag(Res, Tags.GetMemoryTag(Res));
    if (0) {
      fprintf(stderr,
              "Allocate [%p,%p) SP %p Pos %zd ChunkSize %zd NumChunks %d "
              "Meta %zd "
              "LastIdxHint %zd\n",
              Res, (char *)Res + SCD.ChunkSize(), this, Pos, SCD.ChunkSize(),
              (int)SCD.NumChunks, SizeOfInlineMeta(SCD.NumChunks, SCD.RangeNum),
              *HintPtr);
      Print();
    }
    return Res;
  }

  uint8_t *ComputeStatePtr(void *Ptr, SizeClassDescr SCD) {
    //assert(SCD.ChunkSizeDiv16 * 16 == ChunkSize());
    //assert(SCD.NumChunks == NumChunks());
    //assert(SCD.ChunkSizeMulDiv == ChunkSizeMulDiv());
    uintptr_t P = reinterpret_cast<uintptr_t>(Ptr);
    uintptr_t Offset = P % kSuperPageSize;
    // size_t Idx = Offset / ChunkSize();
    size_t Idx = DivBySizeViaMul(Offset, SCD.ChunkSizeMulDiv);
    if (Idx * SCD.ChunkSize() != Offset) {
      fprintf(stderr,
              "ComputeStatePtr Idx %zd SC.ChunkSize %zd Offset %zx P %zx\n",
              Idx, SCD.ChunkSize(), Offset, P);
      TRAP();
    }
    if (Idx >= SCD.NumChunks) TRAP();
    return &State(SCD.NumChunks, SCD.RangeNum)[Idx];
  }

  void Mark(uintptr_t P) {
    P %= kSuperPageSize;
    auto SCD = SCDescr[GetSizeClass(This()).v];
    size_t NumChunks = SCD.NumChunks;
    uint32_t ChunkSizeMulDiv = SCD.ChunkSizeMulDiv;
    size_t Idx = DivBySizeViaMul(P, ChunkSizeMulDiv);
    if (Idx >= NumChunks) return;
    uint8_t *S = State(NumChunks, SCD.RangeNum);
    if (__atomic_load_n(&S[Idx], __ATOMIC_RELAXED) == QUARANTINED)
      __atomic_store_n(&S[Idx], MARKED, __ATOMIC_RELAXED);
  }

  void MoveFromQuarantineToAvailable() {
    IterateStates([](uint8_t &S) {
      if (S == QUARANTINED) S = AVAILABLE;
      if (S == MARKED) S = QUARANTINED;
    });
  }

  __attribute__((always_inline))
  void ExchangeAndCheckForDoubleFree(void *Ptr, uint8_t *S,
                                           uint8_t NewValue) {
    // XCHG here is expensive. If we don't need to check for double-free
    // *precisely*, we can do a regular atomic load/store instead.
    // If we don't need to check double-free at all, just a store is enough.
    // auto OldValue = __atomic_exchange_n(S, NewValue, __ATOMIC_RELAXED);
    auto OldValue = __atomic_load_n(S, __ATOMIC_RELAXED);
    __atomic_store_n(S, NewValue, __ATOMIC_RELAXED);
    if (OldValue != USED_MIXED && OldValue != USED_DATA) {
      fprintf(stderr, "DoubleFree on %p\n", Ptr);
      TRAP();
    }
  }

  // Returns the new tag.
  uint8_t UpdateMemoryTagOnFree(void *P, size_t Size) {
    if (!Config.UseShadow) return 0;
    uint8_t OldMemoryTag = Tags.GetMemoryTag(P);
    uint8_t NewMemoryTag = OldMemoryTag + 1;  // or random?
    Tags.SetMemoryTag(P, Size, NewMemoryTag);
    return NewMemoryTag;
  }

  __attribute__((always_inline))
  void Deallocate(void *Ptr) {
    auto SCD = GetSCD();
    auto S = ComputeStatePtr(Ptr, SCD);
    UpdateMemoryTagOnFree(Ptr, SCD.ChunkSize());
    ExchangeAndCheckForDoubleFree(Ptr, S, AVAILABLE);
  }

  size_t Quarantine(void *Ptr) {
    auto SCD = GetSCD();
    auto S = ComputeStatePtr(Ptr, SCD);
    uint8_t NewTag = UpdateMemoryTagOnFree(Ptr, SCD.ChunkSize());
    uint8_t NewValue = QUARANTINED;
    if (Config.UseTag == 1 && (NewTag & 15) != 0)
      NewValue = AVAILABLE;
    if (Config.UseTag == 2 && (NewTag & 255) != 0)
      NewValue = AVAILABLE;
    // fprintf(stderr, "Ptr %p NewTag %d\n", Ptr, NewTag);
    // Memset is good for security, and with Arm MTE tagging we'll get it
    // as a byproduct of tagging, but we don't scan the memory in quarantine,
    // so strictly for UAF we don't need it here. Disabled for now to simplify
    // benchmarking.
    // memset(Ptr, 0xfb, SCD.ChunkSize());
    ExchangeAndCheckForDoubleFree(Ptr, S, NewValue);
    if (NewValue == AVAILABLE)
      return 0;
    return SCD.ChunkSize();
  }

  void MarkAllLivePointers(size_t NumSuperPages[kNumSizeClassRanges]) {
    static_assert(kNumSizeClassRanges == 2);  // not sure if we want to generalize.
    auto SCD = GetSCD();
    size_t ChunkSize = SCD.ChunkSize();
    size_t SuperPageRegionSize[2] = {NumSuperPages[0] * kSuperPageSize,
                                     NumSuperPages[1] * kSuperPageSize};
    uint8_t *S = State(SCD.NumChunks, SCD.RangeNum);
    for (size_t Idx = 0, N = SCD.NumChunks; Idx < N; Idx++) {
      if (S[Idx] == USED_MIXED) {
        uint8_t *P = AddressOfChunk(Idx, SCD);
        for (uint8_t *Word = P; Word < P + ChunkSize; Word += sizeof(void *)) {
          // This is a very hot load.  I've tried using AVX512 for this
          // (_mm512_load_si512/_mm512_cmpgt_epu64_mask) but it was slower.
          uintptr_t Value = *(uintptr_t *)Word;
          // if (Value <= kFirstSuperPage || Value >= LastSuperPage) continue;
          if (Value - kFirstSuperPage[0] >= SuperPageRegionSize[0] &&
              Value - kFirstSuperPage[1] >= SuperPageRegionSize[1])
            continue;
          reinterpret_cast<SuperPage *>((RoundDownTo(Value, kSuperPageSize)))
              ->Mark(Value);
        }
      }
    }
  }

  void Unmark() {
    IterateStates([](uint8_t &S) {if (S == MARKED) S = QUARANTINED;});
  }

  size_t CountMarked() {
    return CountStates(MARKED);
  }

  size_t CountAvailable() {
    return CountStates(AVAILABLE);
  }

  size_t CountQuarantined() {
    return CountStates(QUARANTINED);
  }

  // Very basic release to OS.
  // Possible improvements:
  // * release parts of the SuperPage.
  // * use 8-byte or 16-byte CAS (and loads).
  // * Try not to release already released SuperPage.
  // * Choose pages to try-to-release better than randomly.
  // Can we use this?
  // https://www.kernel.org/doc/html/latest/admin-guide/mm/pagemap.html
  void MaybeReleaseToOs() {
    // fprintf(stderr, "MaybeReleaseToOs %p\n", this);
    auto SCD = GetSCD();
    size_t NumChunks = SCD.NumChunks;
    size_t Ava = CountAvailable();
    if (Ava != NumChunks) return;
    size_t NumReadyToRelease = 0;
    IterateStates([&](uint8_t &S) {
      uint8_t ExpectedState = AVAILABLE;
      uint8_t NewState = RELEASING;
      if (__atomic_compare_exchange_n(&S, &ExpectedState, NewState, true,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        NumReadyToRelease++;
    });
    if (NumReadyToRelease == NumChunks) {
      madvise(this, kSuperPageSize, MADV_DONTNEED);
      if (SCD.RangeNum == 1)  // state is stored separately.
        IterateStates([](uint8_t &S) {
          __atomic_store_n(&S, AVAILABLE, __ATOMIC_RELAXED);
        });
    } else {
      IterateStates([](uint8_t &S) {
        if (__atomic_load_n(&S, __ATOMIC_RELAXED) == RELEASING)
          __atomic_store_n(&S, AVAILABLE, __ATOMIC_RELAXED);
      });
    }
    if (0)
      fprintf(
          stderr, "SP %p: %s\n", this,
          NumReadyToRelease == NumChunks ? "released" : "failed to release");
  }
};

SuperPage *A2SP(uintptr_t Addr) {
  if (Addr < kAllocatorSpace) TRAP();
  if (Addr >= kAllocatorSpace + kAllocatorSize) TRAP();
  if (Addr % kSuperPageSize) TRAP();
  return reinterpret_cast<SuperPage*>(Addr);
}

SuperPage *GetSuperPage(size_t RangeNum, size_t Idx) {
  size_t Addr = kFirstSuperPage[RangeNum] + Idx * kSuperPageSize;
  if (Addr >= kAllocatorSpace + kAllocatorSize) TRAP();
  return reinterpret_cast<SuperPage*>(Addr);
}


struct Statistics {
  uint64_t AllocsPerSizeClass[kNumSizeClasses];
  uint64_t AccessesPerSizeClass[kNumSizeClasses];
  uint64_t LargeAllocs;
  uint64_t AccessOther;

  void MergeFrom(Statistics *From) {
    for (size_t i = 0; i < kNumSizeClasses; i++) {
      __atomic_fetch_add(&AllocsPerSizeClass[i], From->AllocsPerSizeClass[i],
                         __ATOMIC_RELAXED);
      __atomic_fetch_add(&AccessesPerSizeClass[i],
                         From->AccessesPerSizeClass[i], __ATOMIC_RELAXED);
    }
    __atomic_fetch_add(&LargeAllocs, From->LargeAllocs, __ATOMIC_RELAXED);
    __atomic_fetch_add(&AccessOther, From->AccessOther, __ATOMIC_RELAXED);
  }

  void Print() {
    for (uint8_t i = 0; i < kNumSizeClasses; i++)
      if (auto Allocs = AllocsPerSizeClass[i])
        fprintf(stderr, "stat.allocs sc %d\tsize\t%zd\tcount %zd\n", i,
                SizeClassToSize({i}), Allocs);
    if (LargeAllocs) fprintf(stderr, "stat.large_allocs %zd\n", LargeAllocs);
    for (uint8_t i = 0; i < kNumSizeClasses; i++)
      if (auto Accesses = AccessesPerSizeClass[i])
        fprintf(stderr, "stat.accesses sc %d\tsize\t%zd\tcount %zd\n", i,
                SizeClassToSize({i}), Accesses);
    if (AccessOther) fprintf(stderr, "stat.access_other %zd\n", AccessOther);
  }
};

struct ThreadLocalAllocator {
  uint32_t Rand;
  size_t LocalQuarantineSize;
  struct {
    SuperPage *SP;
    size_t LastIdxHint;
  } PerSC[kNumSizeClasses];
  Statistics Stats;
};

__attribute__((tls_model("initial-exec")))
__thread ThreadLocalAllocator TLS;

// ANSI C linear congruential PRNG.
static inline uint32_t RandR(uint32_t *State) {
  uint32_t NewState = *State * 1103515245 + 12345;
  *State = NewState;
  return NewState >> 16;
}

struct ScopedLock {
  ScopedLock(pthread_mutex_t &Mu, int line) : Mu(Mu) {
    //fprintf(stderr, "Trying    %p line %d\n", &Mu, line);
    pthread_mutex_lock(&Mu);
    //fprintf(stderr, "Acquired  %p line %d\n", &Mu, line);
  }
  ~ScopedLock() {
    //fprintf(stderr, "Releasing %p\n", &Mu);
    pthread_mutex_unlock(&Mu);
  }
  pthread_mutex_t &Mu;
};

struct Allocator {
  static pthread_key_t TSDKey;
  static pthread_once_t TSDOKeyOnce;

  pthread_once_t InitAllOnce;  // Assuming PTHREAD_ONCE_INIT==0.

  Statistics Stats;

  pthread_mutex_t Mu;
  pthread_cond_t Cv;
  static Allocator *SingletonSelf;
  size_t NumScans;
  size_t NumSuperPages[kNumSizeClassRanges];  // atomic
  size_t GetNumSuperPages(size_t RangeNum) {
    return __atomic_load_n(&NumSuperPages[RangeNum], __ATOMIC_ACQUIRE);
  }

  size_t BytesInQuarantine; // atomic outside of scan.
  size_t ScanPos[kNumSizeClassRanges];  // atomic
  size_t LastQurantineSize;

  size_t DataOnlyScopeLevel;

  __attribute__((noinline))
  size_t ScanLoop() {
    const size_t kPosIncrement = 1024;
    size_t NumSuperPages[kNumSizeClassRanges] = {GetNumSuperPages(0),
                                                 GetNumSuperPages(1)};
    size_t NumDone = 0;
    for (size_t RangeNum : {0, 1}) {
      size_t N = NumSuperPages[RangeNum];
      // fprintf(stderr, "ScanLoop TID %d start\n", gettid());
      while (true) {
        size_t Pos = __atomic_fetch_add(&ScanPos[RangeNum], kPosIncrement,
                                        __ATOMIC_RELAXED);
        if (Pos >= N) break;
        size_t EndIdx = std::min(N, Pos + kPosIncrement);
        NumDone += EndIdx - Pos;
        for (size_t SPIdx = Pos; SPIdx < EndIdx; SPIdx++)
          GetSuperPage(RangeNum, SPIdx)->MarkAllLivePointers(NumSuperPages);
      }
    }
    return NumDone;
    // fprintf(stderr, "ScanLoop TID %d done %zd\n", gettid(), NumDone);
  }

  size_t PostScan(bool Verbose) {
    size_t NewBytesInQuarantine = 0;
    for (size_t RangeNum : {0, 1}) {
      for (size_t SPIdx = 0, N = GetNumSuperPages(RangeNum); SPIdx < N;
           SPIdx++) {
        auto SP = GetSuperPage(RangeNum, SPIdx);
        size_t WasInQuarantine = SP->CountQuarantined();
        size_t WasAvailable = SP->CountAvailable();
        auto SCD = SP->GetSCD();
        size_t NumChunks = SCD.NumChunks;
        size_t ChunkSize = SCD.ChunkSize();
        // if (!WasInQuarantine) continue; // nothing to do.
        SP->MoveFromQuarantineToAvailable();
        size_t NowInQuorantine = SP->CountQuarantined();
        if (NowInQuorantine)
          NewBytesInQuarantine += ChunkSize * NowInQuorantine;
        if (Verbose)
          fprintf(stderr,
                  "--- %p SC %d marked %zd quarantined %zd=>%zd available "
                  "%zd=>%zd nchunks %zd %s bytesInQ %zd\n",
                  SP, (int)SP->GetSC().v, SP->CountMarked(), WasInQuarantine,
                  NowInQuorantine, WasAvailable, SP->CountAvailable(),
                  NumChunks, WasAvailable == NumChunks ? "was_empty" : "",
                  ChunkSize * NowInQuorantine);
        // fprintf(stderr, "scan10 %p SC %d\n", (void*)SS, GetSizeClass(SPA).v);
      }
    }
    return NewBytesInQuarantine;
  }

  __attribute__((noinline))
  void Scan() {
    for (size_t RangeNum : {0, 1})
      __atomic_store_n(&ScanPos[RangeNum], 0, __ATOMIC_RELAXED);
    size_t NumSeenThreads = KillAllThreadsButMyself();
    NumScans++;
    size_t time1 = usec();
    bool Verbose = Config.PrintScan;
    if (Verbose)
      fprintf(stderr, "scan1 %p %p %zd %zd\n", (void *)kFirstSuperPage[0],
              (void *)kFirstSuperPage[1], GetNumSuperPages(0),
              GetNumSuperPages(1));

    size_t NumDoneInThisThread = ScanLoop();
    size_t NewBytesInQuarantine = PostScan(Verbose);
    size_t time2 = usec();

    // if (Verbose)
    fprintf(
        stderr,
        "Scan %zd: tid %d BytesInQuarantine %zdM => %zdM; "
        "SuperPages %zd / %zd Allocated %zdM RSS %zdM time %zd threads %zd\n",
        NumScans, GetTID(), BytesInQuarantine >> 20, NewBytesInQuarantine >> 20,
        GetNumSuperPages(0) + GetNumSuperPages(1), NumDoneInThisThread,
        ((GetNumSuperPages(0) + GetNumSuperPages(1)) * kSuperPageSize) >> 20,
        GetRss() >> 20, time2 - time1, NumSeenThreads);
    LastQurantineSize = BytesInQuarantine = NewBytesInQuarantine;

  }

  __attribute__((always_inline))
  //__attribute__((noinline))
  void *Allocate(size_t Size) {
    SizeClassDescr SCD;
    SizeClass SC  = SizeToSizeClass(Size, SCD);

    auto &PerSC = TLS.PerSC[SC.v];
    TLS.Stats.AllocsPerSizeClass[SC.v]++;

    if (PerSC.SP)
      if (void *Res = PerSC.SP->TryAllocate(DataOnlyScopeLevel, SCD,
                                            &PerSC.LastIdxHint))
        return Res;
    return AllocateSlower(Size);
  }

  __attribute__((noinline))
  void *AllocateSlower(size_t Size) {
    if (!TLS.Rand) {
      SingletonSelf = this;
      pthread_once(&InitAllOnce, InitSingleton);
      pthread_once(&TSDOKeyOnce, TSDCreate);
      pthread_setspecific(TSDKey, (void*)32UL);
      // fprintf(stderr, "Thread first seen tid %d TLS %p\n", GetTID(), &TLS);
      TLS.Rand = pthread_self();
    }
    // Remember that on the first call the size class table is not yet set up.
    SizeClassDescr SCD;
    SizeClass SC  = SizeToSizeClass(Size, SCD);

    auto *PerSC = &TLS.PerSC[SC.v];
    uint8_t *Meta = reinterpret_cast<uint8_t*>(kMeta[SCD.RangeNum]);
    while (true) {
      size_t N = GetNumSuperPages(SCD.RangeNum);
      size_t Offset = N ? (RandR(&TLS.Rand) % N) : 0;
      for (size_t I = 0; I < N; I++) {
        size_t Idx = I + Offset;
        if (Idx >= N) Idx -= N;
        if (Meta[Idx] == SC.v) {
          auto SP = PerSC->SP = GetSuperPage(SCD.RangeNum, Idx);
          if (void *Res =
                  SP->TryAllocate(DataOnlyScopeLevel, SCD, &PerSC->LastIdxHint))
            return Res;
        }
      }
      AllocateSuperPage(Size);
      PerSC->LastIdxHint = 0;
    }
  }

  size_t KillAllThreadsButMyself() {
    pid_t MyTID = GetTID();
    pid_t MyPID = getpid();
    // fprintf(stderr, "KillAllThreadsButMyself %d\n", MyTID);
    pid_t SeenThreads[kMaxThreads];
    size_t NumSeenThreads = 1;
    SeenThreads[0] = MyTID;
    bool Changed = true;
    while (Changed) {
      Changed = false;
      IterateTIDs([&](pid_t TID) {
        if (SeenThreads + NumSeenThreads ==
            std::find(SeenThreads, SeenThreads + NumSeenThreads, TID)) {
          SeenThreads[NumSeenThreads++] = TID;
          if (NumSeenThreads > kMaxThreads) TRAP();
          // fprintf(stderr, "Killing %d\n", TID);
          TGKill(MyPID, TID, SIGUSR2);  // don't check the result.
          // fprintf(stderr, "done %d\n", res);
          Changed = true;
        }
      });
    }
    return NumSeenThreads;
    // fprintf(stderr, "\n\n\n Seen %zd threads\n\n\n", NumSeenThreads);
  }

  size_t GetPtrChunkSize(void *Ptr) {
    Ptr = Tags.ApplyAddressTag(Ptr, 0);
    uintptr_t P = reinterpret_cast<uintptr_t>(Ptr);
    uintptr_t StartSP = RoundDownTo(P, kSuperPageSize);
    assert(StartSP >= kAllocatorSpace);
    assert(StartSP < kAllocatorSpace + kAllocatorSize);
    return A2SP(StartSP)->GetSCD().ChunkSize();
  }

  __attribute__((noinline))
  void CountAccess(void *Ptr) {
    if (IsMine(Ptr))
      TLS.Stats
          .AccessesPerSizeClass[GetSizeClass(reinterpret_cast<uintptr_t>(
                                                 Tags.ApplyAddressTag(Ptr, 0)))
                                    .v]++;
    else
      TLS.Stats.AccessOther++;
  }

  bool IsMine(void *Ptr) {
    uintptr_t P = reinterpret_cast<uintptr_t>(Ptr);
    if (Config.UseAliases)
      return P >= kAllocatorSpace && P < kAllocatorSpace + 16 * kAllocatorSize;
    return P >= kAllocatorSpace && P < kAllocatorSpace + kAllocatorSize;
  }

  void *RemoveAddressTagAndCheckForDoubleFree(void *Ptr) {
    uint8_t AddressTag = Tags.GetAddressTag(Ptr);
    Ptr = Tags.ApplyAddressTag(Ptr, 0);
    uint8_t MemoryTag = Tags.GetMemoryTag(Ptr) & 15;
    // fprintf(stderr, "Deallocate %p %x %x\n", Ptr, (int)AddressTag,
    //        (int)MemoryTag);
    if (Config.UseShadow && Config.UseAliases && AddressTag != MemoryTag) {
      fprintf(stderr, "ERROR: double-free %p\n", Ptr);
      __builtin_trap();
    }
    return Ptr;
  }

  __attribute__((always_inline))
  void Deallocate(void *Ptr) {
    Ptr = RemoveAddressTagAndCheckForDoubleFree(Ptr);
    uintptr_t P = reinterpret_cast<uintptr_t>(Ptr);
    uintptr_t StartSP = RoundDownTo(P, kSuperPageSize);
    if (StartSP < kAllocatorSpace) TRAP();
    if (StartSP >= kAllocatorSpace + kAllocatorSize) TRAP();
    reinterpret_cast<SuperPage*>(StartSP)->Deallocate(Ptr);
  }

  void Quarantine(void *Ptr) {
    Ptr = RemoveAddressTagAndCheckForDoubleFree(Ptr);
    uintptr_t P = reinterpret_cast<uintptr_t>(Ptr);
    uintptr_t StartSP = RoundDownTo(P, kSuperPageSize);
    if (StartSP < kAllocatorSpace) TRAP();
    if (StartSP >= kAllocatorSpace + kAllocatorSize) TRAP();
//    fprintf(stderr, "Quarantine: %p %zd %zd\n", Ptr, TLS.LocalQuarantineSize,
//            BytesInQuarantine);
    TLS.LocalQuarantineSize += A2SP(StartSP)->Quarantine(Ptr);
  }

  void QuarantineAndMaybeScan(void *Ptr, size_t MaxQuarantineSize) {
    Quarantine(Ptr);
    if (TLS.LocalQuarantineSize >= kSizeOfLocalQuarantine) {
      size_t TotalQuarantineSize = __atomic_add_fetch(
          &BytesInQuarantine, TLS.LocalQuarantineSize, __ATOMIC_RELAXED);
      TLS.LocalQuarantineSize = 0;
      size_t Limit = MaxQuarantineSize + LastQurantineSize;
      if (TotalQuarantineSize > Limit) {
        ScopedLock Lock(Mu, __LINE__);
        if (__atomic_load_n(&BytesInQuarantine, __ATOMIC_RELAXED) < Limit)
          return;
        Scan();
      }
    }
  }

  void MemoryReleaseThread() {
    fprintf(stderr, "MemoryReleaseThread\n");
    for (size_t Iter = 0; ; Iter++) {
      size_t RangeNum = Iter % kNumSizeClassRanges;
      size_t N = GetNumSuperPages(RangeNum);
      if (!N) continue;
      size_t Idx = Iter % N;
      GetSuperPage(RangeNum, Idx)->MaybeReleaseToOs();
      usleep(1000 * Config.ReleaseFreq);
    }
  }
  static void *MemoryReleaseThread(void *) {
    SingletonSelf->MemoryReleaseThread();
    return nullptr;
  }

  void SignalHandler() {
    ScanLoop();
  }

  static void SignalHandler(int, siginfo_t *, void *) {
    SingletonSelf->SignalHandler();
  }

  void SetSignalHandler() {
    struct sigaction sigact = {};
    sigact.sa_flags = SA_SIGINFO;
    sigact.sa_sigaction = SignalHandler;
    if (sigaction(SIGUSR2, &sigact, 0)) TRAP();
  }

  static void TSDOnThreadExit(void *TSD) {
    SingletonSelf->Stats.MergeFrom(&TLS.Stats);
    // fprintf(stderr, "TSDOnThreadExit tid %d TSD %p\n", GetTID(), TSD);
  }

  static void TSDCreate() { pthread_key_create(&TSDKey, TSDOnThreadExit); }

  __attribute__((noinline))
  void InitAll() {
    Config.Init();
    if (Config.HandleSigUsr2) SetSignalHandler();

    for (size_t i = 0; i < kNumSizeClasses; i++) {
      size_t ChunkSize = SCArray[i];
      while (!IsCorrectDivToMul(ChunkSize,
                                ComputeMulForDiv(ChunkSize, kDivMulShift),
                                kDivMulShift, kSuperPageSize))
        ChunkSize += kSizeAlignmentForSecondRange;
      if (ChunkSize != SCArray[i])
        fprintf(stderr, "Fix up the size: %zd => %zd\n", SCArray[i], ChunkSize);
      assert((ChunkSize % 16) == 0);
      assert(ChunkSize / 16 < (1 << 16));  // fits into uint16_t.
      SCDescr[i].RangeNum = (ChunkSize % kSizeAlignmentForSecondRange) == 0;
      SCDescr[i].ChunkSizeDiv16 = ChunkSize / 16;
      SCDescr[i].NumChunks = ComputeNumChunks(ChunkSize, SCDescr[i].RangeNum);
      SCDescr[i].ChunkSizeMulDiv = ComputeMulForDiv(ChunkSize, kDivMulShift);
      if (!IsCorrectDivToMul(ChunkSize, SCDescr[i].ChunkSizeMulDiv,
                             kDivMulShift, kSuperPageSize)) {
        fprintf(stderr, "!IsCorrectDivToMul(%zd)\n", ChunkSize);
        assert(0);
      }
    }
    void *mmap_res = mmap((void *)kAllocatorSpace, kAllocatorSize, PROT_NONE,
                          MAP_FIXED | MAP_ANONYMOUS | MAP_NORESERVE |
                              (Config.UseAliases ? MAP_SHARED : MAP_PRIVATE),
                          -1, 0);
    if (mmap_res != (void *)kAllocatorSpace) TRAP();

    SuperPageMetadata.Init();
    SecondRangeMeta.Init();
    Tags.Init();
  }

  static void InitSingleton() { SingletonSelf->InitAll(); }

  SuperPage *AllocateSuperPage(size_t Size) {
    ScopedLock Lock(Mu, __LINE__);
    // if (!GetNumSuperPages(0) && !GetNumSuperPages(1)) InitAll();
    SizeClassDescr SCD;
    SizeClass SC = SizeToSizeClass(Size, SCD);
    SuperPage *Res = GetSuperPage(SCD.RangeNum, GetNumSuperPages(SCD.RangeNum));
    void *MmapRes = mmap(Res, kSuperPageSize, PROT_READ | PROT_WRITE,
                         MAP_FIXED | MAP_ANONYMOUS | MAP_NORESERVE |
                             (Config.UseAliases ? MAP_SHARED : MAP_PRIVATE),
                         -1, 0);
    if (MmapRes != Res) TRAP();

    if (Config.UseAliases) {
      // Super-inefficient way to have address tags (TLB doesn't like it).
      uintptr_t AliasPage = reinterpret_cast<uintptr_t>(MmapRes);
      for (size_t Tag = 1; Tag < 16; Tag++) {
        AliasPage += kAllocatorSize;
        void *MremapRes =
            mremap(MmapRes, 0, kSuperPageSize, MREMAP_FIXED | MREMAP_MAYMOVE,
                   (void *)AliasPage);
        if (MremapRes != (void*)AliasPage) TRAP();
      }
    }

    // fprintf(stderr, "AllocateSuperPage %p %p\n", mmap_res,
    // (void*)kAllocatorSpace);
    SetSizeClass(Res->This(), SC);
    if (Config.PrintSpAlloc) {
      fprintf(stderr, "Allocated SP: %d\n", (int)SC.v);
      Res->Print();
    }
    size_t ChunkSize = SCD.ChunkSize();
    for (size_t Pos = Res->This(), End = Pos + ChunkSize * SCD.NumChunks;
         Pos < End; Pos += ChunkSize)
      Tags.SetMemoryTag(reinterpret_cast<void *>(Pos), ChunkSize,
                        RandR(&TLS.Rand));
    super_pages[SC.v]++;
    __atomic_add_fetch(&NumSuperPages[SCD.RangeNum], 1, __ATOMIC_RELEASE);
    return Res;
  }

  void DataOnlyScope(int level) {
    if (level == 1) DataOnlyScopeLevel++;
    else if (level == -1) {
      if (!DataOnlyScopeLevel) TRAP();
      DataOnlyScopeLevel--;
    } else TRAP();
  }

  void PrintAll() {
    fprintf(stderr, "RSS: %zdM SPs: {%zd %zd}\n", GetRss() >> 20,
            GetNumSuperPages(0), GetNumSuperPages(1));
    for (uint8_t i = 0; i < kNumSizeClasses; i++) SuperPage::PrintSizes({i});
    Stats.MergeFrom(&TLS.Stats);
    Stats.Print();
  }
};

}  // namespace
#endif  // __MTMALLOC_H__
