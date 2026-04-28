// v0.10 — second long-tail pass.  Each construct here was a parser
// hole on the PTC reference file before this version.
package LongTail2 {
    private import ScalarValues::*;

    // (1) Bare-form features `name :> X = expr;` with no kind keyword
    //     and no modifier/direction prefix.  Common in derived-quantity
    //     packages.
    attribute def scalarQuantities;
    attribute def DerivedUnit;
    distancePerVolume :> scalarQuantities;
    kpl : DerivedUnit;

    // (2) `parallel` on state usages (`exhibit state X parallel { ... }`)
    //     — orthogonal-region keyword silently consumed.
    state def OperatingStates;
    part def VehicleStates {
        exhibit state engineStates parallel {
            state off;
            state on;
        }
    }

    // (3) Lifecycle action with body and no trailing `;`:
    //     `do senseTemperature { out temp; }` — the `}` terminates.
    action def SenseTemp {
        out temp;
    }
    action senseTemperature : SenseTemp;
    state def Mode {
        do senseTemperature {
            out temp;
        }
        state running;
    }

    // (4) Attribute with inline body.
    item def Box;
    attribute spatialCF : Box [1] {
        attribute internal;
    }

    // (5) Standalone metadata block as the only member of a usage
    //     body: `part bumper { @Safety { isMandatory = true; } }`.
    metadata def Safety {
        attribute isMandatory : Boolean;
    }
    part def Bumper;
    part def Vehicle {
        part bumper : Bumper {
            @Safety { isMandatory = true; }
        }
    }

    // (6) Anonymous-by-= parameter: `in item = expr;`.
    action def Provide {
        in item = something;
    }
    item def something;

    // (7) Perform with body containing nested actions.
    action def StartVehicle;
    part def StartContext {
        perform action startVehicle {
            action turnOn;
        }
    }
}
