# This is a primitive Makefile, because most development is done
# under Xcode.  Note that the code uses blocks, and therefore requires
# clang to compile.

BINDIR=bin

CC=llvm/install/bin/clang -fblocks -Illvm/install/include -Lllvm/install/lib

LIBSRC=Intervals/interval.c Intervals/thread_pool.c
LIBOBJ=$(LIBSRC:.c=.o)

.PHONY: clang

all: test

clang:
	./build-clang

clean:
	rm -rf $(BINDIR)

library: clean
	mkdir $(BINDIR)
	$(CC) -O3 -c -o $(BINDIR)/interval.o Intervals/interval.c 
	$(CC) -O3 -c -o $(BINDIR)/thread_pool.o Intervals/thread_pool.c
	ar rcs $(BINDIR)/intervals.a $(BINDIR)/interval.o $(BINDIR)/thread_pool.o

nqueens: library
	$(CC) -O3 -o $(BINDIR)/nqueens Intervals/nqueens.c $(BINDIR)/intervals.a -lpthread -lBlocksRuntime

test: nqueens
	LD_LIBRARY_PATH=$(PWD)/llvm/install/lib $(BINDIR)/nqueens 8 2
