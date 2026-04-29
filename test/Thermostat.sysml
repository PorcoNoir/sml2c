package Thermostat {
    private import ScalarValues::*;

    /* Pure function: compute output power from temperature error. */
    calc def ResponsePower {
        in error : Real;
        in gain  : Real;
        return out : Real = error * gain;
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