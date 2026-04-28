package ConstraintDemo {
    private import ScalarValues::*;

    // Single-line constraint def with parameters.
    constraint def TorqueLimit {
        in torque : Real;
        in maxT : Real;
        torque <= maxT
    }

    // Multi-line body with `and` connective, mirroring the SysML spec
    // example (force * deltaT = mass * (finalSpeed - initialSpeed)).
    constraint def AveragedDynamics {
        in mass : Real;
        in initialSpeed : Real;
        in finalSpeed : Real;
        in deltaT : Real;
        in force : Real;

        force * deltaT == mass * (finalSpeed - initialSpeed) and
        mass > 0
    }

    // No-parameter constraint — just a body expression.
    constraint def AlwaysTrue {
        true
    }

    // Constraint def using `or` connective and parens.
    constraint def Direction {
        in v : Real;
        v < 0 or v > 0
    }

    // Asserted by-reference; both forms (named-and-typed, anonymous inline).
    part def Engine {
        attribute torque : Real;
        attribute maxT : Real;
        attribute speed : Real;
        assert constraint c1 : TorqueLimit;
        assert constraint { torque > 0 and speed >= 0 }
        assert constraint named : TorqueLimit;
    }
}
