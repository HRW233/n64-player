CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter
LDLIBS  ?= -lm
SRC     := src/bus.c src/cpu.c src/rom.c src/rsp.c src/boot.c

all: n64player

n64player: $(SRC) src/main.c src/n64.h
	$(CC) $(CFLAGS) -Isrc -o $@ $(SRC) src/main.c $(LDLIBS)

# Drop a cartridge in and run it:  make run ROM=zelda.z64
run: n64player
	@test -n "$(ROM)" || (echo "usage: make run ROM=<cart.z64>" >&2; exit 2)
	./n64player $(ROM)

build:
	@mkdir -p build

test: build tests/test_cpu.c tests/test_rsp.c tests/test_boot.c $(SRC) src/n64.h
	$(CC) $(CFLAGS) -Isrc -o build/test_cpu tests/test_cpu.c $(SRC) $(LDLIBS)
	./build/test_cpu
	$(CC) $(CFLAGS) -Isrc -o build/test_rsp tests/test_rsp.c $(SRC) $(LDLIBS)
	./build/test_rsp
	$(CC) $(CFLAGS) -Isrc -o build/test_boot tests/test_boot.c $(SRC) $(LDLIBS)
	./build/test_boot

clean:
	rm -f n64player n64info
	rm -rf build

.PHONY: all run test clean
