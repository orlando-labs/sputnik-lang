// Included in Vm: keep process-specific adaptation out of the main dispatcher.
Value system_to_value(const SystemValue &value) {
  using Kind = SystemValue::Kind;
  switch (value.kind) {
  case Kind::Null:
    return Value::null();
  case Kind::Bool:
    return Value::boolean(value.boolean);
  case Kind::Int:
    return Value::integer(value.integer);
  case Kind::Float:
    return Value::floating(value.floating);
  case Kind::String:
    return string_value_from_text(value.text);
  case Kind::Bytes:
    return Value::io_value(std::make_shared<RuntimeBytes>(value.text));
  case Kind::Object:
    return Value::io_value(value.object);
  case Kind::Opaque:
    return *std::static_pointer_cast<Value>(value.opaque);
  case Kind::Module:
    return Value::native_type(RuntimeNativeTypeKind::System);
  case Kind::List: {
    std::vector<Value> items;
    for (const auto &item : value.items)
      items.push_back(system_to_value(item));
    return make_list_value(std::move(items));
  }
  default:
    throw SystemError("TypeError", "unsupported process result");
  }
}

SystemValue system_from_value(const Frame &frame, const Value &value,
                              bool detached) {
  using Kind = SystemValue::Kind;
  if (value.is_null())
    return {};
  if (value.is_bool())
    return SystemValue::boolean_value(value.as_bool());
  if (value.is_integer())
    return SystemValue::number(value.as_integer());
  if (value.is_float()) {
    SystemValue v;
    v.kind = Kind::Float;
    v.floating = value.as_float();
    return v;
  }
  if (value.is_heap_string())
    return SystemValue::str(value.as_heap_string()->text);
  if (auto text = stdlib_text_of(value))
    return SystemValue::str(*text);
  if (value.is_native_type() &&
      value.as_native_type().kind == RuntimeNativeTypeKind::System) {
    SystemValue v;
    v.kind = Kind::Module;
    return v;
  }
  if (value.is_io_value() && system_resource(value.as_io_value()))
    return SystemValue::resource(value.as_io_value());
  if (value.is_io_value()) {
    if (auto bytes = stdlib_bytes_of(&frame, value))
      return SystemValue::str(*bytes, true);
    throw NativeBlockUnwind();
  }
  if (value.is_list() || value.is_tuple()) {
    std::vector<Value> items;
    if (!stdlib_sequence_items(&frame, value, &items))
      throw NativeBlockUnwind();
    std::vector<SystemValue> converted;
    for (const auto &item : items)
      converted.push_back(system_from_value(frame, item, detached));
    return SystemValue::list(std::move(converted));
  }
  if (value.is_map()) {
    std::vector<std::pair<std::string, Value>> entries;
    if (!stdlib_string_keyed_entries(&frame, value, &entries))
      throw NativeBlockUnwind();
    SystemValue converted;
    converted.kind = Kind::Map;
    for (const auto &entry : entries)
      converted.entries[entry.first] =
          system_from_value(frame, entry.second, detached);
    return converted;
  }
  if (value.is_closure()) {
    auto invoker =
        detached
            ? make_detached_native_block_invoker(frame, value,
                                                 "system callback")
            : make_scoped_native_block_invoker(frame, value, "system callback");
    if (!invoker)
      throw NativeBlockUnwind();
    const auto strand = current_runtime_strand_id();
    const auto task_id = tls_runtime_task_id,
               sync_owner = tls_runtime_sync_owner_id;
    const auto cancel = tls_runtime_task_cancel_flag;
    const auto cancel_owner = current_runtime_task_cancel_owner();
    const auto run = current_runtime_run_cancellation();
    const bool cleanup = runtime_run_cancel_requested() &&
                         !runtime_run_cancel_checkpoint_requested();
    const auto context = tls_runtime_task_context;
    const auto scheduler = tls_runtime_scheduler_identity;
    const auto stdout_writer = current_runtime_stdout(),
               stderr_writer = current_runtime_stderr();
    SystemValue converted;
    converted.kind = Kind::Callback;
    converted.callback =
        [this, scoped = !detached, fn = std::move(*invoker), strand, task_id,
         sync_owner, cancel, cancel_owner, run, cleanup, context, scheduler, stdout_writer,
         stderr_writer](const std::vector<SystemValue> &args) mutable {
          RuntimeStrandScope strand_scope(strand);
          RuntimeRunCancellationScope run_scope(run);
          RuntimeRunCancellationMask cleanup_mask(cleanup);
          RuntimeTaskScope task_scope(task_id,
                                      system_callback_cancel_flag()
                                          ? system_callback_cancel_flag()
                                          : cancel,
                                      sync_owner, context, scheduler, cancel_owner);
          RuntimeOutputScope output_scope(stdout_writer, stderr_writer);
          IoParkGuard no_park(false);
          // The outer operation has already parked this VM's strand. A
          // scoped block preserves the live VM stack on this host thread;
          // nested waits must not replace the pending outer continuation.
          struct ScopedParkMode {
            bool *mode;
            bool previous;
            explicit ScopedParkMode(bool *mode)
                : mode(mode), previous(mode && *mode) {
              if (mode)
                *mode = false;
            }
            ~ScopedParkMode() {
              if (mode)
                *mode = previous;
            }
          } park_mode(scoped ? &parkable_ : nullptr);
          std::vector<Value> values;
          for (const auto &arg : args)
            values.push_back(system_to_value(arg));
          SystemValue result;
          result.kind = SystemValue::Kind::Opaque;
          result.opaque = std::make_shared<Value>(fn(values));
          return result;
        };
    return converted;
  }
  throw SystemError("TypeError", "unsupported process argument");
}

SendStatus
apply_system_send(const Frame &source_frame, const Value &receiver,
                  const std::string &selector, const std::vector<Value> &args,
                  const Value &block,
                  const std::vector<std::pair<std::uint32_t, Value>> &kwargs,
                  Value *out) {
  Frame &frame = const_cast<Frame &>(source_frame);
  const bool blocking = system_blocking_selector(selector);
  if (blocking && block_suspension_in_property_arm(frame, "system"))
    return SendStatus::Faulted;
  if (frame.pending_native_extension_call &&
      frame.pending_native_extension_call->logical == "stdlib.system") {
    const auto pending = frame.pending_native_extension_call;
    std::lock_guard<std::mutex> lock(pending->mutex);
    if (!pending->ready) {
      ParkRequest park;
      park.kind = ParkRequest::Kind::Ffi;
      park_request_ = std::move(park);
      return SendStatus::Matched;
    }
    frame.pending_native_extension_call.reset();
    blocking_ffi_call_.reset();
    if (!pending->exception_error_name.empty()) {
      raise_runtime_error(frame, pending->exception_error_name,
                          pending->exception_message);
      return SendStatus::Faulted;
    }
    if (!pending->ok)
      return SendStatus::Faulted;
    *out = pending->value;
    return SendStatus::Matched;
  }
  try {
    // Capture handlers run independently. Ordinary each_chunk blocks run on
    // their calling VM, preserving local mutation and ordinary block exits.
    const bool detached = selector == "capture" || selector == "output" ||
                          selector == "communicate";
    SystemCall call;
    call.receiver = system_from_value(frame, receiver, false);
    call.selector = selector;
    for (const auto &arg : args)
      call.args.push_back(system_from_value(frame, arg, false));
    for (const auto &entry : kwargs)
      call.kwargs[selector_text_from_symbol(entry.first).value_or("")] =
          system_from_value(frame, entry.second, detached);
    call.block = system_from_value(frame, block, false);
    call.authorize = [this, &frame](const std::string &capability,
                                    const std::string &target) {
      if (!check_io_policy(frame, "process", capability, target)) {
        // The worker reports errors to the suspended caller; unwinding its
        // rescue stack here would race the pending-send continuation.
        if (fault_) {
          SystemError error(fault_->error_name, fault_->message);
          fault_.reset();
          throw error;
        }
        throw NativeBlockUnwind();
      }
    };
    const auto cancel = runtime_wait_cancel_flag();
    // A subprocess monitor may retain this predicate on another host thread.
    // Keep both possible owners of the borrowed cancellation flag alive.
    call.cancelled = [cancel, owner = current_runtime_task_cancel_owner(),
                      run = current_runtime_run_cancellation()] {
      (void)owner; (void)run;
      return cancel && cancel->load();
    };
    if (parkable_ && blocking) {
      auto pending = std::make_shared<PendingNativeExtensionCall>();
      pending->logical = "stdlib.system";
      pending->call_pc = frame.pc;
      frame.pending_native_extension_call = pending;
      blocking_ffi_call_ = pending;
      const auto strand = current_runtime_strand_id();
      auto task = task_module_;
      ParkRequest park;
      park.kind = ParkRequest::Kind::Ffi;
      park.ffi_start = [pending, call = std::move(call), task,
                        strand](std::shared_ptr<Vm> vm) mutable {
        // A bounded FFI pool can deadlock when all its slots await input
        // from another subprocess. Preserve each operation's host stack
        // independently while its Sputnik strand is parked.
        std::thread([pending, call = std::move(call), task, strand,
                     vm = std::move(vm)]() mutable {
          RuntimeStrandScope strand_scope(strand);
          try {
            pending->value = vm->system_to_value(system_dispatch(call));
            pending->ok = true;
          } catch (const SystemError &error) {
            pending->exception_error_name = error.name;
            pending->exception_message = error.what();
          } catch (const RuntimeTaskFailure &error) {
            pending->exception_error_name = error.error_name();
            pending->exception_message = error.message();
          } catch (const RuntimeTaskCancelled &) {
            pending->exception_error_name = "CancelledError";
            pending->exception_message = "process operation cancelled";
          } catch (const NativeBlockUnwind &) {
          } catch (const std::exception &error) {
            pending->exception_error_name = "ProcessError";
            pending->exception_message = error.what();
          }
          {
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->ready = true;
          }
          task->scheduler().wake_strand(strand);
        }).detach();
      };
      park_request_ = std::move(park);
      return SendStatus::Matched;
    }
    SystemValue result;
    {
      SystemCallbackSuspension suspension(blocking);
      result = system_dispatch(call);
    }
    *out = system_to_value(result);
    return SendStatus::Matched;
  } catch (const SystemError &error) {
    raise_runtime_error(frame, error.name, error.what());
  } catch (const RuntimeTaskFailure &error) {
    raise_runtime_error(frame, error.error_name(), error.message());
  } catch (const NativeBlockUnwind &) {
  }
  return SendStatus::Faulted;
}
