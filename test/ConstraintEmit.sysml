/*
 * ConstraintEmit — v0.24 regression test for inline `assert
 * constraint` lowering to T_check predicates.
 *
 * Two part defs:
 *
 *   1. Engine — three constraints (two named, one anonymous).
 *      All pass at default values; the driver mutates fields to
 *      trigger each one and verifies the failure message.
 *
 *   2. Battery — single constraint, lets us check that T_check
 *      returns true cleanly when nothing's wrong (no stderr noise).
 */
package ConstraintEmit {
    private import ScalarValues::*;

    part def Engine {
        attribute mass    : Real = 200.0;
        attribute torque  : Real = 350.0;
        attribute redline : Real = 7000.0;

        /* Named — failure message says "Engine.sane_mass failed: ...". */
        assert constraint sane_mass {
            mass > 0.0 and mass < 10000.0
        }

        /* Named — same form, different field. */
        assert constraint sane_torque {
            torque > 0.0
        }

        /* Anonymous — failure message says "Engine constraint failed: ...".
         * Source text is preserved through emitExprAsCString. */
        assert constraint {
            redline < 12000.0
        }
    }

    part def Battery {
        attribute voltage : Real = 12.6;
        attribute soc     : Real = 1.0;       /* state of charge 0–1 */

        assert constraint healthy_voltage { voltage > 10.0 }
    }
}
