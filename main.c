/* sysmlc — main.c
 *
 *   ./sysmlc                       parse built-in sample, print AST
 *   ./sysmlc file.sysml            parse file, print AST
 *   ./sysmlc --tokens [file]       just dump the token stream (old behavior)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scanner.h"
#include "parser.h"
#include "ast.h"

static const char* SAMPLE =
    "package MBSEPodcast {\n"
    "    import ScalarValues::*;\n"
    "\n"
    "    /* A trivial system definition. */\n"
    "    part def SimpleSystem {\n"
    "        attribute mass : Real;\n"
    "    }\n"
    "\n"
    "    // a usage of that definition\n"
    "    part mySystem : SimpleSystem {\n"
    "        part p1 : SimpleSystem;\n"
    "        part p2 : SimpleSystem;\n"
    "        part p3 : SimpleSystem;\n"
    "    }\n"
    "}\n";

static char* readFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Could not open %s\n", path); exit(74); }
    fseek(f, 0L, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char* buf = (char*)malloc(size + 1);
    if (!buf) { fprintf(stderr, "Out of memory\n"); exit(74); }
    size_t got = fread(buf, 1, size, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static void dumpTokens(const char* source) {
    initScanner(source);
    int prevLine = -1;
    for (;;) {
        Token t = scanToken();
        if (t.line != prevLine) { printf("%4d ", t.line); prevLine = t.line; }
        else                     printf("   | ");
        printf("%-13s '%.*s'\n", tokenTypeName(t.type), t.length, t.start);
        if (t.type == TOKEN_EOF) break;
    }
}

int main(int argc, char** argv) {
    bool tokensOnly = false;
    const char* path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tokens") == 0) tokensOnly = true;
        else                                  path = argv[i];
    }

    char* allocated = NULL;
    const char* source = SAMPLE;
    if (path) { allocated = readFile(path); source = allocated; }

    if (tokensOnly) {
        dumpTokens(source);
        free(allocated);
        return 0;
    }

    initScanner(source);
    Node* root = parse();
    if (parserHadError()) {
        fprintf(stderr, "Parse failed.\n");
        free(allocated);
        return 65;
    }
    astPrint(root);
    free(allocated);
    return 0;
}