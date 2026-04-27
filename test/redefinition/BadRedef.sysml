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
}
