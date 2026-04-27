package ReferentialDemo {
    private import ScalarValues::*;

    item def Signal { }

    // Top-level usage at package scope: must be referential (rule 1c).
    part topPart : Signal;

    part def Sensor {
        // Directed ports: must be referential (rule 1a).
        in  port command  : Signal;
        out port reading  : Signal;
        // Plain port inside a part: must be referential (rule 3).
        port plain        : Signal;
    }

    // Subport pattern: a port inside a port can stay composite.
    // The inference does NOT force `inner` to be referential because
    // its owningType is itself a PortDefinition (rule 3 carve-out).
    port def OuterPort {
        port inner : Signal;
    }

    // Connection definition with end features: ends are referential
    // by rule 1b (isEnd).
    connection def Wire {
        end source : Signal;
        end target : Signal;
    }

    // Flow definition with the maximum two ends.
    flow def DataFlow {
        end producer : Signal;
        end consumer : Signal;
    }
}
