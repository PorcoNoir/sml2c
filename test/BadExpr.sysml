package BadExpr {
    part def X {
        attribute a = ;             // missing expression
        attribute b = 1 + ;         // missing right operand
        attribute c = (1 + 2;       // unclosed paren
        attribute d = + 5;          // '+' has no prefix rule
    }
}
