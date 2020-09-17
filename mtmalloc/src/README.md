# MTMalloc - experimental malloc with support for memory tagging.

Don't expect to see much here at the moment.
This malloc implementation is mostly to enable our experimentation
with hardware memory tagging extensions, such as Arm MTE, and related technologies
(e.g.[MarkUs-Gc](https://github.com/kcc/sanitizers/blob/master/hwaddress-sanitizer/MarkUs-GC.md))

## Get and Build
Make sure you have a recent clang++ instralled.
Building with g++ is supported, but not regularly tested.
```
git clone git@github.com:kcc/sanitizers.git
cd sanitizers/mtmalloc/src
make
```

This will produce `mtmalloc.a` which you can link to your binary:

```
echo 'void *p = new int[100000]; int main() {}' > test.cpp
clang++  test.cpp mtmalloc.a -lpthread
./a.out
```


## Test
```
sudo apt-get install libbenchmark-dev libgtest-dev
make test

```

## Flags
MTMalloc is configurable via environment variables, see `mtmalloc_config.h` for the current list.

```
# build your code and link with mtmalloc.a
MTM_PRINT_STATS=1 ./a.out  # will print some stats on exit
```

## MTMalloc and TSan instrumentation
When combined with clang's tsan instrumentation (e.g. `clang -O2 -fsanitize=thread -mllvm -tsan-instrument-atomics=0`), 
MTMalloc can be used to gather statistics about memory loads. 
It also works as a poor man's [HWASAN](https://clang.llvm.org/docs/HardwareAssistedAddressSanitizerDesign.html) on x86_64. 

```
% cat uaf.cpp
cat uaf.cpp 
char *p = new char[1000];
char *q = p;
int main() {
  delete [] p;
  return *q;
}
# compile with tsan, don't link!
% clang -fsanitize=thread -mllvm -tsan-instrument-atomics=0 -c -g -O2 uaf.cpp
# Link with MTMalloc (not with tsan!)
% clang++ uaf.o sanitizers/mtmalloc/src/mtmalloc.a -lpthread 
# Collect stats on allocations and loads/stores.
% MTM_PRINT_STATS=1 ./a.out 
...
stat.allocs sc 0        size    16      count 1
stat.allocs sc 21       size    480     count 1
stat.allocs sc 28       size    1024    count 2
stat.accesses sc 28     size    1024    count 1
stat.access_other 4
# Use as a poor man's HWASAN
% MTM_PRINT_STATS=1 MTM_USE_ALIASES=1 MTM_USE_SHADOW=1 ./a.out 
ERROR: address-memory-tag-mismatch 0x6f8000080000 f 0
Illegal instruction
```

## HWASAN 
MTMalloc is not a full-featured [HWASAN](https://clang.llvm.org/docs/HardwareAssistedAddressSanitizerDesign.html) implementation,
but it can be used as a simple heap-only HWASAN-like tool on x86-64 (with tsan instrumentation, as described above). 
* `MTM_USE_SHADOW=1` enables the use of software shadow memory (see `SetMemoryTag()` / `GetMemoryTag()`)
* `MTM_USE_ALIASES=1` uses `mremap` to implement 16 page aliases for every small heap allocation, thus effectively implmenting 4-bit address tags in bits `40..43`. This is wastly inefficient compared to Arm's top-byte-ignore (TBI), but works as functional model of TBI. See `ApplyAddressTag()` and `GetAddressTag()`. 
* The callbacks inserted by tsan instrumentation (`__mtm_access()`) verify that the address belongs to the small heap and if so, checks whether the address tag matches the memory tag.
