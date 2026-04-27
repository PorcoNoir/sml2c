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
    NODE_PART_DEF,        /* part def Name { ... }                       */
    NODE_PART_USAGE,      /* part p : Type;  or  part p : Type { ... }   */
    NODE_ATTRIBUTE,       /* attribute name : Type;                      */
    NODE_QUALIFIED_NAME   /* A::B::C  (used as a type ref or import tgt) */
} NodeKind;

typedef struct Node Node;

struct Node {
    NodeKind kind;
    int      line;
    union {
        /* PROGRAM, PACKAGE, PART_DEF — all "named scopes" with members. */
        struct {
            Token  name;            /* unused for PROGRAM                */
            Node** members;
            int    memberCount;
            int    memberCapacity;
            Node*  specializes;     /* `:>`  / `specializes` (PART_DEF)  */
            Node*  redefines;       /* `:>>` / `redefines`   (PART_DEF)  */
        } scope;

        /* PART_USAGE — has an optional type and an optional body.       */
        struct {
            Token  name;
            Node*  type;            /* `:`   NULL if no `: Type`         */
            Node*  specializes;     /* `:>`  / `specializes`             */
            Node*  redefines;       /* `:>>` / `redefines`               */
            Node** members;         /* NULL if no `{ ... }`              */
            int    memberCount;
            int    memberCapacity;
        } usage;

        /* ATTRIBUTE — name + optional type/specializes/redefines.      */
        struct {
            Token name;
            Node* type;             /* `:`   NULL if untyped             */
            Node* specializes;      /* `:>`  / `specializes`             */
            Node* redefines;        /* `:>>` / `redefines`               */
        } attribute;

        /* IMPORT — points to a QUALIFIED_NAME, plus a wildcard flag.    */
        struct {
            Node* target;
            bool  wildcard;
        } import;

        /* QUALIFIED_NAME — array of identifier tokens (A::B::C → 3).    */
        struct {
            Token* parts;
            int    partCount;
            int    partCapacity;
        } qualifiedName;
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

/* ---- inspection ---- */
void  astPrint(const Node* root);

#endif /* SYSMLC_AST_H */
