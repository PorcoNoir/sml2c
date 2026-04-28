// v0.9 — long-tail parser features.
package LongTailDemo {
    private import ScalarValues::*;

    // Anonymous enum values (bare `enum =` introducer).
    enum def DiameterChoices :> Real {
        enum = 60;
        enum = 80;
        enum = 100;
    }

    // `default` keyword as alternate to `=`.
    part def Vehicle {
        attribute mass : Real default 0;
        attribute fuelMass : Real default 60;
    }

    // Bare `constraint { expr }` without assertion prefix.
    part def Engine {
        attribute torque : Real;
        constraint { torque > 0 }
    }

    // Tuple expression `(a, b, c)`.
    part def Position {
        attribute x : Real;
        attribute y : Real;
        attribute z : Real;
        attribute pos = (x, y, z);
    }

    // `xor` operator inside a constraint.
    part def Crew {
        attribute driver : Boolean;
        attribute passenger : Boolean;
        assert constraint {driver xor passenger}
    }

    // `meta` operator (parses; semantics deferred).
    metadata def Tag;
    attribute def baseRef;
    metadata def Marker {
        attribute baseType = baseRef meta Tag;
    }

    // Anonymous flow with implicit `from` (first arg is source).
    item def Torque;
    part def Drivetrain {
        out port input : Torque;
        in port output : Torque;
        flow input to output;
    }
}
