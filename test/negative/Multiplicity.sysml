package MultiplicityDemo {
    private import ScalarValues::*;

    /* Supertype that owns the features we'll later redefine.
     * The redefinition-featuring-types rule requires the redefined
     * feature to come from a *different* (super-)type.            */
    part def BaseThing {
        attribute id : Integer [1];
    }

    part def Vehicle :> BaseThing {
        // single bound — exactly N
        attribute wheels      : Integer [4];

        // open ranges
        attribute owners      : String  [0..*];
        attribute trailers    : String  [*];

        // closed range
        attribute children    : String  [1..5];

        // open lower
        attribute backupKeys  : String  [3..*];

        // multiplicity before type works too — can appear in any order
        // alongside the other relationship clauses
        attribute aliases [0..*] : String;

        // multiplicity combined with :> and :>>; the redefine target
        // is inherited from BaseThing, so featuring types differ.
        attribute load : Real [1] :> ScalarValues::Real :>> BaseThing::id;
    }

    // multiplicities on part usages
    part fleet     : Vehicle [10];
    part backups   : Vehicle [0..*];
    part oneOrMore : Vehicle [1..*] {
        // nested usage with a multiplicity inside the body
        part shadows : Vehicle [3];
    }
}
