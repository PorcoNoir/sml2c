/*  ast_walk.c — centralized AST descent.  See `ast_walk.h` for the
 *  visitor protocol; this file only implements the walker. */

#include "ast_walk.h"

#include <stddef.h>

static void walk(AstWalkCtx* ctx, Node* n);

static void walkScopeMembers(AstWalkCtx* ctx, Node* n) {
    /* PROGRAM, PACKAGE, and DEFINITION all store their members in the
     * `as.scope` union arm.  USAGE stores them separately in
     * `as.usage`; the caller dispatches accordingly.                 */
    for (int i = 0; i < n->as.scope.memberCount; i++) {
        walk(ctx, n->as.scope.members[i]);
    }
}

static void walkUsageMembers(AstWalkCtx* ctx, Node* n) {
    for (int i = 0; i < n->as.usage.memberCount; i++) {
        walk(ctx, n->as.usage.members[i]);
    }
}

static void walkSuccessionTargets(AstWalkCtx* ctx, Node* n) {
    /* A succession's targets list mixes qualified-name references
     * (which are leaves) with inline action declarations (which are
     * NODE_USAGE subtrees that need visiting).  Skip the qnames
     * because the framework only walks structural nodes; per-pass
     * hooks deal with reference resolution.                          */
    for (int i = 0; i < n->as.succession.targets.count; i++) {
        Node* t = n->as.succession.targets.items[i];
        if (t && t->kind != NODE_QUALIFIED_NAME) walk(ctx, t);
    }
}

#define CALL(fn, n) do { if ((fn)) (fn)(ctx, (n)); } while (0)

static void walk(AstWalkCtx* ctx, Node* n) {
    if (!n) return;
    const AstVisitor* v = ctx->visitor;

    switch (n->kind) {
    case NODE_PROGRAM:
        CALL(v->onProgramEnter, n);
        walkScopeMembers(ctx, n);
        CALL(v->onProgramLeave, n);
        break;

    case NODE_PACKAGE:
        CALL(v->onPackageEnter, n);
        walkScopeMembers(ctx, n);
        CALL(v->onPackageLeave, n);
        break;

    case NODE_DEFINITION: {
        const Node* prev = ctx->enclosing;
        CALL(v->onDefinitionEnter, n);
        ctx->enclosing = n;
        walkScopeMembers(ctx, n);
        ctx->enclosing = prev;
        CALL(v->onDefinitionLeave, n);
        break;
    }

    case NODE_USAGE: {
        const Node* prev = ctx->enclosing;
        CALL(v->onUsageEnter, n);
        ctx->enclosing = n;
        walkUsageMembers(ctx, n);
        ctx->enclosing = prev;
        CALL(v->onUsageLeave, n);
        break;
    }

    case NODE_ATTRIBUTE:
        CALL(v->onAttribute, n);
        break;

    case NODE_SUCCESSION:
        CALL(v->onSuccession, n);
        walkSuccessionTargets(ctx, n);
        break;

    default:
        /* Other kinds (qualified names, expressions, ends, etc.) are
         * not container nodes that any current pass walks generically;
         * they are reached via the per-callback inspection of the
         * containing node.                                            */
        break;
    }
}

#undef CALL

void astWalkProgram(Node* program, const AstVisitor* v, void* userData) {
    AstWalkCtx ctx = {
        .visitor   = v,
        .userData  = userData,
        .enclosing = NULL,
    };
    walk(&ctx, program);
}
