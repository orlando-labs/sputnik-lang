// Direct ByteBuffer/ByteSlice adapters for generated native code. The backing
// buffers and confinement checks are shared with the VM; no bytecode is run.
static void native_byte_buffer_error(const std::string &name,
                                     const std::string &message) {
  throw NativeRaised{native_named_error(name, message)};
}

static void native_byte_buffer_check(
    const sputnik::runtime::RuntimeIoStatus &status) {
  if (!status.ok)
    native_byte_buffer_error(status.error_name.empty() ? "IOError" : status.error_name,
                             status.message);
}

static std::string native_byte_buffer_bytes(const NativeValue &value) {
  using namespace sputnik::runtime;
  if (value.tag == NativeValue::Tag::Bytes) return as_bytes(value).bytes;
  if (native_value_is_string(value)) return native_string_text(value);
  if (value.tag == NativeValue::Tag::RuntimeHandle) {
    const auto &runtime = as_native_runtime_handle(value)->value;
    if (runtime.is_io_value()) {
      if (const auto bytes = std::dynamic_pointer_cast<RuntimeBytes>(runtime.as_io_value()))
        return bytes->string();
      if (const auto buffer = std::dynamic_pointer_cast<RuntimeByteBuffer>(runtime.as_io_value())) {
        native_byte_buffer_check(buffer->access_status());
        return buffer->bytes();
      }
      if (const auto slice = std::dynamic_pointer_cast<RuntimeByteSlice>(runtime.as_io_value()))
        return slice->bytes()->string();
    }
  }
  native_byte_buffer_error("TypeError", "expected Bytes, ByteSlice, ByteBuffer, or Str");
  return {};
}

static NativeValue native_byte_buffer_factory(const NativeArgsView &args,
                                               NativeValue block) {
  using namespace sputnik::runtime;
  if (block.tag != NativeValue::Tag::Null || args.size() > 1U)
    native_byte_buffer_error("TypeError", "ByteBuffer expects one capacity and no block");
  if (args.empty())
    return NativeValue::runtime_handle(Value::native_type(RuntimeNativeTypeKind::ByteBuffer));
  if (args[0].tag != NativeValue::Tag::Integer)
    native_byte_buffer_error("TypeError", "ByteBuffer capacity must be Int");
  if (args[0].scalar_value < 0)
    native_byte_buffer_error("ArgumentError", "ByteBuffer capacity must be non-negative");
  return NativeValue::runtime_handle(Value::io_value(
      std::make_shared<RuntimeByteBuffer>(static_cast<std::size_t>(args[0].scalar_value))));
}

static bool native_byte_buffer_receiver(const NativeValue &receiver) {
  using namespace sputnik::runtime;
  if (receiver.tag != NativeValue::Tag::RuntimeHandle) return false;
  const auto &value = as_native_runtime_handle(receiver)->value;
  if (value.is_native_type()) return value.as_native_type().kind == RuntimeNativeTypeKind::ByteBuffer;
  if (!value.is_io_value()) return false;
  return std::dynamic_pointer_cast<RuntimeByteBuffer>(value.as_io_value()) != nullptr ||
         std::dynamic_pointer_cast<RuntimeByteSlice>(value.as_io_value()) != nullptr;
}

static NativeValue native_byte_buffer_send(
    const NativeValue &receiver, const std::string &selector,
    const NativeArgsView &args, const NativeKeywordArgsView &kwargs,
    NativeValue block) {
  using namespace sputnik::runtime;
  if (!kwargs.empty() || block.tag != NativeValue::Tag::Null)
    native_byte_buffer_error("TypeError", "ByteBuffer does not accept keywords or a block");
  const auto arity = [&](std::size_t count) {
    if (args.size() != count)
      native_byte_buffer_error("TypeError", "wrong native stdlib SEND arity");
  };
  const auto &value = as_native_runtime_handle(receiver)->value;
  if (value.is_native_type()) {
    arity(1);
    if (selector == "new" || selector == "__call__")
      return native_byte_buffer_factory(args, block);
    if (selector == "wrap")
      return NativeValue::runtime_handle(Value::io_value(std::make_shared<RuntimeByteBuffer>(
          RuntimeBytes(native_byte_buffer_bytes(args[0])))));
    native_byte_buffer_error("NoMethodError", "unknown ByteBuffer constructor: " + selector);
  }
  if (const auto slice = std::dynamic_pointer_cast<RuntimeByteSlice>(value.as_io_value())) {
    arity(0);
    if (selector == "count") return NativeValue::integer(slice->count());
    if (selector == "shareable?") return NativeValue::boolean(slice->shareable());
    if (selector == "owner") return slice->owner()
        ? NativeValue::runtime_handle(Value::io_value(slice->owner())) : NativeValue::nullv();
    if (selector == "bytes" || selector == "copy_bytes")
      return NativeValue::bytes((selector == "bytes" ? slice->bytes() : slice->copy_bytes())->string());
    native_byte_buffer_error("NoMethodError", "unknown ByteSlice method: " + selector);
  }
  const auto buffer = std::dynamic_pointer_cast<RuntimeByteBuffer>(value.as_io_value());
  native_byte_buffer_check(buffer->access_status());
  if (selector == "capacity" || selector == "position" || selector == "limit" ||
      selector == "count" || selector == "remaining" || selector == "empty?" || selector == "full?") {
    arity(0);
    if (selector == "empty?") return NativeValue::boolean(buffer->empty());
    if (selector == "full?") return NativeValue::boolean(buffer->full());
    return NativeValue::integer(selector == "capacity" ? buffer->capacity() :
        selector == "position" ? buffer->position() : selector == "limit" ? buffer->limit() :
        selector == "count" ? buffer->count() : buffer->remaining());
  }
  if (selector == "clear!" || selector == "flip!" || selector == "rewind!" || selector == "compact!") {
    arity(0);
    native_byte_buffer_check(selector == "clear!" ? buffer->clear() : selector == "flip!" ? buffer->flip() :
                             selector == "rewind!" ? buffer->rewind() : buffer->compact());
    return selector == "clear!" ? NativeValue::nullv() : receiver;
  }
  if (selector == "get!" || selector == "get_at") {
    arity(selector == "get!" ? 0U : 1U);
    if (selector == "get_at" && args[0].tag != NativeValue::Tag::Integer)
      native_byte_buffer_error("TypeError", "ByteBuffer index must be Int");
    const auto result = selector == "get!" ? buffer->get() : buffer->get_at(args[0].scalar_value);
    native_byte_buffer_check(result);
    return NativeValue::integer(result.byte);
  }
  if (selector == "put!" || selector == "put_all!") {
    arity(1);
    if (selector == "put!" && args[0].tag != NativeValue::Tag::Integer)
      native_byte_buffer_error("TypeError", "ByteBuffer byte must be Int");
    native_byte_buffer_check(selector == "put!" ? buffer->put(args[0].scalar_value) :
                             buffer->put_all(native_byte_buffer_bytes(args[0])));
    return receiver;
  }
  if (selector == "slice" || selector == "read_slice" || selector == "write_slice") {
    const bool indexed = selector == "slice";
    if (args.size() > (indexed ? 2U : 1U) || (indexed && args.empty()))
      native_byte_buffer_error("TypeError", "wrong slice argument count");
    if (indexed && args[0].tag != NativeValue::Tag::Integer)
      native_byte_buffer_error("TypeError", "ByteBuffer.slice expects Int start");
    std::optional<std::size_t> length;
    const std::size_t offset = indexed ? 1U : 0U;
    if (args.size() > offset && args[offset].tag != NativeValue::Tag::Null) {
      if (args[offset].tag != NativeValue::Tag::Integer)
        native_byte_buffer_error("TypeError", "slice length must be Int or null");
      if (args[offset].scalar_value < 0)
        native_byte_buffer_error("ArgumentError", "slice length must be non-negative");
      length = static_cast<std::size_t>(args[offset].scalar_value);
    }
    if (indexed) {
      std::int64_t start = args[0].scalar_value;
      if (start < 0) start += static_cast<std::int64_t>(buffer->count());
      if (start < 0 || static_cast<std::uint64_t>(start) > buffer->count())
        native_byte_buffer_error("IndexError", "ByteBuffer slice is out of bounds");
      return NativeValue::runtime_handle(Value::io_value(buffer->byte_slice(args[0].scalar_value, length)));
    }
    if ((selector == "read_slice") != buffer->read_mode())
      native_byte_buffer_error("ArgumentError", selector == "read_slice"
          ? "read_slice requires read mode" : "write_slice requires write mode");
    return NativeValue::runtime_handle(Value::io_value(selector == "read_slice"
        ? buffer->read_slice(length) : buffer->write_slice(length)));
  }
  if (selector == "bytes" || selector == "copy_bytes" || selector == "freeze_bytes!") {
    arity(0);
    return NativeValue::bytes(selector == "bytes" ? buffer->bytes() :
        (selector == "copy_bytes" ? buffer->copy_bytes() : buffer->freeze_bytes())->string());
  }
  native_byte_buffer_error("NoMethodError", "unknown ByteBuffer method: " + selector);
  return NativeValue::nullv();
}
