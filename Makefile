# sml2c — SysML v2 compiler
#
#   make             build bin/sml2c
#   make run         build and run on the built-in sample
#   make tokens      build and dump the token stream of the built-in sample
#   make test        parse every .sysml in test/ and print the AST
#   make test-all    strict pass/fail run, including test/bad/ negatives
#   make test-c      cc -fsyntax-only on each --emit-c output
#   make test-c-run  compile each --emit-c output + companion .driver.c,
#                    run it, diff stdout against test/expected/*.expect
#   make test-graphsml  Python adapter smoke run on each --emit-json output
#   make test-ptc    PTC reference-file gates (parser-strict + baseline)
#   make sweep       all of the above + verify-tokens.sh, single report
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
	for f in $(TEST_DIR)/*.sysml $(TEST_DIR)/bad/*.sysml; do \
	    [ -e "$$f" ] || continue; \
	    case $$f in \
	        $(TEST_DIR)/bad/*) expect=fail; label=$${f#$(TEST_DIR)/} ;; \
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

# ---- C codegen smoke test --------------------------------------------
#
# Pipe each positive .sysml test through `--emit-c` into `cc
# -fsyntax-only`.  Files with bad SysML are skipped (test/bad/ is
# excluded already because TEST_FILES is wildcard test/*.sysml).
# A failure here means we generated invalid C, which is a regression
# worth catching independently of the SysML test suite.

.PHONY: test-c
test-c: $(BIN)
	@pass=0; fail=0; \
	for f in $(TEST_FILES); do \
	    base=$$(basename $$f); \
	    out=$$(./$(BIN) --emit-c $$f 2>/dev/null | $(CC) -std=c11 -I runtime -fsyntax-only -x c - 2>&1); \
	    if [ $$? -eq 0 ]; then \
	        pass=$$((pass+1)); \
	    else \
	        echo "  FAIL  $$base"; \
	        echo "$$out" | sed 's/^/        /'; \
	        fail=$$((fail+1)); \
	    fi; \
	done; \
	echo "  C codegen: $$pass passed, $$fail failed"; \
	[ $$fail -eq 0 ]

# ---- emitted-C runtime gate -----------------------------------------
#
# `test-c` proves the emitted C is *syntactically* valid (cc -fsyntax-
# only).  `test-c-run` goes further: for any test/<name>.sysml that has
# a companion test/<name>.driver.c (a hand-written main exercising the
# generated API) and a test/expected/<name>.expect (golden stdout), it
# compiles+links+runs and diffs.
#
# Walks every test/*.driver.c.  Failure diffs are printed inline.  A
# missing .expect file fails the gate (force authors to commit one).

.PHONY: test-c-run
test-c-run: $(BIN)
	@pass=0; fail=0; \
	for drv in $(wildcard test/*.driver.c); do \
	    base=$$(basename $$drv .driver.c); \
	    src="test/$$base.sysml"; \
	    exp="test/expected/$$base.expect"; \
	    if [ ! -f "$$src" ]; then \
	        echo "  FAIL  $$base (missing $$src)"; fail=$$((fail+1)); continue; fi; \
	    if [ ! -f "$$exp" ]; then \
	        echo "  FAIL  $$base (missing $$exp)"; fail=$$((fail+1)); continue; fi; \
	    ./$(BIN) --emit-c "$$src" > /tmp/sml2c-$$base.c 2>/dev/null; \
	    if ! $(CC) -std=c11 -I runtime -o /tmp/sml2c-$$base /tmp/sml2c-$$base.c "$$drv" 2>/tmp/sml2c-$$base.cc.log; then \
	        echo "  FAIL  $$base (compile failed)"; \
	        sed 's/^/        /' /tmp/sml2c-$$base.cc.log; \
	        fail=$$((fail+1)); continue; fi; \
	    /tmp/sml2c-$$base > /tmp/sml2c-$$base.out 2>&1; \
	    if ! diff -u "$$exp" /tmp/sml2c-$$base.out > /tmp/sml2c-$$base.diff 2>&1; then \
	        echo "  FAIL  $$base (output mismatch)"; \
	        sed 's/^/        /' /tmp/sml2c-$$base.diff; \
	        fail=$$((fail+1)); continue; fi; \
	    pass=$$((pass+1)); \
	done; \
	echo "  C runtime: $$pass passed, $$fail failed"; \
	[ $$fail -eq 0 ]

# ---- PTC reference-file gate ----------------------------------------
#
# PTC is a 1580-line industry-standard SysML file from the SysML
# Practitioner Toolkit Companion.  We track two numbers:
#
#   parser-only errors   — strict gate, must be 0
#   default-mode errors  — informational, tracked against PTC_BASELINE
#                          so an unexpected jump shows up as a FAIL
#                          (regression catch) without forcing every
#                          turn to be net-zero on PTC errors
#
# Override PTC_FILE to point at a different reference, or
# PTC_BASELINE to update the tracked count after a deliberate
# improvement / regression.

PTC_FILE     ?= ./docs/ptc-25-04-31.sysml
PTC_BASELINE ?= 15

.PHONY: test-ptc
test-ptc: $(BIN)
	@if [ ! -f "$(PTC_FILE)" ]; then \
	    echo "  PTC: skipped (file not found: $(PTC_FILE))"; \
	    exit 0; \
	fi; \
	parser_errs=$$(./$(BIN) --no-resolve --no-typecheck --no-redefcheck \
	                       --no-connectcheck --no-refcheck \
	                       "$(PTC_FILE)" 2>&1 | grep -c "Error:"); \
	default_errs=$$(./$(BIN) "$(PTC_FILE)" 2>&1 | grep -c "Error:"); \
	if [ "$$parser_errs" -ne 0 ]; then \
	    echo "  FAIL  PTC parser-only: $$parser_errs errors (expected 0)"; \
	    exit 1; \
	fi; \
	if [ "$$default_errs" -gt "$(PTC_BASELINE)" ]; then \
	    echo "  FAIL  PTC default-mode: $$default_errs errors (baseline $(PTC_BASELINE))"; \
	    echo "        regression — fix or update PTC_BASELINE"; \
	    exit 1; \
	fi; \
	echo "  PTC: parser=$$parser_errs, default=$$default_errs (baseline $(PTC_BASELINE))"

# ---- Aggregate sweep ------------------------------------------------
#
# Single command for the full validation matrix.  Runs each gate,
# prints one summary line per gate, and exits non-zero on any fail.
# Order is cheapest-first so a regression surfaces quickly.

.PHONY: sweep
sweep: $(BIN)
	@status=0; \
	echo "==> verify-tokens"; \
	./verify-tokens.sh         || status=1; \
	echo "==> test-all (strict)"; \
	$(MAKE) -s test-all        || status=1; \
	echo "==> test-c (cc -fsyntax-only)"; \
	$(MAKE) -s test-c          || status=1; \
	echo "==> test-c-run (cc + ./binary + diff)"; \
	$(MAKE) -s test-c-run      || status=1; \
	echo "==> test-graphsml"; \
	$(MAKE) -s test-graphsml   || status=1; \
	echo "==> test-ptc"; \
	$(MAKE) -s test-ptc        || status=1; \
	echo ""; \
	if [ $$status -eq 0 ]; then echo "sweep: all gates green"; \
	else                        echo "sweep: FAIL"; fi; \
	exit $$status

