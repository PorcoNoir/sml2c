# sml2c — minimal SML2 to C compiler
#
# Usage:
#   make            build ./sml2c
#   make run        build and run on the built-in sample
#   make clean      remove build artifacts

CC      ?= cc
CFLAGS  ?= -std=c11 -O0 -g -Wall -Wextra -Wpedantic
SRC     := scanner.c main.c
OBJ     := $(SRC:.c=.o)
BIN     := sml2c

.PHONY: all run clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c scanner.h
	$(CC) $(CFLAGS) -c $< -o $@

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(OBJ) $(BIN)
