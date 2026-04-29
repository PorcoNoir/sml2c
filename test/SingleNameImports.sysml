// SingleNameImports — exercises v0.19's two fixes.
//
// Fix A: non-wildcard `import A::B::C;` brings `C` into scope as
// an alias for the resolved decl.  Without this, PTC's
// `public import ContextDefinitions::TransportPassenger;` left the
// trailing name undefined when used unqualified.
//
// Fix B: anonymous `perform A::B redefines C;` is visible by the
// name `C` so other code in the same scope can reach the
// redefining usage by bare name.  Without this, an outer
// `providePower.distributeTorque` lookup would resolve
// `providePower` to the inherited stub (no body) instead of the
// local redefiner that carries `:> ActionTree::providePower`.

package P {
    private import ScalarValues::*;

    package Lib {
        action def ProvidePower {
            in throttle : Real;
        }
        // The "real" implementation, with a subaction we want to
        // reach by member access through the redefining usage.
        package ActionTree {
            action providePower : ProvidePower {
                action distributeTorque;
            }
        }
        part def Vehicle {
            // The inherited stub — has the name but no body.
            perform action providePower;
        }
    }

    package Use {
        // Fix A: bring two single names from Lib into Use's scope.
        public import Lib::Vehicle;
        public import Lib::ActionTree::providePower;

        part vehicle_b : Vehicle {
            // Fix B: anonymous perform with `redefines providePower`.
            // The redefining usage's specializes carry the path that
            // gives access to `distributeTorque`; declaring it under
            // the redefined name makes the reference below resolve
            // to it (rather than to the empty inherited stub).
            perform Lib::ActionTree::providePower redefines providePower;
            part axle {
                // The bare-name lookup for `providePower` from here
                // walks scope chain → vehicle_b's inner scope, which
                // (post-fix) has `providePower` declared as an alias
                // for the redefining usage.
                perform providePower.distributeTorque;
            }
        }
    }
}
