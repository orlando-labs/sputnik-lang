#include "runtime/sputnik_ext.h"
#include <stdlib.h>
#include <string.h>

static const char counter_tag[] = "buffers.Counter.long.native.tag";
static int64_t counter_reclaims;

SputnikStatus call_buffers_counter_new(SputnikCtx *cx, const SputnikValue *args,
                                     size_t argc, SputnikValue *out) {
  int64_t value;
  if (argc != 1 || !sputnik_as_int(cx, args[0], &value))
    return sputnik_fault(cx, "TypeError", "expected Int");
  int64_t *counter = malloc(sizeof(*counter));
  if (counter == NULL) return sputnik_fault(cx, "ValueError", "allocation failed");
  *counter = value;
  *out = sputnik_make_handle(cx, counter_tag, counter);
  return SPUTNIK_OK;
}

SputnikStatus call_buffers_counter_read(SputnikCtx *cx, SputnikValue self,
                                      const SputnikValue *args, size_t argc,
                                      SputnikValue *out) {
  (void)args;
  if (argc != 0) return sputnik_fault(cx, "TypeError", "unexpected arguments");
  void *pointer;
  if (!sputnik_handle_ptr(cx, self, counter_tag, &pointer)) return SPUTNIK_ERR;
  *out = sputnik_make_int(cx, *(int64_t *)pointer);
  return SPUTNIK_OK;
}

void call_buffers_counter_free(void *pointer) {
  ++counter_reclaims;
  free(pointer);
}

SputnikStatus call_buffers_counter_reclaims(SputnikCtx *cx, const SputnikValue *args,
                                          size_t argc, SputnikValue *out) {
  (void)args;
  (void)argc;
  *out = sputnik_make_int(cx, counter_reclaims);
  return SPUTNIK_OK;
}

SputnikStatus call_buffers_sum(SputnikCtx *cx, const SputnikValue *args,
                             size_t argc, SputnikValue *out) {
  int64_t total = 0;
  for (size_t i = 0; i < argc; ++i) {
    int64_t value;
    if (!sputnik_as_int(cx, args[i], &value))
      return sputnik_fault(cx, "TypeError", "expected Int");
    total += value;
  }
  *out = sputnik_make_int(cx, total);
  return SPUTNIK_OK;
}

SputnikStatus call_buffers_churn(SputnikCtx *cx, const SputnikValue *args,
                               size_t argc, SputnikValue *out) {
  if (argc != 3 || !sputnik_is_list(cx, args[0]))
    return sputnik_fault(cx, "TypeError", "expected List, Str, block");
  const char *text;
  size_t length;
  if (!sputnik_str_view(cx, args[1], &text, &length) || length < 32)
    return sputnik_fault(cx, "TypeError", "expected long Str");
  const size_t count = sputnik_list_len(cx, args[0]);
  if (count != 64)
    return sputnik_fault(cx, "ValueError", "expected 64 items");
  SputnikValue saved[64];
  for (size_t i = 0; i < count; ++i) saved[i] = sputnik_list_at(cx, args[0], i);
  // Grow both the inline and overflow arenas before using old handles/views.
  for (size_t i = 0; i < 2048; ++i) sputnik_make_int(cx, (int64_t)i);
  int64_t total = 0;
  for (size_t i = 0; i < count; ++i) {
    int64_t value;
    if (!sputnik_as_int(cx, saved[i], &value) || value != (int64_t)i)
      return sputnik_fault(cx, "ValueError", "old handle changed after growth");
    total += value;
  }
  SputnikValue input = sputnik_make_int(cx, total);
  SputnikValue result = NULL;
  if (sputnik_call_block(cx, args[2], &input, 1, &result) != SPUTNIK_OK)
    return SPUTNIK_ERR;
  const char *after;
  size_t after_length;
  if (!sputnik_str_view(cx, args[1], &after, &after_length) ||
      length != after_length || memcmp(text, after, length) != 0)
    return sputnik_fault(cx, "ValueError", "borrowed text changed in callback");
  SputnikValue values[] = {args[0], sputnik_make_str(cx, text, length), result};
  *out = sputnik_make_list(cx, values, 3);
  return SPUTNIK_OK;
}

SputnikStatus call_buffers_fail(SputnikCtx *cx, const SputnikValue *args,
                              size_t argc, SputnikValue *out) {
  (void)args;
  (void)argc;
  (void)out;
  return sputnik_fault(cx, "ValueError", "expected failure");
}
