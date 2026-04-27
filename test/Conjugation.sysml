package ConjugationDemo {
    private import ScalarValues::*;

    item def Signal { }

    // A port pair: command flows in (commands arrive), reading flows out
    // (sensor data leaves).  Producer side has Sensor, consumer side has
    // ~Sensor — the conjugation gives the matching half automatically.
    port def Sensor {
        in port command  : Signal;
        out port reading : Signal;
    }

    part def Producer {
        port iface : Sensor;
    }

    part def Consumer {
        port iface : ~Sensor;            // conjugated — directions flip
    }

    part system {
        part p : Producer;
        part c : Consumer;

        // Producer's reading is `out`, Consumer's reading (via ~Sensor)
        // is effectively `in` — flow direction-correct without any
        // manual fiddling on the consumer side.
        flow from p.iface.reading to c.iface.reading;

        // Symmetric: Producer's command is `in`, Consumer's command
        // (via ~Sensor) is effectively `out`.
        flow from c.iface.command to p.iface.command;
    }
}
