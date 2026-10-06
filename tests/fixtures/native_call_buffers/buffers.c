#include "runtime/amber_ext.h"
#include <stdlib.h>
#include <string.h>

static const char counter_tag[] = "buffers.Counter.long.native.tag";
static int64_t counter_reclaims;

AmberStatus call_buffers_counter_new(AmberCtx *cx, const AmberValue *args,
                                     size_t argc, AmberValue *out) {
  int64_t value;
  if (argc != 1 || !amber_as_int(cx, args[0], &value))
    return amber_fault(cx, "TypeError", "expected Int");
  int64_t *counter = malloc(sizeof(*counter));
  if (counter == NULL) return amber_fault(cx, "ValueError", "allocation failed");
  *counter = value;
  *out = amber_make_handle(cx, counter_tag, counter);
  return AMBER_OK;
}

AmberStatus call_buffers_counter_read(AmberCtx *cx, AmberValue self,
                                      const AmberValue *args, size_t argc,
                                      AmberValue *out) {
  (void)args;
  if (argc != 0) return amber_fault(cx, "TypeError", "unexpected arguments");
  void *pointer;
  if (!amber_handle_ptr(cx, self, counter_tag, &pointer)) return AMBER_ERR;
  *out = amber_make_int(cx, *(int64_t *)pointer);
  return AMBER_OK;
}

void call_buffers_counter_free(void *pointer) {
  ++counter_reclaims;
  free(pointer);
}

AmberStatus call_buffers_counter_reclaims(AmberCtx *cx, const AmberValue *args,
                                          size_t argc, AmberValue *out) {
  (void)args;
  (void)argc;
  *out = amber_make_int(cx, counter_reclaims);
  return AMBER_OK;
}

AmberStatus call_buffers_sum(AmberCtx *cx, const AmberValue *args,
                             size_t argc, AmberValue *out) {
  int64_t total = 0;
  for (size_t i = 0; i < argc; ++i) {
    int64_t value;
    if (!amber_as_int(cx, args[i], &value))
      return amber_fault(cx, "TypeError", "expected Int");
    total += value;
  }
  *out = amber_make_int(cx, total);
  return AMBER_OK;
}

AmberStatus call_buffers_churn(AmberCtx *cx, const AmberValue *args,
                               size_t argc, AmberValue *out) {
  if (argc != 3 || !amber_is_list(cx, args[0]))
    return amber_fault(cx, "TypeError", "expected List, Str, block");
  const char *text;
  size_t length;
  if (!amber_str_view(cx, args[1], &text, &length) || length < 32)
    return amber_fault(cx, "TypeError", "expected long Str");
  const size_t count = amber_list_len(cx, args[0]);
  if (count != 64)
    return amber_fault(cx, "ValueError", "expected 64 items");
  AmberValue saved[64];
  for (size_t i = 0; i < count; ++i) saved[i] = amber_list_at(cx, args[0], i);
  // Grow both the inline and overflow arenas before using old handles/views.
  for (size_t i = 0; i < 2048; ++i) amber_make_int(cx, (int64_t)i);
  int64_t total = 0;
  for (size_t i = 0; i < count; ++i) {
    int64_t value;
    if (!amber_as_int(cx, saved[i], &value) || value != (int64_t)i)
      return amber_fault(cx, "ValueError", "old handle changed after growth");
    total += value;
  }
  AmberValue input = amber_make_int(cx, total);
  AmberValue result = NULL;
  if (amber_call_block(cx, args[2], &input, 1, &result) != AMBER_OK)
    return AMBER_ERR;
  const char *after;
  size_t after_length;
  if (!amber_str_view(cx, args[1], &after, &after_length) ||
      length != after_length || memcmp(text, after, length) != 0)
    return amber_fault(cx, "ValueError", "borrowed text changed in callback");
  AmberValue values[] = {args[0], amber_make_str(cx, text, length), result};
  *out = amber_make_list(cx, values, 3);
  return AMBER_OK;
}

AmberStatus call_buffers_fail(AmberCtx *cx, const AmberValue *args,
                              size_t argc, AmberValue *out) {
  (void)args;
  (void)argc;
  (void)out;
  return amber_fault(cx, "ValueError", "expected failure");
}
