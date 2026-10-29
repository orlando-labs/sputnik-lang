// Included by the generated native runtime after user-method dispatch.
static NativeValue native_prepare_membership_map(
    const NativeValue &source, const NativeArgsView &keys) {
  NativeValue value = source;
  if (value.tag != NativeValue::Tag::Map) {
    if (!native_value_is_user_receiver(value)) return NativeValue::nullv();
    const bool class_side = value.tag == NativeValue::Tag::Class;
    const auto owner = class_side
        ? static_cast<std::uint32_t>(value.scalar_value)
        : (value.tag == NativeValue::Tag::ForeignHandle
            ? as_native_foreign_handle(value)->class_index
            : as_native_instance(value)->class_index);
    if (!native_lookup_user_method(owner, "deconstruct_keys", class_side))
      return NativeValue::nullv();
    value = native_user_send(value, "deconstruct_keys",
        {NativeValue::list(std::vector<NativeValue>(keys.begin(), keys.end()))}, {});
    if (value.tag == NativeValue::Tag::Null) return value;
    if (value.tag != NativeValue::Tag::Map)
      throw NativeRaised{native_named_error("TypeError", "deconstruct_keys must return Map or null")};
  }
  return native_copy(value, false);
}

static NativeValue native_membership_send(
    const NativeValue &receiver, const std::string &selector,
    const NativeArgsView &args, const NativeKeywordArgsView &kwargs,
    NativeValue block, bool dispatch_override) {
  if (dispatch_override && native_value_is_user_receiver(receiver)) {
    const bool class_side = receiver.tag == NativeValue::Tag::Class;
    const auto owner = class_side
        ? static_cast<std::uint32_t>(receiver.scalar_value)
        : (receiver.tag == NativeValue::Tag::ForeignHandle
            ? as_native_foreign_handle(receiver)->class_index
            : as_native_instance(receiver)->class_index);
    if (native_lookup_user_method(owner, selector, class_side))
      return native_user_send(receiver, selector, args, kwargs, block);
  }
  if (!kwargs.empty() || block.tag != NativeValue::Tag::Null ||
      (selector == "in?" && args.size() != 1U)) {
    throw NativeRaised{native_named_error("TypeError", selector == "in?"
        ? "in? expects one container and no keywords or block"
        : "one_of? accepts positional targets only")};
  }
  if (selector == "in?") {
    const auto &container = args[0];
    NativeValue result;
    if (native_value_is_user_receiver(container)) {
      const bool class_side = container.tag == NativeValue::Tag::Class;
      const auto owner = class_side
          ? static_cast<std::uint32_t>(container.scalar_value)
          : (container.tag == NativeValue::Tag::ForeignHandle
              ? as_native_foreign_handle(container)->class_index
              : as_native_instance(container)->class_index);
      if (!native_lookup_user_method(owner, "contains?", class_side))
        throw NativeRaised{native_named_error("TypeError",
            "in? expects a container supporting contains?")};
      result = native_user_send(container, "contains?", {receiver}, {});
    } else if (native_value_is_string(container) || native_is_sequence(container) ||
               container.tag == NativeValue::Tag::Map) {
      result = native_contains(container, receiver);
    } else {
      throw NativeRaised{native_named_error("TypeError",
          "in? expects a container supporting contains?")};
    }
    if (result.tag != NativeValue::Tag::Bool)
      throw NativeRaised{native_named_error("TypeError", "contains? must return Bool")};
    return result;
  }
  for (const auto &target : args) {
    const NativeValue equal = native_value_is_user_receiver(receiver)
        ? native_user_send(receiver, "==", {target}, {})
        : NativeValue::boolean(native_value_equal(receiver, target));
    if (equal.tag != NativeValue::Tag::Bool)
      throw NativeRaised{native_named_error("TypeError", "one_of? equality must return Bool")};
    if (equal.scalar_value != 0) return NativeValue::boolean(true);
  }
  return NativeValue::boolean(false);
}
