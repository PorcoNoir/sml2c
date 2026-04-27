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
        case TOKEN_ATTRIBUTE:
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
 *      ':'  qualifiedName            (typing)
 *      ':>'  / 'specializes' qname   (subsetting / specialization)
 *      ':>>' / 'redefines'   qname   (redefinition)
 *
 * The clauses can appear in any order, but each at most once.  This
 * helper consumes whatever clauses are present, fills in the FeatureRels
 * struct, and returns when the next token isn't a relationship opener.
 */
typedef struct {
    Node* type;
    Node* specializes;
    Node* redefines;
} FeatureRels;

static FeatureRels parseFeatureRelationships(void) {
    FeatureRels r = { NULL, NULL, NULL };
    for (;;) {
        if (match(TOKEN_COLON)) {
            if (r.type) error("Duplicate type clause.");
            r.type = qualifiedName();
        } else if (match(TOKEN_COLON_GREATER) || match(TOKEN_SPECIALIZES)) {
            if (r.specializes) error("Duplicate 'specializes' clause.");
            r.specializes = qualifiedName();
        } else if (match(TOKEN_COLON_GREATER_GREATER) || match(TOKEN_REDEFINES)) {
            if (r.redefines) error("Duplicate 'redefines' clause.");
            r.redefines = qualifiedName();
        } else {
            break;
        }
    }
    return r;
}

/* ============================================== grammar functions */

/*  qualifiedName ::= IDENTIFIER ( "::" IDENTIFIER )*  */
static Node* qualifiedName(void) {
    Node* q = astMakeNode(NODE_QUALIFIED_NAME, parser.current.line);
    if (!check(TOKEN_IDENTIFIER)) {
        errorAtCurrent("Expected name.");
        return q;
    }
    advance();
    astAppendQualifiedPart(q, parser.previous);
    while (check(TOKEN_COLON_COLON)) {
        /* Peek-via-advance trick: we can only know what follows '::'
         * by consuming it. For plain qualifiedName (not import), only
         * an identifier is valid here, so if we see COLON_COLON we
         * commit to consuming '::' IDENTIFIER. */
        advance();                              /* eat ::            */
        if (!check(TOKEN_IDENTIFIER)) {
            error("Expected name after '::'.");
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

    consume(TOKEN_SEMICOLON, "Expected ';' after attribute.");

    Node* a = astMakeNode(NODE_ATTRIBUTE, line);
    a->as.attribute.name        = name;
    a->as.attribute.type        = rels.type;
    a->as.attribute.specializes = rels.specializes;
    a->as.attribute.redefines   = rels.redefines;
    return a;
}

/*  partDef ::= "def" IDENTIFIER featureRelationships? "{" declaration* "}"
 *  Caller has already consumed "part".  Note that `:` (typing) is not
 *  meaningful for a definition, but we accept it here and let later
 *  semantic analysis reject it; the parser stays simple.            */
static Node* partDef(void) {
    int line = parser.previous.line;            /* 'part' just consumed */
    advance();                                  /* eat 'def'          */
    consume(TOKEN_IDENTIFIER, "Expected part definition name.");
    Token name = parser.previous;

    FeatureRels rels = parseFeatureRelationships();

    consume(TOKEN_LEFT_BRACE, "Expected '{' after part definition name.");

    Node* def = astMakeNode(NODE_PART_DEF, line);
    def->as.scope.name        = name;
    def->as.scope.specializes = rels.specializes;
    def->as.scope.redefines   = rels.redefines;
    /* rels.type is silently dropped for part def — semantic check will warn. */

    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        Node* m = declaration();
        if (m) astAppendScopeMember(def, m);
        if (parser.panicMode) synchronize();
    }
    consume(TOKEN_RIGHT_BRACE, "Expected '}' to close part definition.");
    return def;
}

/*  partUsage ::= IDENTIFIER featureRelationships? ( "{" declaration* "}" | ";" )
 *  Caller has already consumed "part".                                */
static Node* partUsage(void) {
    int line = parser.previous.line;            /* 'part' just consumed */
    consume(TOKEN_IDENTIFIER, "Expected part name.");
    Token name = parser.previous;

    FeatureRels rels = parseFeatureRelationships();

    Node* u = astMakeNode(NODE_PART_USAGE, line);
    u->as.usage.name        = name;
    u->as.usage.type        = rels.type;
    u->as.usage.specializes = rels.specializes;
    u->as.usage.redefines   = rels.redefines;

    if (match(TOKEN_LEFT_BRACE)) {
        while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
            Node* m = declaration();
            if (m) astAppendUsageMember(u, m);
            if (parser.panicMode) synchronize();
        }
        consume(TOKEN_RIGHT_BRACE, "Expected '}' to close part body.");
    } else {
        consume(TOKEN_SEMICOLON, "Expected ';' or '{' after part declaration.");
    }
    return u;
}

/*  partDecl ::= "part" ( partDef | partUsage )                        */
static Node* partDecl(void) {
    /* 'part' has just been consumed.  Look at current to disambiguate. */
    if (check(TOKEN_DEF)) return partDef();
    return partUsage();
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

/*  declaration ::= packageDecl | importDecl | partDecl | attributeDecl */
static Node* declaration(void) {
    if (match(TOKEN_PACKAGE))   return packageDecl();
    if (match(TOKEN_IMPORT))    return importDecl();
    if (match(TOKEN_PART))      return partDecl();
    if (match(TOKEN_ATTRIBUTE)) return attributeDecl();

    errorAtCurrent("Expected declaration.");
    advance();                          /* make progress so we don't loop */
    return NULL;
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