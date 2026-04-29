#ifndef SYSMLC_AST_WALK_H
#define SYSMLC_AST_WALK_H

/*  ast_walk — centralized recursive descent over the AST.
 *
 *  Before v0.13, every back-end pass (typechecker, redefchecker,
 *  connectchecker, referentialchecker) carried its own per-NodeKind
 *  switch with manual recursion through scope/usage members.  Every
 *  new container kind required mechanical edits in four places.  The
 *  passes also differed slightly on whether they tracked the nearest
 *  ancestor in a parameter — three of four did, with subtle bugs in
 *  the fourth (no ancestor for top-level definitions).
 *
 *  This module folds that boilerplate into one place:
 *
 *    * `astWalkProgram` walks the entire program once, invoking
 *      pre/post hooks at each container node and a leaf hook at
 *      attribute / succession.
 *    * `enclosing` is set automatically to the nearest NODE_DEFINITION
 *      or NODE_USAGE ancestor.  Top-level callbacks see `enclosing ==
 *      NULL`.
 *    * `userData` is opaque to the framework and threaded through to
 *      every callback.  Each pass uses it for its own error counter
 *      and any pass-specific scope state.
 *
 *  Hooks default to NULL.  A pass only sets the ones it needs; the
 *  framework still descends through container kinds even when no
 *  callback is registered.  This means the same recursive shape is
 *  shared by every pass and adding a new container NodeKind is a
 *  one-line edit in `ast_walk.c` rather than five edits across passes.
 */

#include "ast.h"

#include <stdbool.h>

typedef struct AstWalkCtx AstWalkCtx;

/*  Hook signature.  `enclosing` is read from `ctx->enclosing` rather
 *  than passed as a separate argument so that callers can also reach
 *  the visitor's userData and any future per-walk state from a single
 *  pointer.                                                          */
typedef void (*AstNodeFn)(AstWalkCtx* ctx, Node* n);

/*  Visitor table.  All fields default to NULL.  The framework calls
 *  Enter hooks before descending into a container's children and Leave
 *  hooks afterwards, which lets a pass do either pre-order or post-
 *  order work without writing its own switch.  Leaf hooks
 *  (`onAttribute`, `onSuccession`) fire once per matching node with no
 *  pre/post split — neither leaf currently has children that matter
 *  to any pass.                                                       */
typedef struct AstVisitor {
    AstNodeFn onProgramEnter;
    AstNodeFn onProgramLeave;
    AstNodeFn onPackageEnter;
    AstNodeFn onPackageLeave;
    AstNodeFn onDefinitionEnter;
    AstNodeFn onDefinitionLeave;
    AstNodeFn onUsageEnter;
    AstNodeFn onUsageLeave;
    AstNodeFn onAttribute;
    AstNodeFn onSuccession;
} AstVisitor;

struct AstWalkCtx {
    const AstVisitor* visitor;     /* set by astWalkProgram        */
    void*             userData;    /* opaque per-pass state         */
    const Node*       enclosing;   /* nearest DEF or USAGE ancestor */
};

/*  Entry point.  Walks `program` (typically a NODE_PROGRAM root) and
 *  invokes hooks in `v` with `userData` accessible through the ctx.  */
void astWalkProgram(Node* program, const AstVisitor* v, void* userData);

#endif /* SYSMLC_AST_WALK_H */
