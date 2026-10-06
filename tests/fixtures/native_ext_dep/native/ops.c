#include "ops.h"

SputnikStatus sputnik_dep_doubled(SputnikCtx *cx, const SputnikValue *args,
                              size_t argc, SputnikValue *out) {
  int64_t n = 0;
  if (argc != 1 || !sputnik_as_int(cx, args[0], &n)) {
    return sputnik_fault(cx, "TypeError", "doubled expects one Int argument");
  }
  *out = sputnik_make_int(cx, n * 2);
  return SPUTNIK_OK;
}
