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

void astListAppend(NodeList* list, Node* item) {
    if (list->count >= list->capacity) {
        int newCap = (list->capacity < 4) ? 4 : list->capacity * 2;
        list->items = (Node**)realloc(list->items, sizeof(Node*) * (size_t)newCap);
        if (!list->items) { fprintf(stderr, "Out of memory\n"); exit(70); }
        list->capacity = newCap;
    }
    list->items[list->count++] = item;
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
    if (q->as.qualifiedName.isConjugated) printf("~");
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

static void emitAssertKind(AssertKind a) {
    switch (a) {
    case ASSERT_ASSERT:  printf("assert ");  break;
    case ASSERT_ASSUME:  printf("assume ");  break;
    case ASSERT_REQUIRE: printf("require "); break;
    case ASSERT_NONE:    /* nothing */       break;
    }
}

/* Emit feature-modifier flags in the canonical SysML v2 order:
 *   derived → abstract → constant → ref
 * Matches the RefPrefix/BasicUsagePrefix grammar so output reads in
 * the same order it would have been written in the source. */
static void emitFeatureModifiers(bool isDerived, bool isAbstract,
                                 bool isConstant, bool isReference) {
    if (isDerived)   printf("derived ");
    if (isAbstract)  printf("abstract ");
    if (isConstant)  printf("constant ");
    if (isReference) printf("ref ");
}

/* Map a DefKind to the human-readable label used in the AST dump.
 * The two arrays preserve the older "PartDef" / "Part" output that
 * existing test files compare against.                             */
static const char* kindLabel(DefKind k, bool isDefinition) {
    static const char* defs[] = {
        "PartDef", "PortDef", "InterfaceDef",
        "ItemDef", "ConnectionDef", "FlowDef",
        "?EndDef",  /* ends have no def form; sentinel for misuse  */
        "DataTypeDef",
        "EnumDef",
        "?ReferenceDef", /* references have no def form              */
        "ConstraintDef",
        "RequirementDef",
        "?SubjectDef",  /* subjects have no def form; sentinel        */
        "ActionDef",
        "StateDef",
        "CalcDef"
    };
    static const char* uses[] = {
        "Part", "Port", "Interface",
        "Item", "Connection", "Flow", "End",
        "DataType",  /* not produced by the parser, used by stdlib  */
        "EnumValue",
        "Reference",
        "Constraint",
        "Requirement",
        "Subject",
        "Action",
        "State",
        "Calc"
    };
    int idx = (int)k;
    if (idx < 0 || idx >= (int)(sizeof(defs)/sizeof(defs[0]))) return "?";
    return isDefinition ? defs[idx] : uses[idx];
}

/* Emit a comma-separated list of qualified names with a leading prefix
 * (e.g. " : " or " :> ").  Does nothing when the list is empty.       */
static void emitNameList(const char* prefix, const NodeList* list) {
    if (list->count == 0) return;
    printf("%s", prefix);
    for (int i = 0; i < list->count; i++) {
        if (i > 0) printf(", ");
        emitQualifiedName(list->items[i]);
    }
}

/* Emit the connector/flow endpoint clause when a usage carries one. */
static void emitEnds(DefKind k, const NodeList* ends) {
    if (ends->count == 0) return;
    const char* opener = (k == DEF_FLOW) ? " from " : " connect ";
    printf("%s", opener);
    /* For now the grammar produces exactly two ends; we'll relax later. */
    for (int i = 0; i < ends->count; i++) {
        if (i == 1) printf(" to ");
        else if (i > 1) printf(", ");
        emitQualifiedName(ends->items[i]);
    }
}

/* Map an operator token type to its source-form symbol.  Used by
 * emitExpression so the printer doesn't have to thread the lexeme
 * through (the operator's identity is known statically from its
 * type). */
static const char* operatorSymbol(TokenType t) {
    switch (t) {
    case TOKEN_PLUS:           return "+";
    case TOKEN_MINUS:          return "-";
    case TOKEN_STAR:           return "*";
    case TOKEN_STAR_STAR:      return "**";
    case TOKEN_SLASH:          return "/";
    case TOKEN_BANG:           return "!";
    case TOKEN_EQUAL_EQUAL:    return "==";
    case TOKEN_BANG_EQUAL:     return "!=";
    case TOKEN_LESS:           return "<";
    case TOKEN_LESS_EQUAL:     return "<=";
    case TOKEN_GREATER:        return ">";
    case TOKEN_GREATER_EQUAL:  return ">=";
    case TOKEN_AND:            return "and";
    case TOKEN_OR:             return "or";
    default:                   return "?";
    }
}

/* Pretty-print an expression inline.  Binary and unary nodes are
 * surrounded by parens so the output makes the parse tree's shape
 * unambiguous — useful for verifying that precedence is correct.
 * Identifiers reuse the qualified-name printer.                    */
static void emitExpression(const Node* e) {
    if (!e) { printf("(null)"); return; }
    switch (e->kind) {
    case NODE_LITERAL:
        /* String literals carry their surrounding quotes in the lexeme;
         * everything else (ints, reals, true, false) is printed as-is. */
        emitToken(e->as.literal.token);
        break;
    case NODE_QUALIFIED_NAME:
        emitQualifiedName(e);
        break;
    case NODE_BINARY:
        printf("(");
        emitExpression(e->as.binary.left);
        printf(" %s ", operatorSymbol(e->as.binary.op.type));
        emitExpression(e->as.binary.right);
        printf(")");
        break;
    case NODE_UNARY:
        printf("(%s", operatorSymbol(e->as.unary.op.type));
        emitExpression(e->as.unary.operand);
        printf(")");
        break;
    case NODE_CALL:
        emitExpression(e->as.call.callee);
        printf("(");
        for (int i = 0; i < e->as.call.args.count; i++) {
            if (i > 0) printf(", ");
            emitExpression(e->as.call.args.items[i]);
        }
        printf(")");
        break;
    case NODE_MEMBER_ACCESS:
        emitExpression(e->as.memberAccess.target);
        printf(".");
        emitToken(e->as.memberAccess.member);
        break;
    default:
        printf("?");
        break;
    }
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
        emitFeatureModifiers(false, n->as.scope.isAbstract, false, false);
        printf("%s '", kindLabel(n->as.scope.defKind, true));
        emitToken(n->as.scope.name);
        printf("'");
        emitNameList(" :> ",  &n->as.scope.specializes);
        emitNameList(" :>> ", &n->as.scope.redefines);
        if (n->as.scope.body) {
            printf(" body=");
            emitExpression(n->as.scope.body);
        }
        printf("\n");
        for (int i = 0; i < n->as.scope.memberCount; i++)
            printNode(n->as.scope.members[i], depth + 1);
        break;

    case NODE_USAGE:
        emitVisibility(n->as.usage.visibility);
        emitAssertKind(n->as.usage.assertKind);
        emitDirection(n->as.usage.direction);
        emitFeatureModifiers(n->as.usage.isDerived,  n->as.usage.isAbstract,
                             n->as.usage.isConstant, n->as.usage.isReference);
        if (n->as.usage.isPerform) printf("perform ");
        printf("%s", kindLabel(n->as.usage.defKind, false));
        if (n->as.usage.name.length > 0) {
            printf(" '"); emitToken(n->as.usage.name); printf("'");
        }
        emitNameList(" : ",   &n->as.usage.types);
        emitNameList(" :> ",  &n->as.usage.specializes);
        emitNameList(" :>> ", &n->as.usage.redefines);
        emitMultiplicity(n->as.usage.multiplicity);
        emitEnds(n->as.usage.defKind, &n->as.usage.ends);
        if (n->as.usage.defaultValue) {
            printf(" = ");
            emitExpression(n->as.usage.defaultValue);
        }
        if (n->as.usage.body) {
            printf(" {");
            emitExpression(n->as.usage.body);
            printf("}");
        }
        printf("\n");
        for (int i = 0; i < n->as.usage.memberCount; i++)
            printNode(n->as.usage.members[i], depth + 1);
        break;

    case NODE_ATTRIBUTE:
        emitVisibility(n->as.attribute.visibility);
        emitFeatureModifiers(n->as.attribute.isDerived,  n->as.attribute.isAbstract,
                             n->as.attribute.isConstant, n->as.attribute.isReference);
        printf("Attribute '"); emitToken(n->as.attribute.name); printf("'");
        emitNameList(" : ",   &n->as.attribute.types);
        emitNameList(" :> ",  &n->as.attribute.specializes);
        emitNameList(" :>> ", &n->as.attribute.redefines);
        emitMultiplicity(n->as.attribute.multiplicity);
        if (n->as.attribute.defaultValue) {
            printf(" = ");
            emitExpression(n->as.attribute.defaultValue);
        }
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

    case NODE_ALIAS:
        printf("Alias '%.*s' for ",
               n->as.alias.name.length, n->as.alias.name.start);
        emitQualifiedName(n->as.alias.target);
        printf("\n");
        break;

    case NODE_COMMENT:
        printf("Comment");
        if (n->as.comment.name.length > 0) {
            printf(" '%.*s'",
                   n->as.comment.name.length, n->as.comment.name.start);
        }
        if (n->as.comment.about.count > 0) {
            printf(" about ");
            for (int i = 0; i < n->as.comment.about.count; i++) {
                if (i) printf(", ");
                emitQualifiedName(n->as.comment.about.items[i]);
            }
        }
        /* Body preview only — same shape as DOC. */
        {
            Token b = n->as.comment.body;
            const char* s = b.start;
            const char* e = b.start + b.length;
            while (s < e && (*s == ' ' || *s == '\t' || *s == '\n')) s++;
            const char* nl = s;
            while (nl < e && *nl != '\n') nl++;
            printf(" \"%.*s%s\"\n",
                   (int)(nl - s), s,
                   (nl < e) ? " ..." : "");
        }
        break;

    case NODE_DEPENDENCY:
        emitVisibility(n->as.dependency.visibility);
        printf("Dependency");
        if (n->as.dependency.name.length > 0) {
            printf(" '%.*s'",
                   n->as.dependency.name.length, n->as.dependency.name.start);
        }
        printf(" from ");
        for (int i = 0; i < n->as.dependency.sources.count; i++) {
            if (i) printf(", ");
            emitQualifiedName(n->as.dependency.sources.items[i]);
        }
        printf(" to ");
        for (int i = 0; i < n->as.dependency.targets.count; i++) {
            if (i) printf(", ");
            emitQualifiedName(n->as.dependency.targets.items[i]);
        }
        printf("\n");
        break;

    case NODE_SUCCESSION:
        emitVisibility(n->as.succession.visibility);
        printf("Succession");
        if (n->as.succession.name.length > 0) {
            printf(" '%.*s'",
                   n->as.succession.name.length, n->as.succession.name.start);
        }
        if (n->as.succession.first) {
            printf(" first ");
            emitQualifiedName(n->as.succession.first);
        }
        for (int i = 0; i < n->as.succession.targets.count; i++) {
            const Node* t = n->as.succession.targets.items[i];
            printf(" then ");
            if (t && t->kind == NODE_QUALIFIED_NAME) {
                emitQualifiedName(t);
            } else if (t && t->kind == NODE_USAGE) {
                /* Inline action declaration. */
                printf("(action");
                if (t->as.usage.name.length > 0) {
                    printf(" '%.*s'", t->as.usage.name.length,
                                       t->as.usage.name.start);
                }
                printf(")");
            }
        }
        printf("\n");
        /* Print inline declarations as children for inspection. */
        for (int i = 0; i < n->as.succession.targets.count; i++) {
            const Node* t = n->as.succession.targets.items[i];
            if (t && t->kind == NODE_USAGE) {
                printNode(t, depth + 1);
            }
        }
        break;

    case NODE_TRANSITION:
        emitVisibility(n->as.transition.visibility);
        printf("Transition");
        if (n->as.transition.name.length > 0) {
            printf(" '%.*s'",
                   n->as.transition.name.length, n->as.transition.name.start);
        }
        if (n->as.transition.first) {
            printf(" first ");
            emitQualifiedName(n->as.transition.first);
        }
        if (n->as.transition.accept) {
            printf(" accept ");
            emitQualifiedName(n->as.transition.accept);
        }
        if (n->as.transition.guard) {
            printf(" if ");
            emitExpression(n->as.transition.guard);
        }
        if (n->as.transition.effect) {
            printf(" do ");
            emitQualifiedName(n->as.transition.effect);
        }
        printf(" then ");
        if (n->as.transition.target) emitQualifiedName(n->as.transition.target);
        printf("\n");
        break;

    case NODE_LIFECYCLE_ACTION:
        switch (n->as.lifecycleAction.kind) {
        case LIFECYCLE_ENTRY: printf("entry "); break;
        case LIFECYCLE_DO:    printf("do ");    break;
        case LIFECYCLE_EXIT:  printf("exit ");  break;
        }
        if (n->as.lifecycleAction.action) {
            emitQualifiedName(n->as.lifecycleAction.action);
        }
        printf("\n");
        break;

    case NODE_RETURN:
        printf("return");
        if (n->as.ret.name.length > 0) {
            printf(" '%.*s'", n->as.ret.name.length, n->as.ret.name.start);
        }
        emitNameList(" : ",  &n->as.ret.types);
        emitNameList(" :> ", &n->as.ret.specializes);
        if (n->as.ret.defaultValue) {
            printf(" = ");
            emitExpression(n->as.ret.defaultValue);
        }
        printf("\n");
        break;

    /* The expression kinds are normally embedded inside another
     * node (an attribute's default value, eventually a multiplicity
     * bound, etc.) and rendered there via emitExpression.  These cases
     * keep the switch exhaustive — they only fire when astPrint is
     * called on a bare expression, which we currently don't do.       */
    case NODE_LITERAL:
    case NODE_BINARY:
    case NODE_UNARY:
    case NODE_CALL:
    case NODE_MEMBER_ACCESS:
        emitExpression(n);
        printf("\n");
        break;
    }
}

void astPrint(const Node* root) { printNode(root, 0); }
