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

.PHONY: all run tokens test test-all clean

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
	./$(BIN) "$$FILE"

# sml2c — SysML v2 compiler
#
#   make             build bin/sml2c
#   make run         build and run on the built-in sample
#   make tokens      build and dump the token stream of the built-in sample
#   make test        parse every .sysml in test/ and print the AST
#   make test-all    strict pass/fail run, including test/bad/ negatives
#   make test-one    parse a single .sysml file (default: test/Feature.sysml)
#                    override with `make test-one FILE=path/to/file.sysml`
#   make clean       remove obj/, bin/, build/
#
# Layout assumed by this Makefile:
#
#   ./main.c                     program entry point
#   ./src/*.c                    library sources
#   ./include/*.h                public headers
#   ./obj/                       compiled .o files (auto-created)
#   ./bin/sml2c                  final binary       (auto-created)
#   ./build/                     auto-generated .d dependency files
#   ./test/*.sysml               test inputs

# ---- toolchain & flags ------------------------------------------------

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

# All .sysml files under test/, populated lazily by `make test`.
TEST_FILES := $(wildcard $(TEST_DIR)/*.sysml)

# ---- targets ----------------------------------------------------------

.PHONY: all run tokens test test-one test-all clean

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

# Quick smoke loop: parse every .sysml file in test/ and dump its AST.
# No verdict tracking — the printed output IS the report.  Use test-all
# for the strict pass/fail run that also exercises test/bad/.
test: $(BIN)
	@for f in $(TEST_FILES); do \
		echo "=== Testing $$f ==="; \
		./$(BIN) "$$f"; \
	done

# Single-file run.  Defaults to test/Feature.sysml; override with
#   make test-one FILE=test/Connections.sysml
FILE ?= $(TEST_DIR)/Feature.sysml
test-one: $(BIN)
	./$(BIN) $(FILE)

# Run every .sysml file in test/ (positive tests, must parse) and
# test/bad/ (negative tests, must be rejected).  Directory placement
# determines expectation, so files can be named for what they test
# rather than whether they're expected to fail.
test-all: $(BIN)
	@pass=0; fail=0; \
	for f in $(TEST_DIR)/*.sysml $(TEST_DIR)/negative/*.sysml; do \
	    [ -e "$$f" ] || continue; \
	    case $$f in \
	        $(TEST_DIR)/negative/*) expect=fail; label=$${f#$(TEST_DIR)/} ;; \
	        *)                 expect=pass; label=$${f#$(TEST_DIR)/} ;; \
	    esac; \
	    out=$$(./$(BIN) "$$f" 2>&1); rc=$$?; \
	    if   [ "$$expect" = pass ] && [ $$rc -eq 0 ]; then \
	        printf "  ok    %-40s (parsed)\n" "$$label"; pass=$$((pass+1)); \
	    elif [ "$$expect" = fail ] && [ $$rc -ne 0 ]; then \
	        printf "  ok    %-40s (rejected as expected)\n" "$$label"; pass=$$((pass+1)); \
	    else \
	        printf "  FAIL  %-40s (expected %s, exit=%s)\n" "$$label" "$$expect" "$$rc"; \
	        echo "$$out" | sed 's/^/        /'; \
	        fail=$$((fail+1)); \
	    fi; \
	done; \
	echo ""; \
	echo "  $$pass passed, $$fail failed"; \
	[ $$fail -eq 0 ]

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR) $(BUILD_DIR)

# ---- Python tooling smoke test ---------------------------------------
#
# Sanity check: the graphsml2 adapter should convert every positive
# .sysml test's JSON without erroring.  The script's own walk-print
# shows the structure; we just need it to exit cleanly.

.PHONY: test-graphsml
test-graphsml: $(BIN)
	@pass=0; fail=0; \
	for f in $(TEST_FILES); do \
	    base=$$(basename $$f); \
	    out=$$(./$(BIN) --emit-json $$f 2>/dev/null | python3 tools/sml2c_to_graphsml.py - 2>&1); \
	    if [ $$? -eq 0 ]; then \
	        pass=$$((pass+1)); \
	    else \
	        echo "  FAIL  $$base"; \
	        echo "$$out" | sed 's/^/        /'; \
	        fail=$$((fail+1)); \
	    fi; \
	done; \
	echo "  graphsml adapter: $$pass passed, $$fail failed"; \
	[ $$fail -eq 0 ]
