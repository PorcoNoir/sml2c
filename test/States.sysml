package StateDemo {
    private import ScalarValues::*;

    action def Initialize;
    action def MonitorTemp;
    action def ProvidePower;
    action def ApplyBrake;

    // Bare state def — sentinel used to mark a feature.
    state def VehicleStates;

    // State def with members and transitions.
    state def EngineStates {
        action initial : Initialize;

        state off;
        state starting;
        state on {
            entry MonitorTemp;
            do ProvidePower;
            exit ApplyBrake;
        }

        // Initial transition (no `first`, just identifier-as-source).
        transition initial then off;

        // Named full-form transition with accept.
        transition off_to_starting
            first off
            accept Initialize
            then starting;

        // No accept, just first/then.
        transition starting_to_on
            first starting
            then on;
    }

    // Simple state def with no body but specialization.
    state def SubStates :> VehicleStates;

    // Part def with exhibit state usage and bare state usage.
    part def Engine {
        attribute torque : Real;
        exhibit state engineStates : EngineStates;
    }

    // Part def with bare exhibit (no type).
    part def Vehicle {
        exhibit state vehicleStates;
    }
}
