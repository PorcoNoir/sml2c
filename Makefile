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
CFLAGS  ?= -std=c11 -O0 -g -Wall -Wextra -Wpedantic
INC     := -Iinclude

# Auto-dependency flags — for each X.c, gcc will emit build/X.d listing
# every header X.c transitively #include'd. We then `-include` those
# files so editing a header re-builds exactly the .o files that need it.
#
#   -MMD  emit .d alongside compilation, omit system headers
#   -MP   emit dummy "phony" targets so deleted headers don't error
#   -MF   write the .d file at the path we choose (build/X.d)
#
# We use $(basename $(@F)) — file part of the target with .o stripped —
# rather than $*, because $* is only meaningful inside pattern rules.
DEPFLAGS = -MMD -MP -MF $(BUILD_DIR)/$(basename $(@F)).d

# ---- directories ------------------------------------------------------

SRC_DIR   := src
INC_DIR   := include
OBJ_DIR   := obj
BIN_DIR   := bin
BUILD_DIR := build
TEST_DIR  := test

# ---- file lists -------------------------------------------------------

# main.c is at the project root; everything else lives in src/.
SRCS := main.c $(wildcard $(SRC_DIR)/*.c)

# Map every source to obj/<basename>.o regardless of whether it lives
# at the root or inside src/.  $(notdir ...) strips the directory part,
# so both main.c and src/scanner.c land in obj/ uniformly.
OBJS := $(addprefix $(OBJ_DIR)/, $(notdir $(SRCS:.c=.o)))
DEPS := $(addprefix $(BUILD_DIR)/, $(notdir $(SRCS:.c=.d)))

BIN  := $(BIN_DIR)/sml2c

# ---- targets ----------------------------------------------------------

.PHONY: all run tokens test clean

all: $(BIN)

# Linking rule.  The | $(BIN_DIR) is an "order-only prerequisite":
# ensures the directory exists before linking, but its mtime never
# triggers a rebuild — without `|`, touching the dir would relink.
$(BIN): $(OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

# Pattern rule for src/*.c -> obj/*.o.
# $< = first prerequisite (the .c file), $@ = the target (the .o file),
# $* = the stem (the filename minus extension), used by DEPFLAGS.
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR) $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INC) $(DEPFLAGS) -c $< -o $@

# Separate rule for main.c, since it lives at the root rather than src/.
$(OBJ_DIR)/main.o: main.c | $(OBJ_DIR) $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INC) $(DEPFLAGS) -c $< -o $@

# Directory creation.  Listed as targets, used as order-only prereqs.
$(OBJ_DIR) $(BIN_DIR) $(BUILD_DIR):
	@mkdir -p $@

# Pull in the auto-generated header dependencies if they exist yet.
# The `-` prefix means: don't error if the files are missing on a
# fresh clone — they'll get generated on the first compile.
-include $(DEPS)

# ---- convenience targets ---------------------------------------------

run: $(BIN)
	./$(BIN)

tokens: $(BIN)
	./$(BIN) --tokens

# Defaults to test/Feature.sysml; override with `make test FILE=other.sysml`.
FILE ?= $(TEST_DIR)/Feature.sysml
test: $(BIN)
	./$(BIN) $(FILE)

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR) $(BUILD_DIR)