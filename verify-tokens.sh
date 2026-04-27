#!/bin/bash
# verify-tokens.sh — sanity check that scanner.h declares every TOKEN_*
# the parser references.  Catches the drift case where parser.c gets
# updated with new keywords but scanner.h doesn't (or vice versa).
#
# Run from the project root:
#   ./verify-tokens.sh
#
# Exits 0 if every TOKEN_X used in src/*.c is declared in include/scanner.h.

set -e

cd "$(dirname "$0")"

if [ ! -f include/scanner.h ]; then
    echo "verify-tokens.sh: must run from project root (include/scanner.h missing)" >&2
    exit 2
fi

# Extract all TOKEN_X identifiers used anywhere in the source.
USED=$(grep -rhoE 'TOKEN_[A-Z][A-Z_]*' src/ main.c 2>/dev/null | sort -u)

# Extract all TOKEN_X declared in scanner.h.
DECLARED=$(grep -oE 'TOKEN_[A-Z][A-Z_]*' include/scanner.h | sort -u)

# Anything used but not declared is a drift bug.
MISSING=$(comm -23 <(echo "$USED") <(echo "$DECLARED"))

if [ -n "$MISSING" ]; then
    echo "ERROR: tokens used in source but not declared in include/scanner.h:" >&2
    echo "$MISSING" | sed 's/^/    /' >&2
    echo "" >&2
    echo "Either add them to the TokenType enum in include/scanner.h, or" >&2
    echo "remove the references from src/." >&2
    exit 1
fi

echo "OK: all $(echo "$USED" | wc -l | tr -d ' ') referenced tokens are declared."
