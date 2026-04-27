package RedefDemo {
    private import ScalarValues::*;

    part def Vehicle {
        attribute speed : Real;
        attribute name : String;
    }

    // Same-type redefinition; valid because Real specializes Real (reflexive).
    part def Car :> Vehicle {
        attribute speed :>> speed : Real;
    }

    // Type narrowing: Truck declares speed as Integer, which specializes Real.
    part def Truck :> Vehicle {
        attribute speed :>> speed : Integer;
    }

    // Untyped redefinition — inheriting type from supertype is fine.
    part def Sedan :> Vehicle {
        attribute speed :>> speed;
    }

    // Multi-segment redef target: silently accepted (deferred to a later
    // pass; member-of-type resolution isn't implemented yet).
    part def Bus :> Vehicle {
        attribute speed :>> Vehicle::speed : Integer;
    }
}
