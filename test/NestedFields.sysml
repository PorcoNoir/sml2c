// NestedFields — exercises v0.16 emit-c support for part-typed
// struct fields and dependency-ordered emission.  Engine and Wheel
// are referenced by Car, but Car is declared first in the source;
// the codegen registry pass should sort them so Engine and Wheel
// emit before Car in the C output.
package NestedFields {
    private import ScalarValues::*;

    part def Car {
        attribute mass : Real;
        part engine : Engine;
        part wheel : Wheel;
    }

    part def Engine {
        attribute power : Real;
        attribute cyl   : Integer;
    }

    part def Wheel {
        attribute size : Real;
    }
}
