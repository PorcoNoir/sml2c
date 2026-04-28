package PerformDemo {
    private import ScalarValues::*;

    action def ProvidePower {
        in throttle : Real;
    }

    action def ApplyBrake;
    action def SenseTemp;

    part def Vehicle {
        attribute mass : Real;

        // `perform action <name>` — short form.
        perform action providePower;

        // `perform <qname>` — by reference.
        perform ApplyBrake;

        // Named `perform` with type — declares a usage.
        perform action senseTemperature : SenseTemp;
    }

    // `perform` with redefines clause for inheritance.
    part def SportsCar :> Vehicle {
        perform action providePower redefines providePower;
    }
}
