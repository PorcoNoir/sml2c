package P {
    part def Q {
        attribute x : Real;
        // dot followed by nothing valid
        attribute bad : Real = x.;
    }
}
