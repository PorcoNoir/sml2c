/* sysmlc — ast.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"

/* ----------------------------------------------------------- alloc */

Node* astMakeNode(NodeKind kind, int line) {
    Node* n = (Node*)calloc(1, sizeof(Node));
    if (!n) { fprintf(stderr, "Out of memory\n"); exit(70); }
    n->kind = kind;
    n->line = line;
    return n;
}

/* Generic capacity-doubling growth for a Node** array.
 * CI uses GROW_CAPACITY/GROW_ARRAY macros; this is the same idea
 * with explicit functions for clarity. */
static void growMembers(Node*** members, int* cap) {
    int newCap = (*cap < 8) ? 8 : (*cap) * 2;
    *members = (Node**)realloc(*members, sizeof(Node*) * (size_t)newCap);
    if (!*members) { fprintf(stderr, "Out of memory\n"); exit(70); }
    *cap = newCap;
}

void astAppendScopeMember(Node* scope, Node* member) {
    if (scope->as.scope.memberCount >= scope->as.scope.memberCapacity) {
        growMembers(&scope->as.scope.members, &scope->as.scope.memberCapacity);
    }
    scope->as.scope.members[scope->as.scope.memberCount++] = member;
}

void astAppendUsageMember(Node* usage, Node* member) {
    if (usage->as.usage.memberCount >= usage->as.usage.memberCapacity) {
        growMembers(&usage->as.usage.members, &usage->as.usage.memberCapacity);
    }
    usage->as.usage.members[usage->as.usage.memberCount++] = member;
}

void astAppendQualifiedPart(Node* qname, Token part) {
    if (qname->as.qualifiedName.partCount >= qname->as.qualifiedName.partCapacity) {
        int newCap = (qname->as.qualifiedName.partCapacity < 4)
                         ? 4 : qname->as.qualifiedName.partCapacity * 2;
        qname->as.qualifiedName.parts = (Token*)realloc(
            qname->as.qualifiedName.parts, sizeof(Token) * (size_t)newCap);
        if (!qname->as.qualifiedName.parts) {
            fprintf(stderr, "Out of memory\n"); exit(70);
        }
        qname->as.qualifiedName.partCapacity = newCap;
    }
    qname->as.qualifiedName.parts[qname->as.qualifiedName.partCount++] = part;
}

/* ---------------------------------------------------------- print */

static void emitIndent(int depth) {
    for (int i = 0; i < depth; i++) printf("  ");
}

static void emitToken(Token t) {
    printf("%.*s", t.length, t.start);
}

static void emitQualifiedName(const Node* q) {
    for (int i = 0; i < q->as.qualifiedName.partCount; i++) {
        if (i > 0) printf("::");
        emitToken(q->as.qualifiedName.parts[i]);
    }
}

static void emitMultiplicity(const Node* m) {
    if (!m) return;
    printf(" [");
    if (m->as.multiplicity.lowerWildcard) printf("*");
    else                                  printf("%ld", m->as.multiplicity.lower);
    if (m->as.multiplicity.isRange) {
        printf("..");
        if (m->as.multiplicity.upperWildcard) printf("*");
        else                                  printf("%ld", m->as.multiplicity.upper);
    }
    printf("]");
}

static void printNode(const Node* n, int depth) {
    if (!n) { emitIndent(depth); printf("(null)\n"); return; }
    emitIndent(depth);

    switch (n->kind) {
    case NODE_PROGRAM:
        printf("Program\n");
        for (int i = 0; i < n->as.scope.memberCount; i++)
            printNode(n->as.scope.members[i], depth + 1);
        break;

    case NODE_PACKAGE:
        printf("Package '"); emitToken(n->as.scope.name); printf("'\n");
        for (int i = 0; i < n->as.scope.memberCount; i++)
            printNode(n->as.scope.members[i], depth + 1);
        break;

    case NODE_PART_DEF:
        printf("PartDef '"); emitToken(n->as.scope.name); printf("'");
        if (n->as.scope.specializes) {
            printf(" :> ");
            emitQualifiedName(n->as.scope.specializes);
        }
        if (n->as.scope.redefines) {
            printf(" :>> ");
            emitQualifiedName(n->as.scope.redefines);
        }
        printf("\n");
        for (int i = 0; i < n->as.scope.memberCount; i++)
            printNode(n->as.scope.members[i], depth + 1);
        break;

    case NODE_PART_USAGE:
        printf("Part '"); emitToken(n->as.usage.name); printf("'");
        if (n->as.usage.type) {
            printf(" : ");
            emitQualifiedName(n->as.usage.type);
        }
        if (n->as.usage.specializes) {
            printf(" :> ");
            emitQualifiedName(n->as.usage.specializes);
        }
        if (n->as.usage.redefines) {
            printf(" :>> ");
            emitQualifiedName(n->as.usage.redefines);
        }
        emitMultiplicity(n->as.usage.multiplicity);
        printf("\n");
        for (int i = 0; i < n->as.usage.memberCount; i++)
            printNode(n->as.usage.members[i], depth + 1);
        break;

    case NODE_ATTRIBUTE:
        printf("Attribute '"); emitToken(n->as.attribute.name); printf("'");
        if (n->as.attribute.type) {
            printf(" : ");
            emitQualifiedName(n->as.attribute.type);
        }
        if (n->as.attribute.specializes) {
            printf(" :> ");
            emitQualifiedName(n->as.attribute.specializes);
        }
        if (n->as.attribute.redefines) {
            printf(" :>> ");
            emitQualifiedName(n->as.attribute.redefines);
        }
        emitMultiplicity(n->as.attribute.multiplicity);
        printf("\n");
        break;

    case NODE_IMPORT:
        printf("Import ");
        emitQualifiedName(n->as.import.target);
        if (n->as.import.wildcard) printf("::*");
        printf("\n");
        break;

    case NODE_QUALIFIED_NAME:
        emitQualifiedName(n);
        printf("\n");
        break;

    case NODE_MULTIPLICITY:
        /* Always rendered inline by the parent via emitMultiplicity().
         * This case exists only so the switch is exhaustive.          */
        break;
    }
}

void astPrint(const Node* root) { printNode(root, 0); }