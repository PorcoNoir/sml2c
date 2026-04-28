package BadFlowDef {
    item def Signal { }

    // Error: FlowDefinition may not have more than two ends.
    flow def TripleFlow {
        end a : Signal;
        end b : Signal;
        end c : Signal;
    }
}
