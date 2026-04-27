package ModifiersDemo {
    private import ScalarValues::*;

    // `abstract` on a definition.
    abstract part def Vehicle {
        attribute speed : Real;
    }

    // `abstract` on a port definition.
    abstract port def Sensor { }

    // `abstract` on an interface definition.
    abstract interface def Bus { }

    // Specialization: concrete definition narrowing an abstract one.
    part def Car :> Vehicle {
        // `derived` on an attribute with a default-value expression —
        // the value is computed from other features.
        derived attribute area : Real = 1.0 * 2.0;

        // `constant` — value can't change once set.
        constant attribute pi : Real = 3.14159;

        // `ref` — non-composite usage.
        ref part driver : Vehicle;

        // All four modifiers in canonical spec order:
        // direction → derived → abstract → constant → ref → kind.
        // (Direction here happens to apply to a port usage.)
        in derived abstract constant ref port info : Sensor;
    }

    // `ref` on a part usage at package level.
    ref part fleetVehicle : Vehicle;

    // Redefining a `constant` attribute is fine when the redefiner
    // is also `constant` (preserves immutability).
    part def Truck :> Car {
        constant attribute pi :>> pi : Real = 3.14;
    }
}
