// MultiplicityArrays — exercises v0.17 emit-c support for fixed-size
// arrays.  Both attribute fields (Real [3]) and part-typed fields
// (Wheel [4]) lower to C arrays.  Range and wildcard multiplicity
// reject the enclosing definition wholesale (and so don't appear
// here); the negative path is verified by hand against /tmp.
package MultiplicityArrays {
    private import ScalarValues::*;

    part def Wheel { attribute size : Real; }

    part def Car {
        attribute mass   : Real;
        attribute scores : Real [3];
        part wheels      : Wheel [4];
    }
}
