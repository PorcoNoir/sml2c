package BadConn {
    private import ScalarValues::*;

    item def Signal { }

    part def Sensor {
        attribute name : String;       // not a port
        out port out1 : Signal;        // an actual port
    }

    part s : Sensor;
    part t : Sensor;

    // Error: 's' and 't' are part usages, not ports.
    connect s to t;

    // Error: s.name is an attribute, not a port.
    connect s.name to t.out1;

    // Mixed: one ok, one not. Only the bad end errors.
    connect s.out1 to t.name;

    // Error: flow ends must also be port-typed.
    flow from s to t;
}
