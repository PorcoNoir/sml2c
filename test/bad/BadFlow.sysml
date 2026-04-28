package BadFlow {
    private import ScalarValues::*;

    item def Signal { }

    part def Sensor {
        in port input1 : Signal;
        out port output1 : Signal;
        port undirected : Signal;     // no direction
    }

    part s : Sensor;
    part t : Sensor;

    // Error: flow source 'input1' is 'in' — must be 'out' or 'inout'.
    flow from s.input1 to t.input1;

    // Error: flow target 'output1' is 'out' — must be 'in' or 'inout'.
    flow from s.output1 to t.output1;

    // Error: source has no direction — must be 'out' or 'inout'.
    flow from s.undirected to t.input1;
}
