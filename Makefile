CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter
LDLIBS  ?= -lm
SRC     := src/bus.c src/cpu.c src/rom.c

all: n64info

n64info: $(SRC) src/main.c src/n64.h
	$(CC) $(CFLAGS) -Isrc -o $@ $(SRC) src/main.c $(LDLIBS)

build:
	@mkdir -p build

test: build tests/test_cpu.c $(SRC) src/n64.h
	$(CC) $(CFLAGS) -Isrc -o build/test_cpu tests/test_cpu.c $(SRC) $(LDLIBS)
	./build/test_cpu

clean:
	rm -f n64info
	rm -rf build

.PHONY: all test clean
