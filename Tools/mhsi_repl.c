#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mhsi.h"
#include "mhseval.h"

static void chomp(char *line) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
}

int main(void) {
    mhsi_interpreter *interp = mhsi_new();
    if (!interp) {
        fprintf(stderr, "failed to initialize mhsi interpreter\n");
        return 1;
    }

    puts("Tiny mhsi REPL – enter expressions (use :quit to exit).");

    char line[4096];
    while (1) {
        fputs("mhsi> ", stdout);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            putchar('\n');
            break;
        }
        chomp(line);
        if (line[0] == '\0') {
            continue;
        }
        if (strcmp(line, ":quit") == 0 || strcmp(line, ":q") == 0) {
            break;
        }

        const char *prefix = "module Repl where\nmain :: IO ()\nmain = print (";
        const char *suffix = ")\n";
        size_t src_len = strlen(prefix) + strlen(line) + strlen(suffix);
        char *source = malloc(src_len + 1);
        if (!source) {
            fprintf(stderr, "allocation failed\n");
            continue;
        }
        snprintf(source, src_len + 1, "%s%s%s", prefix, line, suffix);

        if (mhsi_run(interp, source, src_len) != 0) {
            fprintf(stderr, "error: %s\n", mhsi_last_error(interp));
        }
        free(source);
    }

    mhsi_free(interp);
    return 0;
}
