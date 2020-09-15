# MTMalloc - experimental malloc with support for memory tagging.

Don't expect to see much here at the moment.
This malloc implementation is mostly to enable our experimentation
with hardware memory tagging extensions, such as Arm MTE, and related technologies
(e.g.[MarkUs-Gc](https://github.com/kcc/sanitizers/blob/master/hwaddress-sanitizer/MarkUs-GC.md))

## Get and Build
Make sure you have a recent clang++ instralled.
Building with g++ is supported, but not regularly tested.
```
git clone git@github.com:google/sanitizers.git
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
