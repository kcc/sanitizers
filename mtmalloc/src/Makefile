# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Sorry, not a fancy build system.
# Primarily, because building mtmalloc should be as simple as
# $CXX -c $CXXFLAGS mtmalloc.cpp
#
# (Well, if this project ends up being something more than a toy,
# we may need a fancy build system. But not now)
#
# In order to build with g++, use "make CXX=g++" (not tested regularly).

CXX=clang++
CXXFLAGS= -O2 -g -std=c++17 -fno-exceptions -Wall -march=native

all: mtmalloc_test malloc_benchmark standalone_malloc_test

test: all
	./mtmalloc_test && ./standalone_malloc_test && ./malloc_benchmark

clean:
	rm -f *.a *.o *_test *_benchmark

HEADERS= mtmalloc.h mtmalloc_config.h mtmalloc_large.h mtmalloc_util.h \
	 mtmalloc_size_classes.h mtmalloc_shadow.h mtmalloc_tags.h

mtmalloc_test: mtmalloc_test.cpp $(HEADERS) Makefile
	$(CXX) $(CXXFLAGS) $< -o $@ -fPIC -lgtest -lgtest_main -lpthread
mtmalloc.o: mtmalloc.cpp $(HEADERS) Makefile
	$(CXX) $(CXXFLAGS) $< -o $@ -fPIC -c
mtmalloc.a: mtmalloc.o
	ar rv $@ $<
malloc_benchmark: malloc_benchmark.cpp mtmalloc.a
	$(CXX) $(CXXFLAGS) $< mtmalloc.a -o $@ -isystem benchmark/include   -Lbenchmark/build/src -lbenchmark -lpthread
system_malloc_benchmark: malloc_benchmark.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ -isystem benchmark/include   -Lbenchmark/build/src -lbenchmark -lpthread
standalone_malloc_test: standalone_malloc_test.cpp mtmalloc.a
	$(CXX) $(CXXFLAGS) $< -o $@ mtmalloc.a -lpthread

