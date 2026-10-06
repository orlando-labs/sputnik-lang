// Native closures have a C++ call stack. Keep that stack on a dedicated host
// thread while the Sputnik strand parks; a single scheduler worker can continue
// running other tasks, including producers a process callback is awaiting.
static sputnik::runtime::RuntimeTaskModule::TaskFunction
native_system_task_driver(
    sputnik::runtime::RuntimeTaskModule::TaskFunction function) {
  struct Completion {
    std::mutex mutex;
    bool started = false, done = false;
    std::exception_ptr failure;
  };
  auto state = std::make_shared<Completion>();
  return [state,
          function = std::move(function)]() mutable -> sputnik::runtime::Value {
    auto &runtime = native_task_runtime();
    std::unique_lock<std::mutex> lock(state->mutex);
    if (state->done) {
      auto failure = state->failure;
      lock.unlock();
      if (failure)
        std::rethrow_exception(failure);
      return sputnik::runtime::Value::null();
    }
    // The host stack still owns the native operation. Stop must not repeatedly
    // resume this driver before that stack returns (or Force Stop kills it).
    if (!runtime.scheduler().park_current(std::nullopt, false)) {
      lock.unlock();
      return function();
    }
    sputnik::runtime::runtime_mark_task_parked();
    if (state->started)
      return sputnik::runtime::Value::null();
    state->started = true;
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
    try {
      // Publishing `done` can resume/retire the scheduler closure before this
      // thread releases its own captures. Account for that last host-stack
      // lifetime as well, with the executable destroyed before membership.
      struct HostWork {
        std::shared_ptr<sputnik::runtime::RuntimeRunState::Task> activity;
        sputnik::runtime::RuntimeTaskModule::TaskFunction function;
      };
      auto work = std::make_shared<HostWork>(HostWork{
          run ? run->register_task() : nullptr, function});
      std::thread([state, work, strand, task_id, cancel, cancel_owner, run, cleanup, sync_owner,
                   context, scheduler, stdout_writer, stderr_writer]() mutable {
        sputnik::runtime::RuntimeStrandScope strand_scope(strand);
        sputnik::runtime::RuntimeRunCancellationScope run_scope(run);
        sputnik::runtime::RuntimeRunCancellationMask cleanup_mask(cleanup);
        sputnik::runtime::RuntimeTaskScope task_scope(task_id, cancel, sync_owner,
                                                    context, scheduler, cancel_owner);
        sputnik::runtime::RuntimeOutputScope output_scope(stdout_writer,
                                                        stderr_writer);
        std::exception_ptr failure;
        try {
          work->function();
        } catch (...) {
          failure = std::current_exception();
        }
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          state->failure = failure;
          state->done = true;
        }
        native_task_runtime().scheduler().wake_strand(strand);
      }).detach();
    } catch (...) {
      state->failure = std::current_exception();
      state->done = true;
      runtime.scheduler().wake_strand(strand);
    }
    return sputnik::runtime::Value::null();
  };
}
