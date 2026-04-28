package BadAlias {
    private import ScalarValues::*;

    part def Engine { }

    // Error: target 'NotEngine' does not exist.
    alias E for NotEngine;

    // Error: 'Engine' exists but it has no member 'fuelType'.
    alias F for Engine::fuelType;
}
