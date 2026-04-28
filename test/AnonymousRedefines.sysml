package AnonymousRedefines {
    private import ScalarValues::*;

    part def Vehicle {
        attribute mass : Real;
        attribute fuelMass : Real;
    }

    // Subclass overrides inherited features without re-declaring names.
    part def MyVehicle :> Vehicle {
        // `attribute redefines mass = …;` — anonymous attribute that redefines.
        attribute redefines mass = 75;

        // `attribute :>> fuelMass;` — anonymous, with the :>> shortcut.
        attribute :>> fuelMass;

        // Anonymous with multiplicity.
        attribute :>> fuelMass [1];
    }
}
