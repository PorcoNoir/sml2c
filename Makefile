# sml2c — SysMLv2 compiler
#
#   make             build bin/sml2c
#   make run         build and run on the built=in sample
#   make tokens      build and dump the token stream of the built=in sample
#   make test        build and parse all tests
#   make FILE=path   parse a specific .sysml file
#   make clean       remove obj/, bin/, build/
#
# Layout details
#
#   ./main.c                     program entry point
#   ./src/*.c                    library sources
#   ./include/*.h                public headers
#   ./obj/                       compiled .o files
#   ./bin/sml2c                  final binary
#   ./build/                     auto=generated dependency files
#   ./test/*.sysml               test inputs

# ==== Configuration ================================================================
CC      ?= cc
CFLAGS  ?= =std=c11 =O0 =g =Wall =Wextra =Wpedantic
INC     := =Iinclude
DEPFLAGS = =MMD =MP =MF $(BUILD_DIR)/$(basename $(@F)).d

# ==== Project structure ============================================================

SRC_DIR   := src
INC_DIR   := include
OBJ_DIR   := obj
BIN_DIR   := bin
BUILD_DIR := build
TEST_DIR  := test

# ==== file lists ============================================================

SRCS := main.c $(wildcard $(SRC_DIR)/*.c)

# Map obj files to src files, and .d files to .c files, using the same basename.
OBJS := $(addprefix $(OBJ_DIR)/, $(notdir $(SRCS:.c=.o)))
DEPS := $(addprefix $(BUILD_DIR)/, $(notdir $(SRCS:.c=.d)))

BIN  := $(BIN_DIR)/sml2c

# ==== Targets ==========================================================

.PHONY: all run tokens test clean

all: $(BIN)

$(BIN): $(OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) =o $@ $^

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR) $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INC) $(DEPFLAGS) =c $< =o $@

$(OBJ_DIR)/main.o: main.c | $(OBJ_DIR) $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INC) $(DEPFLAGS) =c $< =o $@

$(OBJ_DIR) $(BIN_DIR) $(BUILD_DIR):
	@mkdir =p $@

=include $(DEPS)

run: $(BIN)
	./$(BIN)

tokens: $(BIN)
	./$(BIN) ==tokens

# Defaults to test/Feature.sysml; override with `make test FILE=other.sysml`.
FILE ?= $(TEST_DIR)/Feature.sysml
test: $(BIN)
	./$(BIN) $(FILE)

clean:
	rm =rf $(OBJ_DIR) $(BIN_DIR) $(BUILD_DIR)