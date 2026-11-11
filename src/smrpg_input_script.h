#ifndef SMRPG_INPUT_SCRIPT_H
#define SMRPG_INPUT_SCRIPT_H
/* Shared by the desktop and headless hosts for deterministic unattended QA. */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
typedef struct InputSpan {
  long first;
  long last;
  uint32_t mask;
} InputSpan;

enum { kMaxInputSpans = 128 };

static int parse_input_script(InputSpan spans[kMaxInputSpans],
                              size_t *count_out) {
  const char *cursor = getenv("SNESRECOMP_INPUT_SCRIPT");
  *count_out = 0;
  if (!cursor || !cursor[0]) return 1;
  while (*cursor) {
    while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') cursor++;
    if (!*cursor) break;
    if (*count_out >= kMaxInputSpans) return 0;
    char *end = NULL;
    long first = strtol(cursor, &end, 0);
    if (end == cursor || first < 0) return 0;
    cursor = end;
    long last = first;
    if (*cursor == '-') {
      cursor++;
      last = strtol(cursor, &end, 0);
      if (end == cursor || last < first) return 0;
      cursor = end;
    }
    if (*cursor++ != ':') return 0;
    unsigned long mask = strtoul(cursor, &end, 0);
    if (end == cursor || mask > 0x0fffu) return 0;
    cursor = end;
    while (*cursor == ' ' || *cursor == '\t') cursor++;
    if (*cursor && *cursor != ',') return 0;
    spans[*count_out].first = first;
    spans[*count_out].last = last;
    spans[*count_out].mask = (uint32_t)mask;
    (*count_out)++;
  }
  return 1;
}

static uint32_t scripted_input(const InputSpan *spans, size_t count,
                               long frame) {
  uint32_t input = 0;
  for (size_t i = 0; i < count; i++) {
    if (frame >= spans[i].first && frame <= spans[i].last)
      input |= spans[i].mask;
  }
  return input;
}

#endif
