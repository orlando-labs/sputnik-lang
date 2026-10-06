#include "runtime/amber_ext.h"

AmberStatus errors_pending(AmberCtx *cx, const AmberValue *args,
                           size_t argc, AmberValue *out) {
  (void)args;
  (void)argc;
  (void)out;
  return amber_fault(cx, "NotImplementedError", "native pending");
}
