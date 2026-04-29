// TransitiveImports — exercises v0.18's recursive wildcard-import
// resolution.  Without the fix, `Vehicle` declared in PartDefs is
// only reachable from Configs::PartsTree if PartDefs is imported
// directly there.  With the fix, the chain of `public import` re-
// exports in the containing packages is followed transitively.
package Top {
    private import ScalarValues::*;
    public import Defs::*;

    package Defs {
        public import PartDefs::*;
        package PartDefs {
            part def Engine { attribute power : Real; }
            part def Vehicle {
                attribute mass : Real;
                part engine : Engine;
            }
        }
    }

    package Configs {
        package PartsTree {
            part vehicle : Vehicle {
                part engine : Engine;
            }
        }
    }
}
