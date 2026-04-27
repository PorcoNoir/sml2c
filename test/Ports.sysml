package PortsDemo {
    private import ScalarValues::*;

    /** A signal item that flows through ports. */
    item def Signal {
        attribute strength : Real;
    }

    /** Generic interface representing a transmission relationship.
     *  Bodies are empty for now — endpoint syntax (connect/from/to)
     *  arrives in a follow-up turn (option B).                     */
    interface def TxLink { }

    /** A connection definition specializing an interface def. */
    connection def Wire :> TxLink { }

    /** A flow definition. */
    flow def DataFlow { }

    /** Sensor with several ports of different directions. */
    part def Sensor {
        out   port signalOut : Signal;          // outbound
        in    port reset     : Signal;          // inbound
        inout port duplex    : Signal [2];      // bidirectional, multiplicity 2
              port readout   : Signal;          // no direction
    }

    /** Logger with one inbound port. */
    part def Logger {
        in port signalIn : Signal;
    }

    part system : Sensor {
        // usages of all five new families
        item       data    : Signal;
        port       p1      : Signal;
        interface  link    : TxLink;
        connection c1      : Wire;
        flow       f1      : DataFlow;
    }

    // visibility + direction stacked, in source order: visibility direction port
    part def Probe {
        private in  port secret    : Signal;
        public  out port broadcast : Signal [0..*];
    }
}
