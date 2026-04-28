package BareDefs {
    private import ScalarValues::*;

    // Bare-form definitions with no body — common in real SysML.
    part def Cylinder;
    port def StatusPort;
    item def Marker;
    action def DoWork;
    action def DoMore;
    state def WorkStates;
    constraint def AlwaysTrue { true }

    // Bare definitions can still carry specializations.
    part def Engine4Cyl :> Cylinder;
    part def Engine6Cyl :> Cylinder;

    // Bare-form within a part body — just a marker.
    part def Vehicle {
        attribute mass : Real;
        // Bare port usage (no type).
        port flyWheelPort;
    }
}
