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

void astSetVisibility(Node* n, Visibility v) {
    if (!n || v == VIS_DEFAULT) return;
    switch (n->kind) {
    case NODE_PACKAGE:
    case NODE_DEFINITION:  n->as.scope.visibility     = v; break;
    case NODE_USAGE:       n->as.usage.visibility     = v; break;
    case NODE_ATTRIBUTE:   n->as.attribute.visibility = v; break;
    case NODE_IMPORT:      n->as.import.visibility    = v; break;
    default: break;        /* PROGRAM, QUALIFIED_NAME, MULTIPLICITY, DOC: none */
    }
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

static void emitVisibility(Visibility v) {
    switch (v) {
    case VIS_PUBLIC:    printf("public ");    break;
    case VIS_PRIVATE:   printf("private ");   break;
    case VIS_PROTECTED: printf("protected "); break;
    case VIS_DEFAULT:   /* nothing */         break;
    }
}

static void emitDirection(Direction d) {
    switch (d) {
    case DIR_IN:    printf("in ");    break;
    case DIR_OUT:   printf("out ");   break;
    case DIR_INOUT: printf("inout "); break;
    case DIR_NONE:  /* nothing */     break;
    }
}

/* Map a DefKind to the human-readable label used in the AST dump.
 * The two arrays preserve the older "PartDef" / "Part" output that
 * existing test files compare against.                             */
static const char* kindLabel(DefKind k, bool isDefinition) {
    static const char* defs[] = {
        "PartDef", "PortDef", "InterfaceDef",
        "ItemDef", "ConnectionDef", "FlowDef"
    };
    static const char* uses[] = {
        "Part", "Port", "Interface",
        "Item", "Connection", "Flow"
    };
    int idx = (int)k;
    if (idx < 0 || idx >= (int)(sizeof(defs)/sizeof(defs[0]))) return "?";
    return isDefinition ? defs[idx] : uses[idx];
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
        emitVisibility(n->as.scope.visibility);
        printf("Package '"); emitToken(n->as.scope.name); printf("'\n");
        for (int i = 0; i < n->as.scope.memberCount; i++)
            printNode(n->as.scope.members[i], depth + 1);
        break;

    case NODE_DEFINITION:
        emitVisibility(n->as.scope.visibility);
        printf("%s '", kindLabel(n->as.scope.defKind, true));
        emitToken(n->as.scope.name);
        printf("'");
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

    case NODE_USAGE:
        emitVisibility(n->as.usage.visibility);
        emitDirection(n->as.usage.direction);
        printf("%s '", kindLabel(n->as.usage.defKind, false));
        emitToken(n->as.usage.name);
        printf("'");
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
        emitVisibility(n->as.attribute.visibility);
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
        emitVisibility(n->as.import.visibility);
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

    case NODE_DOC: {
        /* Render the body inline-friendly: trim, single-line preview
         * with " ..." when truncated.  The AST stores the full body. */
        Token b = n->as.doc.body;
        const char* start = b.start;
        const char* end   = b.start + b.length;
        /* Trim leading whitespace */
        while (start < end && (*start == ' ' || *start == '\t' || *start == '\n')) start++;
        /* Find first newline (if any) */
        const char* nl = start;
        while (nl < end && *nl != '\n') nl++;
        printf("Doc \"%.*s%s\"\n",
               (int)(nl - start), start,
               (nl < end) ? " ..." : "");
        break;
    }
    }
}

void astPrint(const Node* root) { printNode(root, 0); }
