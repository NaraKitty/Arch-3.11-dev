# libw16 - native Win16 USER/GDI/KERNEL reimplementation (64-bit, SDL2)
CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers \
          -Iinclude $(shell sdl2-config --cflags)
SRC := $(wildcard src/*.c)
OBJ := $(SRC:src/%.c=build/%.o)

all: build/libw16.a

build/%.o: src/%.c src/w16int.h include/w16.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/libw16.a: $(OBJ)
	ar rcs $@ $^

clean:
	rm -rf build

LIBS = $(shell sdl2-config --libs) -lz -lm
.PHONY: all clean
