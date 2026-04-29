/*
 * Thermostat — running example from design/c-codegen.md §2.
 *
 * Exercises the full v0.22 → v0.27 lowering catalog when complete.
 * Status as of v0.23.1:
 *
 *   ResponsePower (calc def)        — lowers (v0.22)
 *   Controller (part def)           — skips: contains inline constraints
 *                                     (v0.24) and an `exhibit state` (v0.25)
 *   Mode (state def)                — skips (v0.25)
 *   TooCold/TooHot/InRange (action) — skips (v0.25 — used as state events)
 *
 * The file is in the corpus to lock in parser correctness across
 * future versions and to give us something concrete to point at when
 * v0.24 / v0.25 land.
 */
package Thermostat {
    private import ScalarValues::*;

    /* Pure function: compute output power from temperature error. */
    calc def ResponsePower {
        in error : Real;
        in gain  : Real;
        return power : Real = error * gain;
    }

    /* Three-mode state machine: idle, heating, cooling. */
    state def Mode {
        state idle;
        state heating;
        state cooling;

        transition idle_to_heating  first idle    accept TooCold then heating;
        transition idle_to_cooling  first idle    accept TooHot  then cooling;
        transition heating_to_idle  first heating accept InRange then idle;
        transition cooling_to_idle  first cooling accept InRange then idle;
    }

    action def TooCold;
    action def TooHot;
    action def InRange;

    /* The thermostat's parts and behavior. */
    part def Controller {
        attribute setpoint : Real = 21.0;       // °C target
        attribute measured : Real = 20.0;       // current reading
        attribute deadband : Real = 0.5;        // hysteresis margin
        attribute gain     : Real = 100.0;
        attribute power    : Real = 0.0;        // current output

        assert constraint sane_setpoint {
            setpoint > 10.0 and setpoint < 35.0
        }
        assert constraint sane_deadband {
            deadband > 0.0 and deadband < 5.0
        }

        exhibit state mode : Mode;
    }
}
