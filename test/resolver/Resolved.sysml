package Resolved {
    private import ScalarValues::*;

    part def Engine {
        attribute torque : Real;
        // expression references sibling attribute — should resolve
        attribute boosted : Real = torque * 2;
    }

    // forward reference: TurboEngine names Engine as supertype, and
    // appears later than Engine in source (good), but order could be
    // flipped and it should still work because the resolver pre-pass
    // declares all members before recursing.
    part def TurboEngine :> Engine {
        // redefines references parent member — resolved against outer scope
        attribute torque :>> torque;
    }

    // forward reference IN THE OPPOSITE DIRECTION:
    // Vehicle is declared later but Sensor refers to it.
    part def Sensor {
        out port reading : Vehicle;
    }

    part def Vehicle {
        attribute mass : Real = 1500;
    }
}
