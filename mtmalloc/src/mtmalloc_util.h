// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef __MTMALLOC_UTIL_H__
#define __MTMALLOC_UTIL_H__

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

namespace MTMalloc {
#define TRAP()                              \
  do {                                      \
    fprintf(stderr, "TRAP %d\n", __LINE__); \
    __builtin_trap();                       \
  } while (0)

int GetTID() { return syscall(SYS_gettid); }
int TGKill(int a, int b, int c) { return syscall(SYS_tgkill, a, b, c); }
int GetDEnts64(unsigned int fd, char *dirp, unsigned int count) {
  return syscall(SYS_getdents64, fd, dirp, count);
}
static inline size_t GetRss() {
  if (FILE *f = fopen("/proc/self/statm", "r")) {
    size_t size = 0, rss = 0;
    fscanf(f, "%zd %zd", &size, &rss);
    fclose(f);
    return rss << 12;  // rss is in pages.
  }
  return 0;
}

template <typename Callback>
void IterateTIDs(Callback CB) {
  // can't use opendir/readdir/closedir, since those may call malloc.
  struct linux_dirent64 {
    ino64_t d_ino;
    off64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
  };
  int fd, nread;
  char buf[1024];
  struct linux_dirent64 *d;
  int bpos;

  fd = open("/proc/self/task", O_RDONLY | O_DIRECTORY);
  if (fd == -1) TRAP();

  for (;;) {
    nread = GetDEnts64(fd, buf, sizeof(buf));
    if (nread == -1) TRAP();
    if (nread == 0) break;

    for (bpos = 0; bpos < nread;) {
      d = (struct linux_dirent64 *)(buf + bpos);
      if (d->d_name[0] != '.') CB(atoll(d->d_name));
      bpos += d->d_reclen;
    }
  }
  close(fd);
}

size_t usec() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec * 1000000UL + tv.tv_usec;
}

inline constexpr size_t MostSignificantSetBitIndex(size_t x) {
  // assert(x);
  unsigned long up = sizeof(void *) * 8 - 1 - __builtin_clzl(x);
  return up;
}

inline size_t LeastSignificantSetBitIndex(size_t x) {
  assert(x);
  unsigned long up;
  up = __builtin_ctzl(x);
  return up;
}

inline constexpr bool IsPowerOfTwo(size_t x) {
  return (x & (x - 1)) == 0;
}

inline constexpr size_t RoundUpToPowerOfTwo(size_t size) {
  //assert(size);
  if (IsPowerOfTwo(size)) return size;

  size_t up = MostSignificantSetBitIndex(size);
  //assert(size < (1ULL << (up + 1)));
  //assert(size > (1ULL << up));
  return 1ULL << (up + 1);
}

inline constexpr size_t RoundUpTo(size_t size, size_t boundary) {
  return (size + boundary - 1) & ~(boundary - 1);
}

inline size_t RoundDownTo(size_t x, size_t boundary) {
  return x & ~(boundary - 1);
}

inline bool IsAligned(size_t a, size_t alignment) {
  return (a & (alignment - 1)) == 0;
}

inline size_t Log2(size_t x) {
  assert(IsPowerOfTwo(x));
  return LeastSignificantSetBitIndex(x);
}

}  // namespace MTMalloc

#endif  // __MTMALLOC_UTIL_H__
