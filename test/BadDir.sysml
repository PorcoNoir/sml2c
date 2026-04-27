package BadDir {
    part def X {
        // direction is only allowed on usages of port-kind
        in part p : X;             // error: direction not valid on a part usage
        out attribute a : Real;    // error: direction not valid on attribute
    }

    in port def BadPortDef { }     // error: direction not valid on a definition
}
