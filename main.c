/* sysmlc — main.c
 *
 *   ./sysmlc                          full pipeline, print AST
 *   ./sysmlc file.sysml               same, on a file
 *   ./sysmlc --tokens [file]          just dump the token stream
 *   ./sysmlc --emit-json [file]       emit AST as JSON instead of pretty-printed tree
 *   ./sysmlc --emit-sysml [file]      emit canonical SysML (round-trip)
 *   ./sysmlc --emit-c [file]          emit C header (typed-AST first slice)
 *   ./sysmlc --no-resolve [file]      skip resolver and later passes
 *   ./sysmlc --no-typecheck [file]    skip typechecker and later passes
 *   ./sysmlc --no-redefcheck [file]   skip redefinition checker
 *   ./sysmlc --no-connectcheck [file] skip connection checker
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scanner.h"
#include "parser.h"
#include "ast.h"
#include "resolver.h"
#include "typechecker.h"
#include "redefchecker.h"
#include "connectchecker.h"
#include "referentialchecker.h"
#include "codegen_json.h"
#include "codegen_sysml.h"
#include "codegen_c.h"

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
    bool emitJsonMode = false;
    bool emitSysmlMode = false;
    bool emitCMode = false;
    bool skipResolve = false;
    bool skipTypecheck = false;
    bool skipRedefcheck = false;
    bool skipConnectcheck = false;
    bool skipRefcheck = false;
    const char* path = NULL;
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--tokens")          == 0) tokensOnly       = true;
        else if (strcmp(argv[i], "--emit-json")       == 0) emitJsonMode     = true;
        else if (strcmp(argv[i], "--emit-sysml")      == 0) emitSysmlMode    = true;
        else if (strcmp(argv[i], "--emit-c")          == 0) emitCMode        = true;
        else if (strcmp(argv[i], "--no-resolve")      == 0) skipResolve      = true;
        else if (strcmp(argv[i], "--no-typecheck")    == 0) skipTypecheck    = true;
        else if (strcmp(argv[i], "--no-redefcheck")   == 0) skipRedefcheck   = true;
        else if (strcmp(argv[i], "--no-connectcheck") == 0) skipConnectcheck = true;
        else if (strcmp(argv[i], "--no-refcheck")     == 0) skipRefcheck     = true;
        else                                                path = argv[i];
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

    if (!skipResolve) {
        if (!resolveProgram(root))                          { free(allocated); return 65; }
        if (!skipTypecheck    && !typecheckProgram(root))   { free(allocated); return 65; }
        if (!skipRedefcheck   && !checkRedefinitions(root)) { free(allocated); return 65; }
        if (!skipConnectcheck && !checkConnections(root))   { free(allocated); return 65; }
        if (!skipRefcheck     && !checkReferential(root))   { free(allocated); return 65; }
    }

    if      (emitSysmlMode) emitSysml(stdout, root);
    else if (emitJsonMode)  emitJson(stdout, root);
    else if (emitCMode)     emitC(stdout, root);
    else                    astPrint(root);

    free(allocated);
    return 0;
}
