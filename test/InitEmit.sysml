/*
 * InitEmit — v0.23 regression test for init-function lowering.
 *
 * Three things to exercise:
 *
 *   1. T_init for a part def with primitive defaults — the simple
 *      case.  Each defaulted attribute → one assignment.
 *
 *   2. T_init with sibling-referencing defaults — the bare-name
 *      rewriter should produce `self->X` for each sibling reference.
 *      The topological sort should order the assignments so that a
 *      derived attribute is assigned after the things it depends on.
 *
 *   3. Top-level non-const initializer — the L2 path.  Becomes a
 *      non-const global plus an assignment in `__sml2c_init`.
 */
package InitEmit {
    private import ScalarValues::*;

    /* For #3 below — a plain calc def we'll call from a top-level
     * initializer to force the L2 path. */
    calc def Square {
        in x : Real;
        return result : Real = x * x;
    }

    /* (1) + (2): primitive defaults with sibling references. */
    part def Engine {
        attribute mass    : Real = 200.0;        /* L4 — direct literal       */
        attribute torque  : Real = 350.0;        /* L4 — direct literal       */
        attribute redline : Real = 7000.0;       /* L4 — direct literal       */

        /* Derived attributes — should topo-sort AFTER mass/torque/redline. */
        attribute massPlusTorque : Real = mass + torque;
        attribute headroom       : Real = redline - massPlusTorque;
    }

    /* (3): top-level non-const.  RHS calls Square, so this can't be
     *     a static const — gets hoisted to L2. */
    attribute referenceSquare : Real = Square(7.0);
}
