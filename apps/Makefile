# Native ports of the 3.11 applications, linked against libw16
CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
          -Wno-missing-field-initializers -I../libw16/include $(shell sdl2-config --cflags)
LIBW16 := ../libw16/build/libw16.a
LIBS = $(LIBW16) $(shell sdl2-config --libs) -lz -lm
APPS := notepad

all: $(APPS:%=build/%)

$(LIBW16): FORCE
	$(MAKE) -C ../libw16

build/notepad: notepad/notepad.c notepad/print.c $(LIBW16)
	@mkdir -p build
	$(CC) $(CFLAGS) notepad/notepad.c notepad/print.c $(LIBS) -o $@

clean:
	rm -rf build

FORCE:
.PHONY: all clean FORCE
