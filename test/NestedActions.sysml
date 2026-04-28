package NestedActionDemo {
    private import ScalarValues::*;

    action def Outer {
        in input : Real;
        out output : Real;

        // Nested action declarations as the chain's targets.
        first start;
        then action stage1 {
            in x : Real;
            out y : Real;
        }
        then action stage2 {
            in u : Real;
            out v : Real;
        }
        then done;
    }
}
