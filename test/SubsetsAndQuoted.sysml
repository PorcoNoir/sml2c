package SubsetsAndQuoted {
    private import ScalarValues::*;

    requirement def MassReq;

    // Bare requirement usage (relaxed in v0.5 — was rejected in v0.2).
    requirement vehicleMassReq : MassReq;

    // Single-quoted lexical names (allowed for hyphenated/special-char names).
    part def 'special-name';
    action def 'another-name';

    part def Engine {
        action a;
        action b;

        // Action subsets (usage-side analogue of :>).
        action driverAction subsets a;
    }
}
