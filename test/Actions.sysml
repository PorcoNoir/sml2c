package ActionDemo {
    private import ScalarValues::*;

    // Plain action def with parameters.
    action def LoadCargo {
        in priority : Integer;
        out loaded : Boolean;
    }

    action def Travel {
        in distance : Real;
        out arrived : Boolean;
    }

    // Action def with a succession chain referencing declared subactions.
    action def DeliverGifts {
        in santa : String;
        out delivered : Boolean;

        action loadCargo : LoadCargo;
        action travel : Travel;
        action unload;

        first start;
        then loadCargo;
        then travel;
        then unload;
        then done;
    }

    // Action def using inline `then action ...` declarations.
    action def InlineFlow {
        first start;
        then action prep;
        then action work : Travel;
        then done;
    }

    // Standalone named succession.
    action def NamedFlow {
        action a;
        action b;
        action c;
        succession s1 first a then b;
        succession first b then c;
        succession then c then done;
    }

    // Action usage referencing an action def.
    part def Vehicle {
        action delivery : DeliverGifts;
    }
}
