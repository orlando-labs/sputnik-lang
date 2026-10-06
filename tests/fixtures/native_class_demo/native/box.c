/* Foreign-handle backing for `native class Box` (native-packages 5c-ii test).
 * A Box owns a malloc'd counter; the handle's `owned` lifetime frees it via
 * box_box_free on destroy!. */
#include "runtime/sputnik_ext.h"

#include <stdint.h>
#include <stdlib.h>

typedef struct {
  int64_t value;
} Box;

/* init(start) -> Box : constructor thunk, returns an owned handle. */
SputnikStatus box_box_new(SputnikCtx *cx, const SputnikValue *args, size_t argc,
                        SputnikValue *out) {
  int64_t start = 0;
  if (argc != 1 || !sputnik_as_int(cx, args[0], &start)) {
    return sputnik_fault(cx, "TypeError", "Box.init expects one Int");
  }
  Box *box = (Box *)malloc(sizeof(Box));
  if (box == NULL) {
    return sputnik_fault(cx, "RuntimeError", "out of memory");
  }
  box->value = start;
  *out = sputnik_make_handle(cx, "box.Box", box);
  return SPUTNIK_OK;
}

/* bump!(by) -> self : mutates the foreign resource, returns the same handle. */
SputnikStatus box_box_bump(SputnikCtx *cx, SputnikValue self, const SputnikValue *args,
                         size_t argc, SputnikValue *out) {
  void *handle = NULL;
  if (!sputnik_handle_ptr(cx, self, "box.Box", &handle)) {
    return SPUTNIK_ERR;
  }
  int64_t by = 0;
  if (argc != 1 || !sputnik_as_int(cx, args[0], &by)) {
    return sputnik_fault(cx, "TypeError", "bump! expects one Int");
  }
  if (by == 0) {
    /* A thunk-raised, rescuable error (sputnik_fault maps to a `rescue`-able
     * class), exercised by the fixture's `rescue ValueError`. */
    return sputnik_fault(cx, "ValueError", "bump! by zero");
  }
  ((Box *)handle)->value += by;
  *out = self;
  return SPUTNIK_OK;
}

/* value() -> Int */
SputnikStatus box_box_value(SputnikCtx *cx, SputnikValue self, const SputnikValue *args,
                          size_t argc, SputnikValue *out) {
  (void)args;
  (void)argc;
  void *handle = NULL;
  if (!sputnik_handle_ptr(cx, self, "box.Box", &handle)) {
    return SPUTNIK_ERR;
  }
  *out = sputnik_make_int(cx, ((Box *)handle)->value);
  return SPUTNIK_OK;
}

/* destroy!() : owned destructor, full runtime context permitted (unused here). */
void box_box_free(SputnikCtx *cx, void *handle) {
  (void)cx;
  free(handle);
}
