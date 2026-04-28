/* sysmlc — parser_expr.c
 *
 * Pratt expression parser (Crafting Interpreters Ch. 17).
 *
 * The trick: each token type carries up to two parse functions plus
 * a precedence —
 *
 *   prefix  : how to BEGIN an expression that starts with this token
 *             (literals, identifiers, '-', '!', '(' for grouping)
 *   infix   : how to EXTEND an expression when this token follows an
 *             already-parsed operand (every binary operator)
 *   prec    : how tightly the infix variant binds
 *
 * parsePrecedence(level) is the only dispatcher.  It eats a token and
 * dispatches to its prefix rule (which produces a left-hand operand),
 * then loops: while the next token's infix precedence is >= the level
 * we were called with, eat it and dispatch to its infix rule, which
 * folds left-hand and a recursively-parsed right-hand into a fresh
 * operand.  The recursive call passes prec+1 — that "+1" is what
 * makes operators left-associative; passing the same level would
 * produce right-associativity.
 *
 *   1 + 2 * 3   →  parsePrec(ASSIGN)
 *                  ├─ literal 1
 *                  └─ + (TERM) infix → parsePrec(FACTOR)
 *                       ├─ literal 2
 *                       └─ * (FACTOR) infix → parsePrec(UNARY)
 *                            └─ literal 3
 *                  =  (1 + (2 * 3))
 *
 * Adding a new operator is a one-row change to the rules table.
 * Adding a new precedence level is one new enum value.
 * Neither requires any new functions.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser_internal.h"

typedef enum {
    PREC_NONE = 0,
    PREC_ASSIGNMENT,    /* = (we don't have assignment expressions yet) */
    PREC_OR,            /* `or`  (SysML keyword form, not || )           */
    PREC_AND,           /* `and` (SysML keyword form, not && )           */
    PREC_EQUALITY,      /* == !=                                         */
    PREC_COMPARISON,    /* < <= > >=                                     */
    PREC_TERM,          /* + -                                           */
    PREC_FACTOR,        /* * /                                           */
    PREC_UNARY,         /* prefix - and !                                */
    PREC_POWER,         /* ** (right-associative; SysML power operator)  */
    PREC_CALL,          /* . () [] (postfix; reserved)                   */
    PREC_PRIMARY
} Precedence;

typedef Node* (*PrefixFn)(void);
typedef Node* (*InfixFn)(Node* left);

typedef struct {
    PrefixFn   prefix;
    InfixFn    infix;
    Precedence precedence;
} ParseRule;

static const ParseRule* getRule(TokenType type);
static Node* parsePrecedence(Precedence prec);

/* --------- prefix parselets (consume parser.previous, return AST) ----- */

/* parsePrecedence() has already advanced when a parselet runs, so the
 * leading token sits in parser.previous.  Each parselet builds an AST
 * node from that token plus whatever follows it. */

static Node* numberLiteral(void) {
    Token t = parser.previous;
    Node* n = astMakeNode(NODE_LITERAL, t.line);
    n->as.literal.token = t;
    /* Decide int vs real by looking for a '.' in the lexeme.  This
     * mirrors what the scanner already accepted as TOKEN_NUMBER. */
    bool isReal = false;
    for (int i = 0; i < t.length; i++) {
        if (t.start[i] == '.') { isReal = true; break; }
    }
    if (isReal) {
        char buf[64];
        int len = (t.length < 63) ? t.length : 63;
        memcpy(buf, t.start, (size_t)len);
        buf[len] = '\0';
        n->as.literal.litKind = LIT_REAL;
        n->as.literal.value.rv = strtod(buf, NULL);
    } else {
        n->as.literal.litKind = LIT_INT;
        n->as.literal.value.iv = tokenToLong(t);
    }
    return n;
}

static Node* stringLiteral(void) {
    Token t = parser.previous;
    Node* n = astMakeNode(NODE_LITERAL, t.line);
    n->as.literal.token   = t;
    n->as.literal.litKind = LIT_STRING;
    return n;
}

static Node* boolLiteral(void) {
    Token t = parser.previous;
    Node* n = astMakeNode(NODE_LITERAL, t.line);
    n->as.literal.token   = t;
    n->as.literal.litKind = LIT_BOOL;
    n->as.literal.value.bv = (t.type == TOKEN_TRUE);
    return n;
}

/* Identifier expression: build a qualified name starting from the
 * already-consumed leading identifier in parser.previous, then keep
 * pulling segments off `::` or `.` separators.  This is the same
 * shape as the standalone qualifiedName() function but adapted for
 * the "leading identifier already eaten" calling convention.       */
static Node* identifierExpr(void) {
    Node* q = astMakeNode(NODE_QUALIFIED_NAME, parser.previous.line);
    astAppendQualifiedPart(q, parser.previous);
    while (check(TOKEN_COLON_COLON) || check(TOKEN_DOT)) {
        advance();                              /* eat :: or .       */
        if (!check(TOKEN_IDENTIFIER)) {
            error("Expected name after '::' or '.'.");
            break;
        }
        advance();
        astAppendQualifiedPart(q, parser.previous);
    }
    return q;
}

/* `( expr )` — parens for grouping, no AST node of their own.        */
static Node* grouping(void) {
    Node* inner = expression();
    consume(TOKEN_RIGHT_PAREN, "Expected ')' after expression.");
    return inner;
}

/* Prefix `-` or `!`. */
static Node* unaryExpr(void) {
    Token op = parser.previous;
    /* Recurse at PREC_UNARY so we collect any further unary prefixes
     * but stop before a higher-precedence binary chains in. */
    Node* operand = parsePrecedence(PREC_UNARY);
    Node* n = astMakeNode(NODE_UNARY, op.line);
    n->as.unary.op      = op;
    n->as.unary.operand = operand;
    return n;
}

/* --------- infix parselet ------------------------------------------- */

static Node* binaryExpr(Node* left) {
    Token op = parser.previous;
    /* Look up our own precedence so we can pass prec+1 to the
     * recursive call — that increment is what gives left-associative
     * operators their left-leaning shape. */
    Precedence ownPrec = getRule(op.type)->precedence;
    Node* right = parsePrecedence((Precedence)(ownPrec + 1));
    Node* n = astMakeNode(NODE_BINARY, op.line);
    n->as.binary.op    = op;
    n->as.binary.left  = left;
    n->as.binary.right = right;
    return n;
}

/* --------- the rules table ----------------------------------------- *
 *
 * One row per token type.  Designated initializers keep the table
 * sparse — every token NOT mentioned here gets {NULL, NULL,
 * PREC_NONE}, which means: not a valid expression starter, not a
 * valid infix operator.  parsePrecedence relies on that to detect
 * "this token can't appear here" automatically.                       */

static const ParseRule rules[TOKEN_EOF + 1] = {
    [TOKEN_LEFT_PAREN]      = { grouping,       NULL,        PREC_NONE       },
    [TOKEN_PLUS]            = { NULL,           binaryExpr,  PREC_TERM       },
    [TOKEN_MINUS]           = { unaryExpr,      binaryExpr,  PREC_TERM       },
    [TOKEN_STAR]            = { NULL,           binaryExpr,  PREC_FACTOR     },
    [TOKEN_STAR_STAR]       = { NULL,           binaryExpr,  PREC_POWER      },
    [TOKEN_SLASH]           = { NULL,           binaryExpr,  PREC_FACTOR     },
    [TOKEN_BANG]            = { unaryExpr,      NULL,        PREC_NONE       },
    [TOKEN_BANG_EQUAL]      = { NULL,           binaryExpr,  PREC_EQUALITY   },
    [TOKEN_EQUAL_EQUAL]     = { NULL,           binaryExpr,  PREC_EQUALITY   },
    [TOKEN_LESS]            = { NULL,           binaryExpr,  PREC_COMPARISON },
    [TOKEN_LESS_EQUAL]      = { NULL,           binaryExpr,  PREC_COMPARISON },
    [TOKEN_GREATER]         = { NULL,           binaryExpr,  PREC_COMPARISON },
    [TOKEN_GREATER_EQUAL]   = { NULL,           binaryExpr,  PREC_COMPARISON },
    [TOKEN_IDENTIFIER]      = { identifierExpr, NULL,        PREC_NONE       },
    [TOKEN_NUMBER]          = { numberLiteral,  NULL,        PREC_NONE       },
    [TOKEN_STRING]          = { stringLiteral,  NULL,        PREC_NONE       },
    [TOKEN_TRUE]            = { boolLiteral,    NULL,        PREC_NONE       },
    [TOKEN_FALSE]           = { boolLiteral,    NULL,        PREC_NONE       },
    [TOKEN_AND]             = { NULL,           binaryExpr,  PREC_AND        },
    [TOKEN_OR]              = { NULL,           binaryExpr,  PREC_OR         },
    /* Everything else: implicit { NULL, NULL, PREC_NONE }.            */
};

static const ParseRule* getRule(TokenType type) {
    /* Defensive bound check.  TokenType values come from the scanner
     * and should always be in range, but a stray TOKEN_ERROR could
     * overflow if the scanner ever grows new tokens not yet in the
     * table — we'd rather degrade to "no rule" than read out of bounds. */
    if ((int)type < 0 || (int)type > TOKEN_EOF) {
        static const ParseRule empty = { NULL, NULL, PREC_NONE };
        return &empty;
    }
    return &rules[type];
}

/* The dispatcher.  Eat a token, run its prefix rule, then keep folding
 * infix operators while their precedence is at least `prec`.          */
static Node* parsePrecedence(Precedence prec) {
    advance();
    PrefixFn prefix = getRule(parser.previous.type)->prefix;
    if (!prefix) {
        error("Expected expression.");
        /* Return a placeholder so downstream code has something to
         * point at — keeps panic-mode recovery cleaner. */
        Node* dummy = astMakeNode(NODE_LITERAL, parser.previous.line);
        dummy->as.literal.litKind = LIT_INT;
        dummy->as.literal.token   = parser.previous;
        return dummy;
    }
    Node* left = prefix();

    /* SysML measurement-units suffix:  `75[kg]`, `1.96[m/s**2]`, `(0,0,0)[spatialCF]`.
     * For now we silently consume the bracket-balanced expression so the
     * parser can advance.  Units carry no AST node yet — typechecker
     * v0.6 will introduce dimensional analysis.                        */
    while (check(TOKEN_LEFT_BRACKET)) {
        advance();                          /* eat '[' */
        int depth = 1;
        while (depth > 0 && !check(TOKEN_EOF)) {
            if      (match(TOKEN_LEFT_BRACKET))  depth++;
            else if (match(TOKEN_RIGHT_BRACKET)) depth--;
            else advance();
        }
    }

    while (prec <= getRule(parser.current.type)->precedence) {
        advance();
        InfixFn infix = getRule(parser.previous.type)->infix;
        left = infix(left);
    }
    return left;
}

Node* expression(void) {
    return parsePrecedence(PREC_ASSIGNMENT);
}
