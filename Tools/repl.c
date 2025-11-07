#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Repl_stub.h"

static char *
trim(char *line)
{
  while (*line && isspace((unsigned char)*line)) {
    ++line;
  }
  size_t len = strlen(line);
  while (len > 0 && isspace((unsigned char)line[len - 1])) {
    line[--len] = '\0';
  }
  return line;
}

static bool
looks_like_definition(const char *line)
{
  static const char *keywords[] = {
    "import", "data", "newtype", "type", "class", "instance",
    "foreign", "infix", "infixl", "infixr", "default", NULL
  };
  for (const char **kw = keywords; *kw; ++kw) {
    size_t n = strlen(*kw);
    if (strncmp(line, *kw, n) == 0 && (line[n] == '\0' || isspace((unsigned char)line[n]))) {
      return true;
    }
  }
  const char *p = line;
  while ((p = strchr(p, '=')) != NULL) {
    char prev = (p > line) ? p[-1] : '\0';
    char next = p[1];
    if (prev == '=' || prev == '<' || prev == '>' || prev == '/' ||
        next == '=' || next == '<' || next == '>' ) {
      ++p;
      continue;
    }
    return true;
  }
  return false;
}

static bool
define_snippet(uintptr_t ctx, const char *line)
{
  char *err = NULL;
  intptr_t rc = mhs_repl_define(ctx, (void *)line, (uintptr_t)strlen(line), (void *)&err);
  if (rc == 0) {
    if (err) {
      free(err);
    }
    return true;
  }
  if (err) {
    fprintf(stderr, "%s\n", err);
    free(err);
  }
  return false;
}

static bool
eval_expr(uintptr_t ctx, const char *line, char **err_out)
{
  char *out = NULL;
  size_t out_len = 0;
  char *err = NULL;
  intptr_t rc = mhs_repl_eval(ctx,
                              (void *)line,
                              (uintptr_t)strlen(line),
                              (void *)&out,
                              (void *)&out_len,
                              (void *)&err);
  if (rc == 0) {
    printf("%.*s\n", (int)out_len, out);
    free(out);
    if (err) {
      printf("%s\n", err);
      free(err);
    }
    *err_out = NULL;
    return true;
  }
  if (out) {
    free(out);
  }
  *err_out = err;
  return false;
}

static bool
run_stmt(uintptr_t ctx, const char *line, char **pending_err)
{
  char *err = NULL;
  intptr_t rc = mhs_repl_run(ctx,
                             (void *)line,
                             (uintptr_t)strlen(line),
                             (void *)&err);
  if (rc == 0) {
    if (err) {
      printf("%s\n", err);
      free(err);
    }
    if (*pending_err) {
      free(*pending_err);
      *pending_err = NULL;
    }
    return true;
  }
  if (*pending_err) {
    fprintf(stderr, "%s\n", *pending_err);
    free(*pending_err);
    *pending_err = NULL;
  }
  if (err) {
    fprintf(stderr, "%s\n", err);
    free(err);
  }
  return false;
}

int
main(void)
{
  char buffer[4096];

  mhs_init();
  uintptr_t ctx = mhs_repl_new();

  puts("Type ':quit' to quit, ':help' for help");

  while (true) {
    fputs("> ", stdout);
    fflush(stdout);
    if (!fgets(buffer, sizeof buffer, stdin)) {
      break;
    }
    char *line = trim(buffer);
    if (*line == '\0') {
      continue;
    }
    if (strcmp(line, ":quit") == 0) {
      break;
    }
    if (strcmp(line, ":help") == 0) {
      puts("Enter definitions (e.g. 'a = 1') or expressions (e.g. 'a').");
      puts("Type ':quit' to exit.");
      continue;
    }

    if (looks_like_definition(line)) {
      (void)define_snippet(ctx, line);
      continue;
    }

    char *eval_err = NULL;
    if (eval_expr(ctx, line, &eval_err)) {
      continue;
    }
    (void)run_stmt(ctx, line, &eval_err);
  }

  mhs_repl_free(ctx);
  return 0;
}
