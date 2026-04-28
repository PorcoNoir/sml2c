// v0.8 — Tier 3 keywords: metadata, verification, objective, satisfy,
// filter; xor operator; nonunique/ordered modifiers; <short> alt-name.
package Tier3Demo {
    private import ScalarValues::*;

    // metadata def with body and bare-form.
    metadata def Safety {
        attribute isMandatory : Boolean;
    }
    metadata def Security;

    // verification def, bare-form.
    verification def MassTest;
    verification def AccelerationTest;

    // multiplicity modifiers nonunique and ordered.
    part def Vehicle {
        attribute partMasses [*] nonunique;
        part wheels [4] ordered;
        part anywheels [*] ordered nonunique;
    }

    // <short> alt-name annotation on a definition.
    metadata def <short> longerMetadataName;

    // xor in a constraint body.
    part def Crew {
        attribute driver : Boolean;
        attribute passenger : Boolean;
        assert constraint { driver xor passenger }
    }

    // satisfy statement.  Body is currently parse-and-skipped (the AST
    // has no slot for satisfy reasoning yet — v0.10 typed-AST work).
    requirement def MassReq;
    part def Subject;
    part def Target;
    part def System {
        part subj : Subject;
        part target : Target;
        satisfy MassReq by subj;
        satisfy MassReq by target;
    }

    // variant prefix on part.
    variation part def TransmissionChoices {
        variant part automatic : Subject;
        variant part manual : Subject;
    }
}
