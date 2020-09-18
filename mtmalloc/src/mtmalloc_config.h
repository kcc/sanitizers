// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef __MTMALLOC_CONFIG_H__
#define __MTMALLOC_CONFIG_H__

#include <stdint.h>
#include <stdlib.h>

namespace MTMalloc {

struct MallocConfig {
  uint64_t Initialized       : 1;
  uint64_t PrintStats        : 1;
  uint64_t PrintSpAlloc      : 1;
  uint64_t PrintScan         : 1;
  uint64_t LargeAllocFence   : 1;
  uint64_t LargeAllocVerbose : 1;
  uint64_t UseTag            : 2;  // 0: no tag, 1 : 4-bit tag, 2: 8bit tag.
  uint64_t UseShadow         : 1;
  uint64_t UseAliases        : 1;
  uint64_t QuarantineSize    : 8;  // 0..255 percent.
  uint64_t HandleSigUsr2     : 1;
  uint64_t ReleaseFreq       : 8;  // 0 .. 255 (in miliseconds; 0 means off).

  void Init() {
    if (Initialized) return;
    PrintStats = !!EnvToLong("MTM_PRINT_STATS", 0, 0, 1);
    PrintSpAlloc = !!EnvToLong("MTM_PRINT_SP_ALLOC", 0, 0, 1);
    PrintScan = !!EnvToLong("MTM_PRINT_SCAN", 0, 0, 1);
    LargeAllocFence = !!EnvToLong("MTM_LARGE_ALLOC_FENCE", 1, 0, 1);
    LargeAllocVerbose = !!EnvToLong("MTM_LARGE_ALLOC_VERBOSE", 0, 0, 1);
    QuarantineSize = EnvToLong("MTM_QUARANTINE_SIZE", 0, 0, 255);
    UseTag = EnvToLong("MTM_USE_TAG", 0, 0, 2);
    UseAliases = !!EnvToLong("MTM_USE_ALIASES", 0, 0, 1);
    UseShadow = !!EnvToLong("MTM_USE_SHADOW", 0, 0, 1);
    HandleSigUsr2 = EnvToBool("MTM_HANDLE_SIGUSR2", true);
    ReleaseFreq = EnvToLong("MTM_RELEASE_FREQ", 0, 0, 255);
  }

  MallocConfig() { Init(); }

  long EnvToLong(const char *Env, long DefaultValue, long MinValue,
                 long MaxValue) {
    const char *Value = getenv(Env);
    if (!Value) return DefaultValue;
    long Res = atol(Value);
    if (Res < MinValue) return MinValue;
    if (Res > MaxValue) return MaxValue;
    return Res;
  }

  bool EnvToBool(const char *Env, bool DefaultValue) {
    return EnvToLong(Env, DefaultValue, 0, 1);
  }
};

extern MallocConfig Config;

}  // namespace MTMalloc

#endif  //  __MTMALLOC_CONFIG_H__
