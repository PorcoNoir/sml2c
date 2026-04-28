package BadModifiers {
    private import ScalarValues::*;

    part def Vehicle { }

    // Error: 'derived' is not valid on a definition.
    derived part def Bad1 { }

    // Error: 'constant' is not valid on a definition.
    constant part def Bad2 { }

    // Error: 'ref' is not valid on a definition.
    ref part def Bad3 { }
}
