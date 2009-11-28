# This is a primitive Makefile, because most development is done
# under Xcode.  Note that the code uses blocks, and therefore requires
# clang to compile.

BINDIR=bin

CC=llvm/install/bin/clang -v -fblocks -Illvm/install/include

LIBSRC=Intervals/interval.c Intervals/thread_pool.c
LIBOBJ=$(LIBSRC:.c=.o)

.PHONY: clang

all: build

clang:
	./build-clang

clean:
	rm -rf $(BINDIR)

build: clean
	mkdir $(BINDIR)
	$(CC) -O3 -c -o $(BINDIR)/interval.o Intervals/interval.c 
	$(CC) -O3 -c -o $(BINDIR)/thread_pool.o Intervals/thread_pool.c
	
