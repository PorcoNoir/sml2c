package BadRedef {
    private import ScalarValues::*;

    part def Vehicle {
        attribute speed : Real;
    }

    // Error: 'color' isn't in any supertype of Bicycle.
    part def Bicycle :> Vehicle {
        attribute color :>> color : String;
    }

    // Error: widening — String can't redefine Vehicle.speed which is Real.
    part def Car :> Vehicle {
        attribute speed :>> speed : String;
    }

    // Error: Standalone has no supertypes, so the redef target can't exist.
    part def Standalone {
        attribute pace :>> pace : Real;
    }

    // Error: multi-segment redef with type widening — String doesn't
    // specialize Real. Now caught because qualifier validation enables
    // the type-compat check for n<=2 paths.
    part def Truck :> Vehicle {
        attribute speed :>> Vehicle::speed : String;
    }
}
