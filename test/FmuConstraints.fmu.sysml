/*
 * FmuConstraints.fmu.sysml — v0.26 fixture for `--emit-fmu-c`.
 *
 * Exercises the full v0.26 lowering surface:
 *
 *   - calc def Power → free C function in src/model.c
 *   - attribute power : Real = Power(force, velocity);
 *           → calc-driven init: model_setDefaults computes
 *             m->power = Power(m->force, m->velocity)
 *           → ExitInit reads the computed value back via fmi3GetFloat64
 *   - assert constraint sane_force { force > 0.0 and force < 10000.0 }
 *           → branch in model_check called from
 *             fmi3ExitInitializationMode; failure routes through the
 *             logger callback and returns fmi3Error
 *
 * The generated test_fmu.c smoke test verifies both:
 *   1. defaults pass (Init succeeds, computed power = 5000.0)
 *   2. nothing about port lowering or items (deferred to v0.27)
 *
 * The constraint-violation path is exercised by an extended test
 * driver that's not auto-generated for v0.26 — see future versions.
 * For now, a manual Reset + fmi3SetFloat64(force, -1.0) + ExitInit
 * sequence will return fmi3Error and produce a logger message
 * "Engine.sane_force failed: force > 0.0 and force < 10000.0".
 */
package FmuConstraints {
    private import ScalarValues::*;

    /* Pure function — returns force times velocity.  Both inputs and
     * the return are Real, so the FMU emits this as a
     * `fmi3Float64 Power(fmi3Float64 f, fmi3Float64 v)`.            */
    calc def Power {
        in f : Real;
        in v : Real;
        return result : Real = f * v;
    }

    part def Engine {
        attribute force    : Real = 100.0;
        attribute velocity : Real = 50.0;

        /* Calc-driven default — exercises the expression emitter's
         * NODE_CALL handling and member-reference rewriting. */
        attribute power    : Real = Power(force, velocity);

        /* Inline assert constraint — exercises model_check emission
         * and the logger-callback failure routing. */
        assert constraint sane_force {
            force > 0.0 and force < 10000.0
        }
    }
}
