/* sysmlc — ast.h
 *
 * Abstract Syntax Tree.
 *
 * Style: tagged union ("sum type"), one Node struct, NodeKind picks
 * which variant of the inner anonymous union is valid.  This is the
 * common idiom for hand-written C compilers (Lua, Tcl, GCC's older
 * tree code).  Crafting Interpreters' jlox uses Java class hierarchy
 * + visitor; clox's compiler skips the AST entirely and emits bytecode
 * during parse.  We're going AST-first because we'll want to re-walk
 * it later for type checking and unit checking.
 *
 * Tokens carried inside nodes are *value copies* of the scanner's
 * Token.  Token::start still points into the original source buffer,
 * which the caller must keep alive for the lifetime of the AST.
 */
#ifndef SYSMLC_AST_H
#define SYSMLC_AST_H

#include <stdbool.h>
#include "scanner.h"

typedef enum {
    NODE_PROGRAM,         /* top-level container, never written by user */
    NODE_PACKAGE,         /* package Foo { ... }                         */
    NODE_IMPORT,          /* import A::B::C; or import A::B::*;          */
    NODE_DEFINITION,      /* part/port/interface/item/connection/flow def Name { ... } */
    NODE_USAGE,           /* part/port/interface/item/connection/flow name : Type [...] */
    NODE_ATTRIBUTE,       /* attribute name : Type;                      */
    NODE_QUALIFIED_NAME,  /* A::B::C  (used as a type ref or import tgt) */
    NODE_MULTIPLICITY,    /* [n], [lo..hi], [*], [lo..*]                 */
    NODE_DOC,             /* doc keyword form, or slash-star-star form */
    NODE_LITERAL,         /* numeric/string/bool literal in an expression */
    NODE_BINARY,          /* left op right (+, -, *, /, ==, !=, <, ...)   */
    NODE_UNARY            /* prefix - or !                                */
} NodeKind;

/* Which flavor of literal a NODE_LITERAL holds.  String literals don't
 * need a parsed value field — the token's lexeme serves directly. */
typedef enum {
    LIT_INT  = 0,
    LIT_REAL,
    LIT_STRING,
    LIT_BOOL
} LiteralKind;

/* Which family a definition or usage belongs to.  All six kinds share
 * the same surface grammar (`<keyword> def Name {…}` and `<keyword> name
 * : Type;`) so we tag them with a DefKind rather than minting distinct
 * NodeKind values for each.                                             */
typedef enum {
    DEF_PART = 0,
    DEF_PORT,
    DEF_INTERFACE,
    DEF_ITEM,
    DEF_CONNECTION,
    DEF_FLOW,
    DEF_END                /* `end name : Type;` inside a connection/flow def */
} DefKind;

/* Port direction modifier.  Currently only port usages can carry one,
 * but we put it on the usage variant uniformly so the parser doesn't
 * need a separate field for it.                                        */
typedef enum {
    DIR_NONE = 0,
    DIR_IN,
    DIR_OUT,
    DIR_INOUT
} Direction;

/* Visibility modifier on a declaration.  VIS_DEFAULT means no modifier
 * appeared in source — SysML treats that as effectively `public` at
 * package scope, but we keep it distinct so the printer can echo
 * source faithfully. */
typedef enum {
    VIS_DEFAULT = 0,      /* no modifier in source            */
    VIS_PUBLIC,
    VIS_PRIVATE,
    VIS_PROTECTED
} Visibility;

/* Dynamic array of Node*.  Used for relationship lists (`: A, B`,
 * `:> X, Y`) and for connector/flow endpoints.                        */
typedef struct {
    struct Node** items;
    int           count;
    int           capacity;
} NodeList;

typedef struct Node Node;

struct Node {
    NodeKind kind;
    int      line;
    union {
        /* PROGRAM, PACKAGE, DEFINITION — all "named scopes" with members.
         * defKind is meaningful only for DEFINITION; the other two leave
         * it at zero (DEF_PART), which is harmless because they're never
         * dispatched through the kind-aware printer.                    */
        struct {
            Token      name;        /* unused for PROGRAM                */
            Visibility visibility;
            DefKind    defKind;
            Node**     members;
            int        memberCount;
            int        memberCapacity;
            NodeList   specializes; /* `:>`  / `specializes` (DEFINITION) */
            NodeList   redefines;   /* `:>>` / `redefines`   (DEFINITION) */
        } scope;

        /* USAGE — has optional name (anonymous for bare `connect a to b`),
         * optional type/spec/redef lists, optional multiplicity, optional
         * connector or flow endpoint clause, and an optional body.       */
        struct {
            Token      name;        /* length==0 means anonymous          */
            Visibility visibility;
            DefKind    defKind;
            Direction  direction;   /* in/out/inout (port usages mostly)  */
            NodeList   types;       /* `: A, B`                           */
            NodeList   specializes; /* `:> X, Y`                          */
            NodeList   redefines;   /* `:>> P, Q`                         */
            Node*      multiplicity;/* `[...]`  NULL if not specified     */
            NodeList   ends;        /* connector/flow endpoints (2 elts)  */
            Node**     members;     /* NULL if no `{ ... }`               */
            int        memberCount;
            int        memberCapacity;
        } usage;

        /* ATTRIBUTE — name + optional type/specializes/redefines.      */
        struct {
            Token      name;
            Visibility visibility;
            NodeList   types;       /* `: A, B`                          */
            NodeList   specializes; /* `:> X, Y`                         */
            NodeList   redefines;   /* `:>> P, Q`                        */
            Node*      multiplicity;/* `[...]`  NULL if not specified    */
            Node*      defaultValue;/* `= expr`  NULL if not specified   */
        } attribute;

        /* IMPORT — points to a QUALIFIED_NAME, plus a wildcard flag.    */
        struct {
            Visibility visibility;
            Node*      target;
            bool       wildcard;
        } import;

        /* QUALIFIED_NAME — array of identifier tokens (A::B::C → 3).    */
        struct {
            Token* parts;
            int    partCount;
            int    partCapacity;
        } qualifiedName;

        /* MULTIPLICITY — `[n]`, `[lo..hi]`, `[*]`, `[lo..*]`.
         *
         * For `[3]`:        lower=3,  upper=0,  isRange=false
         * For `[1..5]`:     lower=1,  upper=5,  isRange=true
         * For `[0..*]`:     lower=0,  upperWildcard=true, isRange=true
         * For `[*]`:        lowerWildcard=true, isRange=false
         *
         * Once expressions land, lower/upper become Node* expression
         * trees; for now they're just integer literals.               */
        struct {
            long lower;
            long upper;
            bool lowerWildcard;
            bool upperWildcard;
            bool isRange;
        } multiplicity;

        /* DOC — captured body of a doc-keyword form or implicit form.
         * The token lexeme is the inner content with delimiters
         * stripped, so printing is straightforward.                  */
        struct {
            Token body;
        } doc;

        /* LITERAL — numeric/string/bool.  The token holds the lexeme
         * and source line; the value union holds the parsed scalar
         * (string literals read directly from the token).            */
        struct {
            Token       token;
            LiteralKind litKind;
            union {
                long long iv;
                double    rv;
                bool      bv;
            } value;
        } literal;

        /* BINARY — left op right.  The op token records both the
         * operator type (TOKEN_PLUS, TOKEN_STAR, ...) and its source
         * location.                                                 */
        struct {
            Token op;
            Node* left;
            Node* right;
        } binary;

        /* UNARY — prefix `-` or `!` applied to an operand.          */
        struct {
            Token op;
            Node* operand;
        } unary;
    } as;
};

/* ---- construction ---- */
Node* astMakeNode(NodeKind kind, int line);

/* For NODE_PROGRAM / NODE_PACKAGE / NODE_PART_DEF: */
void  astAppendScopeMember(Node* scope, Node* member);
/* For NODE_PART_USAGE: */
void  astAppendUsageMember(Node* usage, Node* member);
/* For NODE_QUALIFIED_NAME: */
void  astAppendQualifiedPart(Node* qname, Token part);
/* Append a Node* to any NodeList (relationship lists, ends list). */
void  astListAppend(NodeList* list, Node* item);
/* Set a visibility modifier on whichever variant supports one. */
void  astSetVisibility(Node* node, Visibility v);

/* ---- inspection ---- */
void  astPrint(const Node* root);

#endif /* SYSMLC_AST_H */
