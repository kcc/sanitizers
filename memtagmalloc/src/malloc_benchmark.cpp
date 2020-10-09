// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <benchmark/benchmark.h>
#include <thread>
#include <stdio.h>

void FixedSizeLoop(size_t Size, size_t NumIter) {
  void* P[NumIter];
  for (size_t i = 0; i < NumIter; i++) P[i] = malloc(64);
  for (size_t i = 0; i < NumIter; i++) free(P[i]);
}

// T0: means it happens in main thread.
// T1: one thread
// TN: N threads

static void BM_64_T0(benchmark::State& state) {
  for (auto _ : state) FixedSizeLoop(64, 100000);
}

template<typename CallBack>
void RunThreads(size_t NumThreads, CallBack CB) {
  std::thread *T[NumThreads];
  for (size_t i = 0; i < NumThreads; i++)
    T[i] = new std::thread(CB);
  for (size_t i = 0; i < NumThreads; i++) {
    T[i]->join();
    delete T[i];
  }
}

template <size_t NumThreads>
static void BM_64(benchmark::State& state) {
  for (auto _ : state)
    RunThreads(NumThreads, []() { FixedSizeLoop(64, 100000); });
}
static void BM_64_T1(benchmark::State& state) { BM_64<1>(state); }
static void BM_64_T4(benchmark::State& state) { BM_64<4>(state); }
static void BM_64_T16(benchmark::State& state) { BM_64<16>(state); }
static void BM_64_T64(benchmark::State& state) { BM_64<64>(state); }

// Register the function as a benchmark
BENCHMARK(BM_64_T0);
BENCHMARK(BM_64_T1);
BENCHMARK(BM_64_T4);
BENCHMARK(BM_64_T16);
BENCHMARK(BM_64_T64);

BENCHMARK_MAIN();
