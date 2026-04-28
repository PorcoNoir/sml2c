package BadMemberRef {
    private import ScalarValues::*;

    // Error: ScalarValues has no member 'NotReal'.
    part def X {
        attribute bad : ScalarValues::NotReal;
    }

    part def Engine {
        attribute torque : Real;
    }

    // Error: e.notAField — Engine has no member 'notAField'.
    part def Booster {
        part e : Engine;
        attribute peak : Real = e.notAField;
    }
}
