package BadQualifier {
    private import ScalarValues::*;

    part def A { attribute x : Integer; }
    part def B { attribute x : Real; }

    // C specializes A, not B. :>> B::x is the wrong qualifier path
    // even though B.x exists as a valid member of B itself.
    part def C :> A {
        attribute x :>> B::x : Integer;
    }
}
