/*
 * Inferred-type cache for expression nodes (v0.14).
 *
 * The typechecker populates `inferredType` on literals, qualified
 * names used in value position, and binary/unary operations.
 * `--emit-json` exposes this as a "type" field so codegen and
 * downstream tooling can read it without re-running the typechecker.
 */
package InferredTypes {
    private import ScalarValues::*;

    attribute zero      : Integer = 0;            // literal -> Integer
    attribute pi        : Real    = 3.14;         // literal -> Real
    attribute greeting  : String  = "hi";         // literal -> String
    attribute always    : Boolean = true;         // literal -> Boolean

    attribute area      : Real    = pi * 2.0 * 2.0;       // binary -> Real
    attribute negated   : Real    = -pi;                  // unary  -> Real
    attribute sumI      : Integer = zero + 1;             // binary -> Integer
    attribute compare   : Boolean = pi > 0.0;             // binary -> Boolean
}
