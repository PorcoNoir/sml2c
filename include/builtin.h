/* sysmlc — builtin.h
 *
 * The built-in synthetic standard library.  Constructed in C as a
 * pre-built AST so that the resolver and typechecker have something
 * concrete to point at when SysML source says `import ScalarValues::*`.
 *
 * v0.x scope: just the ScalarValues package, holding Number, Real,
 * Integer, Boolean, and String.  The package is built lazily on
 * first call and lives for the duration of the program.
 *
 * Subtype shape:
 *
 *     Number        (root numeric)
 *       └─ Real
 *            └─ Integer
 *     Boolean       (standalone)
 *     String        (standalone)
 *
 * Note: Integer specializing Real is a pragmatic simplification; the
 * SysML v2 spec actually has Integer and Real as siblings under
 * Number.  Our shape lets `attribute x : Real = 5` succeed without an
 * explicit conversion, which is what most users expect.
 */
#ifndef SYSMLC_BUILTIN_H
#define SYSMLC_BUILTIN_H

#include "ast.h"

/* The synthetic ScalarValues package node.  Suitable for declaring
 * at the resolver's root scope so that `import ScalarValues::*`
 * finds real symbols. */
const Node* builtinScalarValuesPackage(void);

/* Singleton accessors for the well-known datatypes.  Used by the
 * typechecker to identify literal types and check compatibility. */
const Node* builtinNumber(void);
const Node* builtinReal(void);
const Node* builtinInteger(void);
const Node* builtinBoolean(void);
const Node* builtinString(void);

/* Built-in pseudo-actions referenced by `first start;` and `then done;`
 * in action bodies.  Modeled as DEF_ACTION usages with the appropriate
 * name; pre-resolved at parse time so they don't go through the
 * resolver's scope lookups.                                        */
const Node* builtinStart(void);
const Node* builtinDone(void);

#endif /* SYSMLC_BUILTIN_H */
