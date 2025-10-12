CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic `pkg-config --cflags freetype2`
LDFLAGS = -lxcb -lxcb-keysyms -lX11 -lX11-xcb -lXft `pkg-config --libs freetype2`

BIN = arun
SRC = arun.c

default: build

build: $(SRC)
	$(CC) -o $(BIN) $(SRC) $(CFLAGS) $(LDFLAGS)
