#include "runtime/stdlib_bool.h"
#include "runtime/stdlib_registry.h"

namespace amber::runtime {

namespace {

SendStatus bool_dispatch(NativeStdlibCall &call) {
  if (call.selector != "parse") {
    return SendStatus::NotHandled;
  }
  if (!call.require_arity(1) || !call.require_no_block() ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  if (!call.args[0].is_string()) {
    return call.fault("TypeError", "Bool.parse expects a Str");
  }
  const std::optional<std::string> text = call.text_of(call.args[0]);
  if (!text.has_value()) {
    return call.fault("VMError", "string ref is invalid");
  }
  bool parsed = false;
  if (!parse_bool_text(*text, &parsed)) {
    return call.fault("ValueError", "String content is not a Bool");
  }
  *call.out = Value::boolean(parsed);
  return SendStatus::Matched;
}

RuntimeNativeModuleDescriptor bool_module_descriptor() {
  return {{{"Bool", RuntimeNativeTypeKind::Bool}},
          {{RuntimeNativeTypeKind::Bool, &bool_dispatch}},
          {},
          {},
          {}};
}

} // namespace

void register_bool(NativeRegistry &registry) {
  register_native_module_descriptor(registry, bool_module_descriptor());
}

void register_bool_runtime_module(RuntimeModuleRegistry &modules,
                                  RuntimeDispatchRegistry &dispatch,
                                  RuntimeTypeRegistry &types) {
  const RuntimeNativeModuleDescriptor descriptor = bool_module_descriptor();
  register_runtime_module_descriptor(modules, descriptor);
  register_runtime_dispatch_descriptor(dispatch, descriptor);
  register_runtime_type_descriptor(types, descriptor);
}

} // namespace amber::runtime
