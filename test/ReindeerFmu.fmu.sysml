/*
 * ReindeerFmu.fmu.sysml — v0.27 fixture for `--emit-fmu-c`.
 *
 * Exercises the conjugate-port causality flip on the Reindeer side
 * of the Sleigh-Reindeer running example:
 *
 *   - port harnessLoop : ~HarnessConnectionPoint   (note the `~`)
 *   - the same port def as SleighFmu, but every item's effective
 *     causality FLIPS:
 *       out item command  → causality="input"   (was output)
 *       in  item feedback → causality="output"  (was input)
 *   - <Terminal name="harnessLoop"> with the same matching rule
 *     so an importer can pair it with a Sleigh's eyelet across two
 *     FMUs.
 *
 * The non-conjugate path is in SleighFmu.fmu.sysml.  Together they
 * verify that the same port-def schema produces opposite causalities
 * on the two sides of a connection.
 */
package ReindeerFmu {
    part def Reindeer {
        port harnessLoop : ~HarnessConnectionPoint;
    }

    /* Port definition shared between Sleigh and Reindeer.  In an
     * importer that recognizes our matchingRule, a Sleigh's eyelet
     * and a Reindeer's harnessLoop should auto-wire as conjugate
     * ports of the same shape.                                     */
    port def HarnessConnectionPoint {
        out item command  : Command;
        in  item feedback : Feedback;
    }

    item def Command;
    item def Feedback;
}
