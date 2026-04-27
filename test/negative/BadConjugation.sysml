package BadConjugation {
    private import ScalarValues::*;

    item def Signal { }

    port def Sensor {
        in port command  : Signal;
        out port reading : Signal;
    }

    part def Producer {
        port iface : Sensor;
    }

    part def NotConsumer {
        port iface : Sensor;             // NOT conjugated — should fail
    }

    part system {
        part p : Producer;
        part c : NotConsumer;

        // Error: both `reading` ports are `out`; flow target must be `in`.
        flow from p.iface.reading to c.iface.reading;

        // Error: both `command` ports are `in`; flow source must be `out`.
        flow from c.iface.command to p.iface.command;
    }
}
