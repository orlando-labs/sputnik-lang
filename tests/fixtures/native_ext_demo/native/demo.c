/* Native acceleration for `native def doubled` (native-packages 5c-ii test).
 * Deliberately returns a different value than the Sputnik fallback body
 * (n * 2, not n * 10) so a passing test proves the native binary calls this
 * thunk rather than running the bytecode body. */
#include "runtime/sputnik_ext.h"

SputnikStatus sputnik_demo_doubled(SputnikCtx *cx, const SputnikValue *args,
                               size_t argc, SputnikValue *out) {
  int64_t n = 0;
  if (argc != 1 || !sputnik_as_int(cx, args[0], &n)) {
    return sputnik_fault(cx, "TypeError", "doubled expects one Int argument");
  }
  *out = sputnik_make_int(cx, n * 2);
  return SPUTNIK_OK;
}

SputnikStatus sputnik_demo_fail_leaf(SputnikCtx *cx, const SputnikValue *args,
                                 size_t argc, SputnikValue *out) {
  (void)args;
  (void)out;
  if (argc != 0) {
    return sputnik_fault(cx, "TypeError", "fail_leaf expects no arguments");
  }
  return sputnik_fault(cx, "Demo.NativeLeafError", "native leaf failed");
}
