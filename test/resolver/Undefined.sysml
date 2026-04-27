// No `import X::*;` here — undefined names cannot hide behind a wildcard.
package Strict {
    part def Engine {
        // Real isn't declared, so this should error.
        attribute torque : Real;

        // Whatever this is, it's also undefined.
        attribute alias :> NotARealName;

        // Reference to undefined sibling in expression position.
        attribute computed : Integer = nonexistent + 1;
    }
}
