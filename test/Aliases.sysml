package AliasDemo {
    private import ScalarValues::*;

    part def Engine { attribute torque : Real; }

    // Simple alias: a local short name for a deeper qualified path.
    alias E for AliasDemo::Engine;

    // Alias to an attribute through a definition.
    alias T for AliasDemo::Engine::torque;

    // Aliases work inside packages too.
    package Inner {
        part def Wheel { }
        alias W for AliasDemo::Inner::Wheel;
    }

    // Now USE the aliases — these references resolve through the
    // alias to the underlying definition/attribute.
    part def Vehicle {
        // Use of alias E as a type — resolves through to Engine.
        part engine : E;

        // Use of alias E::torque — walks through E, finds torque on Engine.
        attribute peakTorque : Real = engine.torque;

        // Use of alias W from a sibling package.
        part wheel : Inner::W;
    }
}

