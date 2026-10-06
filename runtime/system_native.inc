// Included by generated C++ after native value and closure support. This
// adapter deliberately never constructs RuntimeWorld or invokes a VM bridge.
static bool native_system_receiver(const NativeValue &value) {
  if (value.tag != NativeValue::Tag::RuntimeHandle)
    return false;
  const auto &runtime = as_native_runtime_handle(value)->value;
  return (runtime.is_native_type() &&
          runtime.as_native_type().kind ==
              sputnik::runtime::RuntimeNativeTypeKind::System) ||
         (runtime.is_io_value() &&
          sputnik::runtime::system_resource(runtime.as_io_value()));
}

static NativeValue
native_system_to_value(const sputnik::runtime::SystemValue &value) {
  using Kind = sputnik::runtime::SystemValue::Kind;
  switch (value.kind) {
  case Kind::Null:
    return NativeValue::nullv();
  case Kind::Bool:
    return NativeValue::boolean(value.boolean);
  case Kind::Int:
    return NativeValue::integer(value.integer);
  case Kind::Float:
    return NativeValue::floating(value.floating);
  case Kind::String:
    return NativeValue::heap_string(value.text);
  case Kind::Bytes:
    return NativeValue::bytes(value.text);
  case Kind::Object:
    return NativeValue::runtime_handle(
        sputnik::runtime::Value::io_value(value.object));
  case Kind::Opaque:
    return *std::static_pointer_cast<NativeValue>(value.opaque);
  case Kind::List: {
    std::vector<NativeValue> items;
    for (const auto &item : value.items)
      items.push_back(native_system_to_value(item));
    return NativeValue::list(std::move(items));
  }
  default:
    throw sputnik::runtime::SystemError("TypeError",
                                      "unsupported process result");
  }
}

static sputnik::runtime::SystemValue
native_system_from_value(const NativeValue &value) {
  using S = sputnik::runtime::SystemValue;
  using Kind = S::Kind;
  if (native_value_is_string(value))
    return S::str(native_string_text(value));
  switch (value.tag) {
  case NativeValue::Tag::Null:
    return {};
  case NativeValue::Tag::Bool:
    return S::boolean_value(value.scalar_value != 0);
  case NativeValue::Tag::Integer:
    return S::number(value.scalar_value);
  case NativeValue::Tag::Float: {
    S v;
    v.kind = Kind::Float;
    v.floating = value.float_value;
    return v;
  }
  case NativeValue::Tag::Bytes:
    return S::str(as_bytes(value).bytes, true);
  case NativeValue::Tag::Symbol:
    return S::str(std::string(kModuleSymbols[value.scalar_value]));
  case NativeValue::Tag::RuntimeHandle: {
    const auto &runtime = as_native_runtime_handle(value)->value;
    if (runtime.is_native_type() &&
        runtime.as_native_type().kind ==
            sputnik::runtime::RuntimeNativeTypeKind::System) {
      S v;
      v.kind = Kind::Module;
      return v;
    }
    if (runtime.is_io_value() &&
        sputnik::runtime::system_resource(runtime.as_io_value()))
      return S::resource(runtime.as_io_value());
    break;
  }
  case NativeValue::Tag::List:
  case NativeValue::Tag::Tuple: {
    const auto &items = value.tag == NativeValue::Tag::List
                            ? as_list(value).items
                            : as_tuple(value).items;
    std::vector<S> result;
    for (const auto &item : items)
      result.push_back(native_system_from_value(item));
    return S::list(std::move(result));
  }
  case NativeValue::Tag::Map: {
    S result;
    result.kind = Kind::Map;
    for (const auto &entry : as_map(value).entries) {
      auto key = native_system_from_value(entry.first);
      if (key.kind != Kind::String)
        throw sputnik::runtime::SystemError("TypeError",
                                          "env keys must be strings");
      result.entries[key.text] = native_system_from_value(entry.second);
    }
    return result;
  }
  case NativeValue::Tag::Closure: {
    S result;
    result.kind = Kind::Callback;
    const auto strand = sputnik::runtime::current_runtime_strand_id();
    const auto task_id = sputnik::runtime::tls_runtime_task_id;
    const auto cancel = sputnik::runtime::tls_runtime_task_cancel_flag;
    const auto cancel_owner = sputnik::runtime::current_runtime_task_cancel_owner();
    const auto run = sputnik::runtime::current_runtime_run_cancellation();
    const bool cleanup = sputnik::runtime::runtime_run_cancel_requested() &&
                         !sputnik::runtime::runtime_run_cancel_checkpoint_requested();
    const auto sync_owner = sputnik::runtime::tls_runtime_sync_owner_id;
    const auto context = sputnik::runtime::tls_runtime_task_context;
    const auto scheduler = sputnik::runtime::tls_runtime_scheduler_identity;
    const auto stdout_writer = sputnik::runtime::current_runtime_stdout(),
               stderr_writer = sputnik::runtime::current_runtime_stderr();
    result.callback = [value, strand, task_id, cancel, cancel_owner, run, cleanup, sync_owner, context,
                       scheduler, stdout_writer,
                       stderr_writer](const std::vector<S> &args) {
      sputnik::runtime::RuntimeStrandScope strand_scope(strand);
      const auto activity = run ? run->register_task() : nullptr;
      sputnik::runtime::RuntimeRunCancellationScope run_scope(run);
      sputnik::runtime::RuntimeRunCancellationMask cleanup_mask(cleanup);
      sputnik::runtime::RuntimeTaskScope task_scope(
          task_id,
          sputnik::runtime::system_callback_cancel_flag()
              ? sputnik::runtime::system_callback_cancel_flag()
              : cancel,
          sync_owner, context, scheduler, cancel_owner);
      sputnik::runtime::RuntimeOutputScope output_scope(stdout_writer,
                                                      stderr_writer);
      NativeCycleScope cycle_scope;
      std::vector<NativeValue> values;
      for (const auto &arg : args)
        values.push_back(native_system_to_value(arg));
      S result;
      result.kind = Kind::Opaque;
      result.opaque = std::make_shared<NativeValue>(
          sputnik_native_call_closure(value, values));
      return result;
    };
    return result;
  }
  default:
    break;
  }
  throw sputnik::runtime::SystemError("TypeError",
                                    "unsupported process argument");
}

static NativeValue native_system_send(const NativeValue &receiver,
                                      const std::string &selector,
                                      const NativeArgsView &args,
                                      const NativeKeywordArgsView &kwargs,
                                      NativeValue block) {
  try {
    sputnik::runtime::SystemCall call;
    call.receiver = native_system_from_value(receiver);
    call.selector = selector;
    for (const auto &arg : args)
      call.args.push_back(native_system_from_value(arg));
    for (const auto &entry : kwargs)
      call.kwargs[std::string(entry.name)] =
          native_system_from_value(entry.value);
    call.block = native_system_from_value(block);
    call.authorize = [](const std::string &capability,
                        const std::string &target) {
      if (!sputnik::capability::capability_set_allows(
              embedded_capability_grants(), capability, target))
        throw sputnik::runtime::SystemError(
            "CapabilityError",
            "capability is not granted: " + capability + "=" + target);
      native_commit_effect();
    };
    const auto cancel = sputnik::runtime::runtime_wait_cancel_flag();
    call.cancelled = [cancel, owner = sputnik::runtime::current_runtime_task_cancel_owner(),
                      run = sputnik::runtime::current_runtime_run_cancellation()] {
      (void)owner; (void)run;
      return cancel && cancel->load();
    };
    sputnik::runtime::SystemValue result;
    if (sputnik::runtime::system_blocking_selector(selector)) {
      native_require_suspendable("system");
      native_commit_effect();
      sputnik::runtime::SystemCallbackSuspension callback_suspension;
      NativeCycleSuspension suspension;
      result = sputnik::runtime::system_dispatch(call);
    } else
      result = sputnik::runtime::system_dispatch(call);
    return native_system_to_value(result);
  } catch (const sputnik::runtime::SystemError &error) {
    throw NativeRaised{native_named_error(error.name, error.what())};
  }
}
