// BigInt values are immutable native leaves. Arithmetic shares the VM's exact
// implementation; constructing/operating on one never invokes a RuntimeWorld.
static const std::shared_ptr<sputnik::runtime::BigIntValue> &
native_bigint_value(const NativeValue &value) {
  if (value.tag != NativeValue::Tag::BigInt || value.heap_value == nullptr)
    throw NativeRaised{native_named_error("TypeError", "expected BigInt")};
  return static_cast<const NativeBigInt *>(value.heap_value)->value;
}

static std::shared_ptr<sputnik::runtime::BigIntValue>
native_bigint_operand(const NativeValue &value) {
  if (value.tag == NativeValue::Tag::BigInt) return native_bigint_value(value);
  if (value.tag == NativeValue::Tag::Integer)
    return sputnik::runtime::big_from_int64(value.scalar_value);
  throw NativeRaised{native_named_error(
      "TypeError", "BigInt arithmetic expects Int or BigInt operand")};
}

static NativeValue native_bigint_construct(const NativeValue &value) {
  using namespace sputnik::runtime;
  if (value.tag == NativeValue::Tag::BigInt) return value;
  if (value.tag == NativeValue::Tag::Integer)
    return NativeValue::big_int(big_from_int64(value.scalar_value));
  if (native_value_is_string(value)) {
    auto parsed = big_from_decimal_text(native_string_text(value));
    if (!parsed)
      throw NativeRaised{native_named_error("ValueError", "String content is not a BigInt")};
    return NativeValue::big_int(std::make_shared<BigIntValue>(std::move(*parsed)));
  }
  throw NativeRaised{native_named_error("TypeError", "cannot cast value to BigInt")};
}

static NativeValue native_bigint_to_int(const NativeValue &value) {
  const auto converted = sputnik::runtime::big_to_int64(*native_bigint_value(value));
  if (!converted)
    throw NativeRaised{native_named_error("OverflowError", "BigInt value is out of Int range")};
  return NativeValue::integer(*converted);
}

static bool native_bigint_pair(const NativeValue &lhs, const NativeValue &rhs) {
  return lhs.tag == NativeValue::Tag::BigInt || rhs.tag == NativeValue::Tag::BigInt;
}

static NativeValue native_bigint_binary(const NativeValue &lhs,
                                       const NativeValue &rhs,
                                       std::string_view operation) {
  using namespace sputnik::runtime;
  const auto left = native_bigint_operand(lhs);
  const auto right = native_bigint_operand(rhs);
  if (operation == "+") return NativeValue::big_int(big_add_signed(*left, *right));
  if (operation == "-") return NativeValue::big_int(big_add_signed(*left, *big_negate(*right)));
  if (operation == "*") return NativeValue::big_int(big_mul_signed(*left, *right));
  if (operation == "/" || operation == "//" || operation == "%") {
    if (right->magnitude.empty())
      throw NativeRaised{native_named_error("ZeroDivisionError",
          operation == "%" ? "modulo by zero" : "division by zero")};
    if (operation == "%") return NativeValue::big_int(big_floor_mod(*left, *right));
    if (operation == "//") return NativeValue::big_int(big_floor_div(*left, *right));
    std::shared_ptr<BigIntValue> quotient, remainder;
    big_divmod_trunc(*left, *right, &quotient, &remainder);
    return NativeValue::big_int(std::move(quotient));
  }
  if (operation == "**") {
    const auto exponent = big_to_int64(*right);
    if (!exponent || *exponent < 0)
      throw NativeRaised{native_named_error("TypeError", "BigInt ** expects a non-negative Int exponent")};
    return NativeValue::big_int(big_pow(*left, static_cast<std::uint64_t>(*exponent)));
  }
  const int order = big_compare_signed(*left, *right);
  if (operation == "<") return NativeValue::boolean(order < 0);
  if (operation == ">") return NativeValue::boolean(order > 0);
  if (operation == "<=") return NativeValue::boolean(order <= 0);
  if (operation == ">=") return NativeValue::boolean(order >= 0);
  return NativeValue::integer(order);
}
