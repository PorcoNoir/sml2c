package CommentDemo {
    private import ScalarValues::*;

    part def Vehicle {
        attribute speed : Real;
    }

    // Anonymous comment with no targets — namespace-level note.
    comment /* This package models a small vehicle ontology. */

    // Named comment, untargeted.
    comment N1 /* Versioning note: v0.1, drafted by jb. */

    // Comment with an `about` target referencing a definition.
    comment about Vehicle /* Vehicle is the root abstraction. */

    // Comment with multiple about-targets.
    comment Multi about Vehicle, Vehicle::speed /* Both are central. */
}
