package BadMult {
    private import ScalarValues::*;
    part def X {
        attribute a : Real [0..*];    // empty brackets
        attribute b : Real [3..*];    // missing upper bound
        attribute c : Real [3..4];    // missing ']'
        attribute d : Real [1..2];    // duplicate multiplicity
    }
}
