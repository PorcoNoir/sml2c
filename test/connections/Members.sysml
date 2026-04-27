package MembersDemo {
    private import ScalarValues::*;

    part def Engine {
        attribute torque  : Real;
        attribute redline : Integer;
    }

    // Multi-segment type reference: ScalarValues::Real resolves to Real,
    // not to ScalarValues. The typechecker can read the right type.
    part def Calibration {
        attribute fullScale : ScalarValues::Real = 1000.0;
    }

    // Feature chain in an expression. `e.torque` walks: e is the part
    // usage of type Engine; torque is then looked up as a member of
    // Engine. The whole expression types as Real.
    part def Booster {
        part e : Engine;
        attribute peak : Real = e.torque;
    }

    // Multi-segment redef target. The redef checker uses last-segment
    // lookup against TurboEngine's supertypes (Engine), and finds
    // Engine.torque. Type compat is deferred until we validate the
    // qualifier prefix.
    part def TurboEngine :> Engine {
        attribute torque :>> Engine::torque;
    }
}
