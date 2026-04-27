package ConnectionsDemo {
    private import ScalarValues::*;

    item def Signal { attribute strength : Real; }

    /* Multiple-inheritance via comma list. */
    interface def Bus :> Signal, Real { }

    /* Connection def with endpoint declarations inside its body. */
    connection def Wire :> Bus {
        end source : Signal;
        end target : Signal [1..*];
    }

    /* Flow def with end declarations. */
    flow def DataFlow {
        end producer : Signal;
        end consumer : Signal;
    }

    part def Sensor {
        out port reading  : Signal;
        in  port command  : Signal;
        inout port bidir  : Signal;
    }

    part def Logger {
        in port input : Signal;
        inout port bidir : Signal;
    }

    /* System composition exercising the new endpoint clauses. */
    part system : Sensor {
        part s : Sensor;
        part l : Logger;

        // (B) standalone bare connect
        connect s.reading to l.input;

        // (C) named connection usage with explicit type and ends
        connection c1 : Wire connect s.reading to l.input;

        // anonymous flow with explicit endpoints
        flow from s.reading to l.input;

        // named flow with type and endpoints
        flow signalFlow : DataFlow from s.reading to l.input;

        // inout ports work as both flow source and target
        flow from s.bidir to l.bidir;
        flow from s.reading to l.bidir;

        // attribute carrying multi-valued type and specializes lists
        attribute everything : Real, Integer :> Signal::strength, Bus;
    }
}
