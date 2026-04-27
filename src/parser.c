/* sysmlc — parser.c
 *
 * Hand-written recursive-descent parser, one function per grammar rule.
 * State is one current token + one previous token — same shape as
 * clox's Parser struct in Crafting Interpreters Ch.21.
 *
 * Error model: panic mode (CI Ch.6.5).  On the first error inside a
 * rule, we set panicMode and stop reporting *new* errors; the
 * top-level loops detect panicMode and call synchronize() to skip
 * until a known-safe token (a semicolon, a close-brace, or a keyword
 * that begins a fresh declaration), then clear the flag and continue.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"
#include "scanner.h"

typedef struct {
    Token current;
    Token previous;
    bool  hadError;
    bool  panicMode;
} Parser;

static Parser parser;

/* -------------------------------------------------- error reporting */

static void errorAt(const Token* token, const char* message) {
    if (parser.panicMode) return;       /* swallow cascading errors  */
    parser.panicMode = true;
    fprintf(stderr, "[line %d] Error", token->line);
    if (token->type == TOKEN_EOF) {
        fprintf(stderr, " at end");
    } else if (token->type != TOKEN_ERROR) {
        fprintf(stderr, " at '%.*s'", token->length, token->start);
    }
    fprintf(stderr, ": %s\n", message);
    parser.hadError = true;
}

static void error(const char* message) { errorAt(&parser.previous, message); }
static void errorAtCurrent(const char* m) { errorAt(&parser.current, m); }

/* ------------------------------------------------- token machinery */

static void advance(void) {
    parser.previous = parser.current;

    /* Context-sensitive lexing: if we just consumed `doc`, the next
     * thing in the source is a comment-style description body that
     * scanToken's skipWhitespace would normally throw away.  Route
     * through the specialised entry point instead.                   */
    if (parser.previous.type == TOKEN_DOC) {
        parser.current = scanDocBody();
        if (parser.current.type == TOKEN_ERROR) {
            errorAtCurrent(parser.current.start);
        }
        return;
    }

    for (;;) {
        parser.current = scanToken();
        if (parser.current.type != TOKEN_ERROR) break;
        /* Scanner reports its error message in the token's lexeme. */
        errorAtCurrent(parser.current.start);
    }
}

static bool check(TokenType type) { return parser.current.type == type; }

static bool match(TokenType type) {
    if (!check(type)) return false;
    advance();
    return true;
}

static void consume(TokenType type, const char* message) {
    if (check(type)) { advance(); return; }
    errorAtCurrent(message);
}

/* --------------------------------------------------- panic recovery
 *
 * Walk forward until we find a token where it's safe to start parsing
 * a fresh declaration: just past a ';', at a '}' that closes our
 * scope, or at a keyword that begins a new top-level form.
 */
static void synchronize(void) {
    parser.panicMode = false;
    while (!check(TOKEN_EOF)) {
        if (parser.previous.type == TOKEN_SEMICOLON) return;
        switch (parser.current.type) {
        case TOKEN_PACKAGE:
        case TOKEN_IMPORT:
        case TOKEN_PART:
        case TOKEN_PORT:
        case TOKEN_INTERFACE:
        case TOKEN_ITEM:
        case TOKEN_CONNECTION:
        case TOKEN_FLOW:
        case TOKEN_END:
        case TOKEN_CONNECT:
        case TOKEN_ATTRIBUTE:
        case TOKEN_DOC:
        case TOKEN_RIGHT_BRACE:
            return;
        default: ;
        }
        advance();
    }
}

/* ------------------------------------------------- forward decls */

static Node* declaration(void);
static Node* qualifiedName(void);

/* -------------------------------------- feature relationships helper
 *
 * Many SysML feature-bearing constructs (attributes, part usages,
 * part defs) can carry an optional combination of:
 *
 *      ':'  qname (',' qname)*       (typing list)
 *      ':>'  / 'specializes' qname (',' qname)*   (specialization list)
 *      ':>>' / 'redefines'   qname (',' qname)*   (redefinition list)
 *      '['  multiplicity ']'         (count / range)
 *
 * Each clause-keyword can appear at most once, but each carries a
 * comma-separated list of references.                                */
typedef struct {
    NodeList types;
    NodeList specializes;
    NodeList redefines;
    Node*    multiplicity;
} FeatureRels;

/* Convert a TOKEN_NUMBER's lexeme to a long.  We copy into a local
 * buffer because the token isn't null-terminated. */
static long tokenToLong(Token t) {
    char buf[64];
    int n = (t.length < 63) ? t.length : 63;
    memcpy(buf, t.start, (size_t)n);
    buf[n] = '\0';
    return strtol(buf, NULL, 10);
}

/*  multiplicity ::= "[" ( NUMBER | "*" ) ( ".." ( NUMBER | "*" ) )? "]"
 *  Caller has already consumed the opening '['.                       */
static Node* parseMultiplicity(void) {
    int line = parser.previous.line;
    Node* m = astMakeNode(NODE_MULTIPLICITY, line);
    /* All fields zeroed by astMakeNode's calloc. */

    /* ---- lower bound (or sole value) -------------------------------- */
    if (match(TOKEN_STAR)) {
        m->as.multiplicity.lowerWildcard = true;
    } else if (match(TOKEN_NUMBER)) {
        m->as.multiplicity.lower = tokenToLong(parser.previous);
    } else {
        errorAtCurrent("Expected number or '*' in multiplicity.");
    }

    /* ---- optional ".." upper bound ---------------------------------- */
    if (match(TOKEN_DOT_DOT)) {
        m->as.multiplicity.isRange = true;
        if (match(TOKEN_STAR)) {
            m->as.multiplicity.upperWildcard = true;
        } else if (match(TOKEN_NUMBER)) {
            m->as.multiplicity.upper = tokenToLong(parser.previous);
        } else {
            errorAtCurrent("Expected number or '*' after '..'.");
        }
    }

    consume(TOKEN_RIGHT_BRACKET, "Expected ']' to close multiplicity.");
    return m;
}

/* Helper: consume `qualifiedName (',' qualifiedName)*` into a list. */
static void appendQualifiedNameList(NodeList* list) {
    astListAppend(list, qualifiedName());
    while (match(TOKEN_COMMA)) {
        astListAppend(list, qualifiedName());
    }
}

/* ===================================================================
 * Pratt parser for expressions  (Crafting Interpreters Ch. 17).
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
 * =================================================================== */

typedef enum {
    PREC_NONE = 0,
    PREC_ASSIGNMENT,    /* = (we don't have assignment expressions yet) */
    PREC_OR,            /* ||                                            */
    PREC_AND,           /* &&                                            */
    PREC_EQUALITY,      /* == !=                                         */
    PREC_COMPARISON,    /* < <= > >=                                     */
    PREC_TERM,          /* + -                                           */
    PREC_FACTOR,        /* * /                                           */
    PREC_UNARY,         /* prefix - and !                                */
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
static Node* expression(void);
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

    while (prec <= getRule(parser.current.type)->precedence) {
        advance();
        InfixFn infix = getRule(parser.previous.type)->infix;
        left = infix(left);
    }
    return left;
}

static Node* expression(void) {
    return parsePrecedence(PREC_ASSIGNMENT);
}

/* =================================================================== */

static FeatureRels parseFeatureRelationships(void) {
    FeatureRels r = {0};
    for (;;) {
        if (match(TOKEN_LEFT_BRACKET)) {
            if (r.multiplicity) error("Duplicate multiplicity.");
            r.multiplicity = parseMultiplicity();
        } else if (match(TOKEN_COLON)) {
            if (r.types.count > 0) error("Duplicate type clause.");
            appendQualifiedNameList(&r.types);
        } else if (match(TOKEN_COLON_GREATER) || match(TOKEN_SPECIALIZES)) {
            if (r.specializes.count > 0) error("Duplicate 'specializes' clause.");
            appendQualifiedNameList(&r.specializes);
        } else if (match(TOKEN_COLON_GREATER_GREATER) || match(TOKEN_REDEFINES)) {
            if (r.redefines.count > 0) error("Duplicate 'redefines' clause.");
            appendQualifiedNameList(&r.redefines);
        } else {
            break;
        }
    }
    return r;
}

/* ============================================== grammar functions */

/*  qualifiedName ::= IDENTIFIER ( ( "::" | "." ) IDENTIFIER )*
 *
 *  We accept both '::' (namespace path, typical for type references)
 *  and '.' (member access, typical for endpoint references like
 *  's.reading') as separators, treating them uniformly in the AST.
 *  A future pass that distinguishes type refs from feature chains
 *  can add per-segment tracking; for now the printer always emits
 *  '::'.                                                              */
static Node* qualifiedName(void) {
    Node* q = astMakeNode(NODE_QUALIFIED_NAME, parser.current.line);
    if (!check(TOKEN_IDENTIFIER)) {
        errorAtCurrent("Expected name.");
        return q;
    }
    advance();
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

/*  importDecl ::= "import" IDENTIFIER ("::" IDENTIFIER)* ("::" "*")? ";"  */
static Node* importDecl(void) {
    int line = parser.previous.line;            /* 'import' was just consumed */

    Node* qn = astMakeNode(NODE_QUALIFIED_NAME, line);
    bool wildcard = false;

    if (!check(TOKEN_IDENTIFIER)) {
        errorAtCurrent("Expected name after 'import'.");
    } else {
        advance();
        astAppendQualifiedPart(qn, parser.previous);
        while (check(TOKEN_COLON_COLON)) {
            advance();                          /* eat ::            */
            if (check(TOKEN_STAR)) {
                advance();
                wildcard = true;
                break;                          /* :: * must terminate */
            }
            if (!check(TOKEN_IDENTIFIER)) {
                error("Expected name or '*' after '::'.");
                break;
            }
            advance();
            astAppendQualifiedPart(qn, parser.previous);
        }
    }

    consume(TOKEN_SEMICOLON, "Expected ';' after import.");

    Node* imp = astMakeNode(NODE_IMPORT, line);
    imp->as.import.target = qn;
    imp->as.import.wildcard = wildcard;
    return imp;
}

/*  attributeDecl ::= "attribute" IDENTIFIER featureRelationships? ";"
 *
 *  featureRelationships ::= ( ":" qname | (":>"|"specializes") qname
 *                                       | (":>>"|"redefines")  qname )+
 */
static Node* attributeDecl(void) {
    int line = parser.previous.line;            /* 'attribute' just consumed */
    consume(TOKEN_IDENTIFIER, "Expected attribute name.");
    Token name = parser.previous;

    FeatureRels rels = parseFeatureRelationships();

    /* Optional default value: `= expression`.  Anything that
     * `expression()` recognizes is fair game — arithmetic, comparison,
     * grouped subexpressions, identifier references.                  */
    Node* defaultValue = NULL;
    if (match(TOKEN_EQUAL)) {
        defaultValue = expression();
    }

    consume(TOKEN_SEMICOLON, "Expected ';' after attribute.");

    Node* a = astMakeNode(NODE_ATTRIBUTE, line);
    a->as.attribute.name         = name;
    a->as.attribute.types        = rels.types;
    a->as.attribute.specializes  = rels.specializes;
    a->as.attribute.redefines    = rels.redefines;
    a->as.attribute.multiplicity = rels.multiplicity;
    a->as.attribute.defaultValue = defaultValue;
    return a;
}

/*  partDef-style and partUsage-style rules — now generalized.
 *
 *  Seven kinds (part / port / interface / item / connection / flow /
 *  end) all share the same surface grammar.  We capture what varies
 *  between them in a small KindInfo struct and let one set of parsing
 *  functions consume it.                                              */
typedef struct {
    DefKind     kind;             /* AST tag stamped on the result      */
    bool        allowsDirection;  /* can the usage take in/out/inout?   */
    bool        defAllowed;       /* does this kind have a `def` form?  */
    bool        allowsAnonymous;  /* may the name be omitted?           */
    const char* humanName;        /* "part", "port", … for messages    */
} KindInfo;

static const KindInfo kPart       = { DEF_PART,       false, true,  false, "part"       };
static const KindInfo kPort       = { DEF_PORT,       true,  true,  false, "port"       };
static const KindInfo kInterface  = { DEF_INTERFACE,  false, true,  false, "interface"  };
static const KindInfo kItem       = { DEF_ITEM,       false, true,  false, "item"       };
static const KindInfo kConnection = { DEF_CONNECTION, false, true,  true,  "connection" };
static const KindInfo kFlow       = { DEF_FLOW,       false, true,  true,  "flow"       };
static const KindInfo kEnd        = { DEF_END,        false, false, false, "end"        };

/* Parse a "connect a to b" or "from a to b" clause.  Caller has
 * already consumed the opening keyword (CONNECT or FROM).  Appends
 * exactly two qualified-name nodes to the given list.                */
static void parseEndsClause(NodeList* ends, const char* hint) {
    astListAppend(ends, qualifiedName());
    consume(TOKEN_TO, "Expected 'to' in this clause.");
    (void)hint;
    astListAppend(ends, qualifiedName());
}

/*  definition ::= "def" IDENTIFIER featureRelationships? "{" declaration* "}"
 *  Caller has already consumed the kind keyword (e.g. "part", "port").   */
static Node* definition(const KindInfo* k) {
    int line = parser.previous.line;            /* kind keyword just consumed */
    advance();                                  /* eat 'def'                  */
    consume(TOKEN_IDENTIFIER, "Expected definition name.");
    Token name = parser.previous;

    FeatureRels rels = parseFeatureRelationships();

    consume(TOKEN_LEFT_BRACE, "Expected '{' after definition name.");

    Node* def = astMakeNode(NODE_DEFINITION, line);
    def->as.scope.name        = name;
    def->as.scope.defKind     = k->kind;
    def->as.scope.specializes = rels.specializes;
    def->as.scope.redefines   = rels.redefines;
    /* rels.types and rels.multiplicity are silently dropped on a
     * definition; semantic check will warn.                            */

    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        Node* m = declaration();
        if (m) astAppendScopeMember(def, m);
        if (parser.panicMode) synchronize();
    }
    consume(TOKEN_RIGHT_BRACE, "Expected '}' to close definition.");
    return def;
}

/*  usage ::= IDENTIFIER? featureRelationships? endpointsClause?
 *           ( "{" declaration* "}" | ";" )
 *
 *  The name is optional only for kinds whose KindInfo says so
 *  (currently connection and flow, which support anonymous "connect a
 *  to b" / "from a to b" forms).  The endpoint clause is parsed only
 *  for connection (`connect`) or flow (`from`) kinds.                */
static Node* usage(const KindInfo* k, Direction dir) {
    int line = parser.previous.line;

    /* ---- optional name ---------------------------------------------- */
    Token name = (Token){0};
    if (check(TOKEN_IDENTIFIER)) {
        advance();
        name = parser.previous;
    } else if (!k->allowsAnonymous
               || (k->kind == DEF_CONNECTION && !check(TOKEN_CONNECT))
               || (k->kind == DEF_FLOW       && !check(TOKEN_FROM))) {
        consume(TOKEN_IDENTIFIER, "Expected name.");
    }
    /* (Otherwise name stays zero-length — anonymous usage.)            */

    FeatureRels rels = parseFeatureRelationships();

    /* ---- optional endpoint clause ----------------------------------- */
    NodeList ends = {0};
    if (k->kind == DEF_CONNECTION && match(TOKEN_CONNECT)) {
        parseEndsClause(&ends, "connector");
    } else if (k->kind == DEF_FLOW && match(TOKEN_FROM)) {
        parseEndsClause(&ends, "flow");
    }

    Node* u = astMakeNode(NODE_USAGE, line);
    u->as.usage.name         = name;
    u->as.usage.defKind      = k->kind;
    u->as.usage.direction    = dir;
    u->as.usage.types        = rels.types;
    u->as.usage.specializes  = rels.specializes;
    u->as.usage.redefines    = rels.redefines;
    u->as.usage.multiplicity = rels.multiplicity;
    u->as.usage.ends         = ends;

    if (match(TOKEN_LEFT_BRACE)) {
        while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
            Node* m = declaration();
            if (m) astAppendUsageMember(u, m);
            if (parser.panicMode) synchronize();
        }
        consume(TOKEN_RIGHT_BRACE, "Expected '}' to close usage body.");
    } else {
        consume(TOKEN_SEMICOLON, "Expected ';' or '{' after usage.");
    }
    return u;
}

/*  Dispatch: caller has consumed the kind keyword (and any direction
 *  prefix).  If the next token is `def`, we're a definition; otherwise
 *  a usage.  A direction prefix is only meaningful on a usage; on a
 *  definition it's an error.  Kinds that don't have a `def` form
 *  (notably `end`) reject `def` here.                                 */
static Node* definitionOrUsage(const KindInfo* k, Direction dir) {
    if (check(TOKEN_DEF)) {
        if (!k->defAllowed) {
            errorAtCurrent("This kind has no 'def' form.");
            advance();              /* eat 'def' so we don't loop      */
        }
        if (dir != DIR_NONE) {
            error("Direction modifier is not valid on a definition.");
        }
        return definition(k);
    }
    if (dir != DIR_NONE && !k->allowsDirection) {
        error("Direction modifier is not valid on this kind of usage.");
    }
    return usage(k, dir);
}

/*  Standalone "connect a to b ;" — no preceding `connection` keyword.
 *  Produces an anonymous connection usage.                            */
static Node* connectStatement(void) {
    int line = parser.previous.line;            /* TOKEN_CONNECT consumed */
    Node* u = astMakeNode(NODE_USAGE, line);
    u->as.usage.defKind = DEF_CONNECTION;
    parseEndsClause(&u->as.usage.ends, "connector");
    consume(TOKEN_SEMICOLON, "Expected ';' after connect statement.");
    return u;
}

/*  packageDecl ::= "package" IDENTIFIER "{" declaration* "}"          */
static Node* packageDecl(void) {
    int line = parser.previous.line;            /* 'package' just consumed */
    consume(TOKEN_IDENTIFIER, "Expected package name.");
    Token name = parser.previous;
    consume(TOKEN_LEFT_BRACE, "Expected '{' after package name.");

    Node* p = astMakeNode(NODE_PACKAGE, line);
    p->as.scope.name = name;

    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        Node* m = declaration();
        if (m) astAppendScopeMember(p, m);
        if (parser.panicMode) synchronize();
    }
    consume(TOKEN_RIGHT_BRACE, "Expected '}' to close package.");
    return p;
}

/*  docDecl ::= "doc" SLASH_STAR body STAR_SLASH    explicit keyword form
 *           |  SLASH_STAR_STAR body STAR_SLASH      implicit form
 *
 *  Both produce a NODE_DOC carrying the captured body.               */
static Node* docDecl(void) {
    int line = parser.current.line;
    Token body;

    if (match(TOKEN_DOC)) {
        /* The advance() inside match() saw previous == TOKEN_DOC and
         * routed through scanDocBody, so parser.current should now be
         * the body (or an error token, already reported).            */
        if (parser.current.type != TOKEN_DOC_BODY) {
            return NULL;                /* error already reported     */
        }
        body = parser.current;
        advance();
    } else if (match(TOKEN_DOC_BODY)) {
        body = parser.previous;
    } else {
        errorAtCurrent("Expected doc.");
        advance();
        return NULL;
    }

    Node* d = astMakeNode(NODE_DOC, line);
    d->as.doc.body = body;
    return d;
}

/*  declaration ::= visibility? direction? ( docDecl | packageDecl
 *                                          | importDecl
 *                                          | partDecl    | portDecl
 *                                          | interfaceDecl | itemDecl
 *                                          | connectionDecl | flowDecl
 *                                          | attributeDecl )
 *  visibility  ::= "public" | "private" | "protected"
 *  direction   ::= "in" | "out" | "inout"
 *
 *  Visibility and direction are optional prefixes consumed once and
 *  threaded into the resulting node, so each grammar rule below stays
 *  ignorant of them.  A direction without a kind that allows it (or
 *  on a definition) becomes an error inside definitionOrUsage.        */
static Node* declaration(void) {
    /* Doc forms come first — they don't take visibility or direction. */
    if (check(TOKEN_DOC) || check(TOKEN_DOC_BODY)) return docDecl();

    Visibility vis = VIS_DEFAULT;
    if      (match(TOKEN_PUBLIC))    vis = VIS_PUBLIC;
    else if (match(TOKEN_PRIVATE))   vis = VIS_PRIVATE;
    else if (match(TOKEN_PROTECTED)) vis = VIS_PROTECTED;

    Direction dir = DIR_NONE;
    if      (match(TOKEN_IN))    dir = DIR_IN;
    else if (match(TOKEN_OUT))   dir = DIR_OUT;
    else if (match(TOKEN_INOUT)) dir = DIR_INOUT;

    Node* result = NULL;
    if (match(TOKEN_PACKAGE)) {
        if (dir != DIR_NONE) error("Direction modifier is not valid on a package.");
        result = packageDecl();
    } else if (match(TOKEN_IMPORT)) {
        if (dir != DIR_NONE) error("Direction modifier is not valid on an import.");
        result = importDecl();
    } else if (match(TOKEN_PART))       result = definitionOrUsage(&kPart,       dir);
    else if (match(TOKEN_PORT))         result = definitionOrUsage(&kPort,       dir);
    else if (match(TOKEN_INTERFACE))    result = definitionOrUsage(&kInterface,  dir);
    else if (match(TOKEN_ITEM))         result = definitionOrUsage(&kItem,       dir);
    else if (match(TOKEN_CONNECTION))   result = definitionOrUsage(&kConnection, dir);
    else if (match(TOKEN_FLOW))         result = definitionOrUsage(&kFlow,       dir);
    else if (match(TOKEN_END))          result = definitionOrUsage(&kEnd,        dir);
    else if (match(TOKEN_CONNECT)) {
        if (dir != DIR_NONE) error("Direction modifier is not valid on connect.");
        result = connectStatement();
    }
    else if (match(TOKEN_ATTRIBUTE)) {
        if (dir != DIR_NONE) error("Direction modifier is not valid on an attribute.");
        result = attributeDecl();
    } else {
        if (vis != VIS_DEFAULT) {
            errorAtCurrent("Visibility modifier must be followed by a declaration.");
        } else if (dir != DIR_NONE) {
            errorAtCurrent("Direction modifier must be followed by a declaration.");
        } else {
            errorAtCurrent("Expected declaration.");
        }
        advance();                  /* make progress so we don't loop */
        return NULL;
    }

    /* Visibility belongs only to declarations that can carry it; the
     * helper silently drops it for kinds that can't.                  */
    astSetVisibility(result, vis);
    return result;
}

/* ===================================================== entry point */

Node* parse(void) {
    parser.hadError = false;
    parser.panicMode = false;
    advance();                          /* prime the pump            */

    Node* root = astMakeNode(NODE_PROGRAM, 1);
    while (!check(TOKEN_EOF)) {
        Node* d = declaration();
        if (d) astAppendScopeMember(root, d);
        if (parser.panicMode) synchronize();
    }
    return root;
}

bool parserHadError(void) { return parser.hadError; }
