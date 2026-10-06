#include "runtime/sputnik_ext.h"

SputnikStatus errors_pending(SputnikCtx *cx, const SputnikValue *args,
                           size_t argc, SputnikValue *out) {
  (void)args;
  (void)argc;
  (void)out;
  return sputnik_fault(cx, "NotImplementedError", "native pending");
}
