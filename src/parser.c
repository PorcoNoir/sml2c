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

    consume(TOKEN_SEMICOLON, "Expected ';' after attribute.");

    Node* a = astMakeNode(NODE_ATTRIBUTE, line);
    a->as.attribute.name         = name;
    a->as.attribute.types        = rels.types;
    a->as.attribute.specializes  = rels.specializes;
    a->as.attribute.redefines    = rels.redefines;
    a->as.attribute.multiplicity = rels.multiplicity;
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
