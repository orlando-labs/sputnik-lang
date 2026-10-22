// Direct FS / Unix IO adapters included by the generated C++ backend.
// These share OS resources, cancellation and the reactor with the VM and never
// construct a RuntimeWorld or execute a bytecode body.
static void native_host_io_error(const std::string &name, const std::string &message) {
  throw NativeRaised{native_named_error(name, message)};
}
static void native_host_io_check(const sputnik::runtime::RuntimeIoStatus &status) {
  if (!status.ok && !status.would_block)
    native_host_io_error(status.error_name.empty() ? "IOError" : status.error_name, status.message);
}
static void native_host_io_authorize(const std::string &capability, const std::string &path) {
  if (!sputnik::capability::capability_set_allows(embedded_capability_grants(), capability, path))
    native_host_io_error("CapabilityError", "capability is not granted: " + capability + "=" + path);
}
static const NativeValue *native_host_io_kw(const NativeKeywordArgsView &kwargs, std::string_view name) {
  for (const auto &entry : kwargs) if (entry.name == name) return &entry.value;
  return nullptr;
}
static void native_host_io_keywords(const NativeKeywordArgsView &kwargs,
                                    std::initializer_list<const char *> names) {
  for (const auto &entry : kwargs) {
    bool valid = false;
    for (const char *name : names) if (entry.name == name) valid = true;
    if (!valid) native_host_io_error("ArgumentError", "unknown keyword: " + std::string(entry.name));
  }
}
static bool native_host_io_bool(const NativeKeywordArgsView &kwargs, const char *name, bool fallback) {
  const auto value = native_host_io_kw(kwargs, name);
  if (!value) return fallback;
  if (value->tag != NativeValue::Tag::Bool) native_host_io_error("TypeError", std::string(name) + " must be Bool");
  return value->scalar_value != 0;
}
static std::optional<std::size_t> native_host_io_limit(const NativeKeywordArgsView &kwargs, const char *name = "limit") {
  const auto value = native_host_io_kw(kwargs, name);
  if (!value || value->tag == NativeValue::Tag::Null) return {};
  if (value->tag != NativeValue::Tag::Integer || value->scalar_value < 0)
    native_host_io_error("ArgumentError", std::string(name) + " must be a nonnegative Int");
  return static_cast<std::size_t>(value->scalar_value);
}
static std::chrono::milliseconds native_host_io_timeout(const NativeKeywordArgsView &kwargs) {
  const auto value = native_host_io_kw(kwargs, "timeout");
  if (!value || value->tag == NativeValue::Tag::Null) return std::chrono::milliseconds::max();
  // IO durations use seconds for both Int and Float, matching existing socket IO.
  double seconds;
  if (value->tag == NativeValue::Tag::Integer) seconds = value->scalar_value;
  else if (value->tag == NativeValue::Tag::Float) seconds = value->float_value;
  else { native_host_io_error("TypeError", "timeout must be numeric or null"); return {}; }
  if (!std::isfinite(seconds) || seconds < 0) native_host_io_error("ArgumentError", "invalid timeout");
  if (seconds >= static_cast<double>(INT64_MAX) / 1000) return std::chrono::milliseconds::max();
  return std::chrono::milliseconds(static_cast<std::int64_t>(seconds * 1000));
}
static std::string native_host_io_path(const NativeValue &value) {
  if (!native_value_is_string(value) && value.tag != NativeValue::Tag::FsPath)
    native_host_io_error("TypeError", "path must be Str or fs.Path");
  const auto path = native_fs_path_text(value);
  if (path.find('\0') != std::string::npos) native_host_io_error("ArgumentError", "path contains NUL");
  return path;
}
static NativeValue native_host_io_scoped(NativeValue resource, NativeValue block) {
  if (block.tag == NativeValue::Tag::Null) return resource;
  auto opened = std::dynamic_pointer_cast<sputnik::runtime::RuntimeIoResource>(
      as_native_runtime_handle(resource)->value.as_io_value());
  if (block.tag != NativeValue::Tag::Closure) {
    (void)opened->close(); native_host_io_error("TypeError", "IO block must be a closure");
  }
  try {
    auto result = sputnik_native_call_closure(block, {resource});
    native_host_io_check(opened->close()); return result;
  } catch (...) { (void)opened->close(); throw; }
}
static NativeValue native_fs_direct_send(const NativeValue &receiver, const std::string &selector,
    const NativeArgsView &args, const NativeKeywordArgsView &kwargs, NativeValue block) {
  using namespace sputnik::runtime;
  if (receiver.tag != NativeValue::Tag::FsModule) throw NativeBailout();
  if (selector == "File" && args.empty() && kwargs.empty() && block.tag == NativeValue::Tag::Null)
    return NativeValue::runtime_handle(Value::native_type(RuntimeNativeTypeKind::FsFile));
  if (selector == "Path") {
    native_host_io_keywords(kwargs, {});
    if (block.tag != NativeValue::Tag::Null || args.size() != 1) native_host_io_error("ArgumentError", "Path expects one argument");
    return native_fs_path_new(receiver, args[0], true);
  }
  if (selector == "sync" && (args.size() != 1 || !kwargs.empty() || block.tag != NativeValue::Tag::Null))
    native_host_io_error("TypeError", "fs.sync expects one path and no keywords or block");
  const bool two = selector == "atomic_write_text" || selector == "atomic_write_bytes" ||
    selector == "write_text" || selector == "write_bytes" || selector == "rename" || selector == "copy";
  if (args.size() != (two ? 2U : 1U)) native_host_io_error("ArgumentError", "wrong fs argument count");
  if (block.tag != NativeValue::Tag::Null && selector != "lock") native_host_io_error("ArgumentError", "fs operation does not accept a block");
  const auto path = native_host_io_path(args[0]);
  const RuntimePath target(path);
  const bool metadata = selector == "entries" || selector == "metadata" || selector == "exists?" || selector == "file?" || selector == "dir?";
  const bool read = selector == "read_text" || selector == "read_bytes";
  native_host_io_authorize(metadata ? "fs.metadata" : read || selector == "copy" ? "fs.read" : "fs.write", path);
  native_commit_effect(); native_require_suspendable(selector.c_str());
  if (read) {
    native_host_io_keywords(kwargs, {"limit", "encoding"});
    if (auto encoding = native_host_io_kw(kwargs, "encoding")) {
      if (!native_value_is_string(*encoding) || native_string_text(*encoding) != "utf8") native_host_io_error("ArgumentError", "read_text requires utf8 encoding");
    }
    auto limit = native_host_io_limit(kwargs);
    std::shared_ptr<RuntimeBytes> bytes;
    RuntimeIoStatus status;
    { NativeCycleSuspension suspension; status = runtime_fs_read_bytes(target, limit, &bytes); }
    native_host_io_check(status);
    if (selector == "read_bytes") return NativeValue::bytes(bytes->string());
    auto text = bytes->to_string(); native_host_io_check(text); return NativeValue::heap_string(std::move(text.bytes));
  }
  if (metadata) {
    native_host_io_keywords(kwargs, {});
    if (selector == "entries") {
      std::vector<NativeValue> values;
      std::vector<std::string> paths;
      std::error_code error;
      { NativeCycleSuspension suspension;
        std::filesystem::directory_iterator it(path, error);
        for (auto end = std::filesystem::directory_iterator{}; !error && it != end; it.increment(error)) {
          paths.push_back(it->path().string());
        }
      }
      if (error) native_host_io_error("IOError", "fs.entries: " + error.message());
      std::sort(paths.begin(), paths.end());
      for (auto &entry : paths) values.push_back(NativeValue::fs_path(NativeFsPath{std::move(entry)}));
      return NativeValue::list(std::move(values));
    }
    if (selector == "metadata") {
      auto info = std::make_shared<RuntimeMetadata>();
      RuntimeIoStatus status;
      { NativeCycleSuspension suspension; status = runtime_fs_metadata(target, info.get()); }
      native_host_io_check(status);
      return NativeValue::runtime_handle(Value::io_value(info));
    }
    bool found = false;
    RuntimeIoStatus status;
    { NativeCycleSuspension suspension;
      status = selector == "exists?" ? runtime_fs_exists(target, &found)
        : selector == "file?" ? runtime_fs_file(target, &found) : runtime_fs_dir(target, &found); }
    native_host_io_check(status);
    return NativeValue::boolean(found);
  }
  if (selector == "lock") {
    native_host_io_keywords(kwargs, {"exclusive", "nonblock"});
    const auto exclusive = native_host_io_bool(kwargs, "exclusive", true);
    const auto nonblock = native_host_io_bool(kwargs, "nonblock", true);
    RuntimeFileLockResult result;
    { NativeCycleSuspension suspension; result = RuntimeFileLock::acquire(target, exclusive, nonblock); }
    native_host_io_check(result);
    return native_host_io_scoped(NativeValue::runtime_handle(Value::io_value(result.lock)), block);
  }
  RuntimeIoStatus status;
  if (selector == "atomic_write_text" || selector == "atomic_write_bytes" || selector == "write_text" || selector == "write_bytes") {
    const bool text = selector == "atomic_write_text" || selector == "write_text";
    if (text ? !native_value_is_string(args[1]) : args[1].tag != NativeValue::Tag::Bytes)
      native_host_io_error("TypeError", text ? "write expects Str" : "write expects Bytes");
    const auto data = text ? native_string_text(args[1]) : as_bytes(args[1]).bytes;
    if (selector.compare(0, 7, "atomic_") == 0) {
      native_host_io_keywords(kwargs, {"durable"});
      const auto durable = native_host_io_bool(kwargs, "durable", true);
      NativeCycleSuspension suspension; status = runtime_fs_atomic_write(target, data, durable);
    } else {
      native_host_io_keywords(kwargs, {"create", "truncate"});
      const auto create = native_host_io_bool(kwargs, "create", true), truncate = native_host_io_bool(kwargs, "truncate", true);
      NativeCycleSuspension suspension; status = runtime_fs_write_bytes(target, data, create, truncate);
    }
  } else {
    native_host_io_keywords(kwargs, {});
    if (selector == "rename" || selector == "copy") {
      const auto destination = native_host_io_path(args[1]);
      native_host_io_authorize("fs.write", destination);
      if (selector == "copy") native_host_io_authorize("fs.read", path);
      std::size_t count = 0;
      { NativeCycleSuspension suspension; status = selector == "rename" ? runtime_fs_rename(target, RuntimePath(destination)) : runtime_fs_copy(target, RuntimePath(destination), &count); }
      native_host_io_check(status);
      return selector == "copy" ? NativeValue::integer(count) : NativeValue::nullv();
    }
    if (selector != "mkdir" && selector != "mkdir_p" && selector != "remove" && selector != "sync")
      native_host_io_error("NoMethodError", "fs has no method " + selector);
    { NativeCycleSuspension suspension;
      if (selector == "mkdir") status = runtime_fs_mkdir(target);
      else if (selector == "mkdir_p") status = runtime_fs_mkdir_p(target);
      else if (selector == "sync") status = runtime_fs_sync(target);
      else status = runtime_fs_remove(target); }
  }
  native_host_io_check(status); return NativeValue::nullv();
}

static bool native_host_io_receiver(const NativeValue &receiver) {
  using namespace sputnik::runtime;
  if (receiver.tag != NativeValue::Tag::RuntimeHandle) return false;
  const auto &value = as_native_runtime_handle(receiver)->value;
  if (value.is_native_type()) return value.as_native_type().kind == RuntimeNativeTypeKind::NetUnix || value.as_native_type().kind == RuntimeNativeTypeKind::FsFile;
  if (!value.is_io_value()) return false;
  const auto name = std::string_view(value.as_io_value()->type_name());
  return name == "net.UnixStream" || name == "net.UnixListener" || name == "fs.Lock" || name == "fs.Metadata" || name == "fs.File";
}
static NativeValue native_host_io_send(const NativeValue &receiver, const std::string &selector,
    const NativeArgsView &args, const NativeKeywordArgsView &kwargs, NativeValue block) {
  using namespace sputnik::runtime;
  const auto &value = as_native_runtime_handle(receiver)->value;
  if (value.is_native_type() && value.as_native_type().kind == RuntimeNativeTypeKind::FsFile) {
    if (selector != "open" || args.empty() || args.size() > 2)
      native_host_io_error("ArgumentError", "File.open expects path and optional mode");
    native_host_io_keywords(kwargs, {"create", "truncate", "append", "exclusive", "permissions", "isolation"});
    const auto path = native_host_io_path(args[0]);
    auto mode = RuntimeFileMode::Read;
    if (args.size() == 2) {
      auto name = args[1].tag == NativeValue::Tag::Symbol ? native_symbol_text(args[1].scalar_value) : native_host_io_path(args[1]);
      auto parsed = runtime_file_mode_from_name(name);
      if (!parsed) native_host_io_error("ArgumentError", "unsupported file mode");
      mode = *parsed;
    }
    RuntimeFileOpenOptions options;
    options.create = native_host_io_bool(kwargs, "create", false);
    options.truncate = native_host_io_bool(kwargs, "truncate", false);
    options.append = native_host_io_bool(kwargs, "append", false);
    options.exclusive = native_host_io_bool(kwargs, "exclusive", false);
    if (auto permissions = native_host_io_limit(kwargs, "permissions")) {
      if (*permissions > 07777) native_host_io_error("ArgumentError", "invalid file permissions");
      options.permissions = *permissions;
    }
    auto isolation = RuntimeIsolationMode::Checked;
    if (auto option = native_host_io_kw(kwargs, "isolation")) {
      auto name = option->tag == NativeValue::Tag::Symbol ? native_symbol_text(option->scalar_value) : native_host_io_path(*option);
      auto parsed = runtime_isolation_mode_from_name(name);
      if (!parsed) native_host_io_error("ArgumentError", "invalid isolation");
      isolation = *parsed;
    }
    if (mode == RuntimeFileMode::Read || mode == RuntimeFileMode::ReadWrite) native_host_io_authorize("fs.read", path);
    if (mode != RuntimeFileMode::Read || options.create || options.truncate || options.append) native_host_io_authorize("fs.write", path);
    if (options.exclusive && !options.create) native_host_io_error("ArgumentError", "exclusive requires create");
    native_commit_effect(); native_require_suspendable("fs.open");
    RuntimeFileOpenResult opened;
    { NativeCycleSuspension suspension; opened = RuntimeFile::open(RuntimePath(path), mode, options, isolation); }
    native_host_io_check(opened);
    return native_host_io_scoped(NativeValue::runtime_handle(Value::io_value(opened.file)), block);
  }
  if (value.is_native_type()) {
    const bool listen = selector == "listen";
    if ((!listen && selector != "connect") || args.size() != 1) native_host_io_error("ArgumentError", "invalid Unix socket operation");
    if (!native_value_is_string(args[0])) native_host_io_error("TypeError", "Unix socket expects a path Str");
    native_host_io_keywords(kwargs, listen ? std::initializer_list<const char *>{"isolation", "backlog"} : std::initializer_list<const char *>{"isolation", "timeout"});
    auto path = native_host_io_path(args[0]);
    native_host_io_authorize(listen ? "net.listen" : "net.connect", "unix:" + path);
    if (listen) native_host_io_authorize("fs.write", path);
    RuntimeIsolationMode isolation = RuntimeIsolationMode::Checked;
    if (auto option = native_host_io_kw(kwargs, "isolation")) {
      auto text = option->tag == NativeValue::Tag::Symbol ? native_symbol_text(option->scalar_value) : native_host_io_path(*option);
      auto parsed = runtime_isolation_mode_from_name(text);
      if (!parsed) native_host_io_error("ArgumentError", "invalid isolation"); isolation = *parsed;
    }
    std::shared_ptr<RuntimeIoResource> resource;
    native_commit_effect(); native_require_suspendable(selector.c_str());
    if (listen) {
      const auto backlog = native_host_io_limit(kwargs, "backlog").value_or(128);
      if (!backlog || backlog > 65536) native_host_io_error("ArgumentError", "invalid backlog");
      RuntimeTcpListenResult result;
      { NativeCycleSuspension suspension; result = RuntimeTcpListener::listen_unix(path, backlog, isolation); }
      native_host_io_check(result); resource = result.listener;
    } else {
      auto timeout = native_host_io_timeout(kwargs);
      RuntimeTcpConnectResult result;
      { NativeCycleSuspension suspension; result = RuntimeTcpStream::connect_unix(path, timeout, isolation); }
      native_host_io_check(result); resource = result.stream;
    }
    return native_host_io_scoped(NativeValue::runtime_handle(Value::io_value(resource)), block);
  }
  auto io = value.as_io_value();
  auto metadata = std::dynamic_pointer_cast<RuntimeMetadata>(io);
  if (metadata) {
    native_host_io_keywords(kwargs, {});
    if (!args.empty() || block.tag != NativeValue::Tag::Null) native_host_io_error("ArgumentError", "metadata accepts no arguments");
    if (selector == "path") return NativeValue::fs_path(NativeFsPath{metadata->path.string()});
    if (selector == "size") return NativeValue::integer(metadata->size);
    if (selector == "file?") return NativeValue::boolean(metadata->file);
    if (selector == "dir?") return NativeValue::boolean(metadata->directory);
    if (selector == "symlink?") return NativeValue::boolean(metadata->symlink);
    native_host_io_error("NoMethodError", "unknown metadata selector");
  }
  auto resource = std::dynamic_pointer_cast<RuntimeIoResource>(io);
  if (block.tag != NativeValue::Tag::Null) native_host_io_error("ArgumentError", "resource does not accept a block");
  if (selector == "close!" || selector == "closed?") {
    native_host_io_keywords(kwargs, {});
    if (!args.empty()) native_host_io_error("ArgumentError", "close accepts no arguments");
    if (selector == "closed?") return NativeValue::boolean(resource->closed());
    native_host_io_check(resource->close()); return NativeValue::nullv();
  }
  auto listener = std::dynamic_pointer_cast<RuntimeTcpListener>(io);
  auto stream = std::dynamic_pointer_cast<RuntimeTcpStream>(io);
  native_host_io_check(resource->access_status());
  if (auto file = std::dynamic_pointer_cast<RuntimeFile>(io)) {
    if (selector == "path" || selector == "mode") {
      native_host_io_keywords(kwargs, {});
      if (!args.empty()) native_host_io_error("ArgumentError", "file metadata takes no arguments");
      if (selector == "path") return NativeValue::fs_path(NativeFsPath{file->path().string()});
      const char* mode = file->mode() == RuntimeFileMode::Read ? "read" : file->mode() == RuntimeFileMode::Write ? "write" : file->mode() == RuntimeFileMode::Append ? "append" : "read_write";
      return NativeValue::symbol_ref(native_intern_symbol(mode));
    }
    auto timeout = native_host_io_timeout(kwargs);
    RuntimeIoStatus status;
    if (selector == "write_all!" || selector == "write!") {
      native_host_io_keywords(kwargs, {"timeout"});
      if (args.size() != 1) native_host_io_error("ArgumentError", "file write expects bytes");
      if (!native_value_is_string(args[0]) && args[0].tag != NativeValue::Tag::Bytes)
        native_host_io_error("TypeError", "file write expects Bytes or Str");
      const auto bytes = native_value_is_string(args[0])
        ? native_string_text(args[0]) : as_bytes(args[0]).bytes;
      { NativeCycleSuspension suspension; status = file->write_all(bytes, timeout); }
      native_host_io_check(status); return NativeValue::integer(status.count);
    }
    if (selector == "read_all!" || selector == "read_exact!") {
      native_host_io_keywords(kwargs, selector == "read_all!" ? std::initializer_list<const char*>{"limit"} : std::initializer_list<const char*>{"timeout"});
      if (args.size() != (selector == "read_all!" ? 0U : 1U)) native_host_io_error("ArgumentError", "invalid file read arguments");
      if (!args.empty() && (args[0].tag != NativeValue::Tag::Integer || args[0].scalar_value < 0)) native_host_io_error("ArgumentError", "invalid read size");
      { NativeCycleSuspension suspension; status = selector == "read_all!" ? file->read_all(native_host_io_limit(kwargs)) : file->read_exact(args[0].scalar_value, timeout); }
      native_host_io_check(status); return NativeValue::bytes(status.bytes);
    }
    if (selector == "flush!" || selector == "sync!") {
      native_host_io_keywords(kwargs, {"timeout"});
      if (!args.empty()) native_host_io_error("ArgumentError", "file control takes no arguments");
      { NativeCycleSuspension suspension; status = selector == "sync!" ? file->sync(timeout) : file->flush(timeout); }
      native_host_io_check(status); return NativeValue::nullv();
    }
    native_host_io_error("NoMethodError", "unsupported file selector " + selector);
  }
  if (selector == "local_path" || selector == "remote_path") {
    native_host_io_keywords(kwargs, {});
    if (!args.empty()) native_host_io_error("ArgumentError", "endpoint takes no arguments");
    if ((!listener && !stream) || (listener && selector == "remote_path"))
      native_host_io_error("NoMethodError", "resource has no endpoint " + selector);
    return NativeValue::heap_string(listener ? listener->local_path() : selector == "local_path" ? stream->local_path() : stream->remote_path());
  }
  if (listener && (selector == "accept!" || selector == "try_accept!")) {
    native_host_io_keywords(kwargs, selector == "accept!" ? std::initializer_list<const char *>{"timeout"} : std::initializer_list<const char *>{});
    if (!args.empty()) native_host_io_error("ArgumentError", "accept takes no arguments");
    const auto timeout = native_host_io_timeout(kwargs);
    native_require_suspendable("unix.accept");
    RuntimeTcpAcceptResult result;
    { NativeCycleSuspension suspension; result = selector == "accept!" ? listener->accept(timeout) : listener->try_accept(); }
    native_host_io_check(result);
    return result.would_block ? NativeValue::nullv() : NativeValue::runtime_handle(Value::io_value(result.stream));
  }
  if (!stream) native_host_io_error("NoMethodError", "unknown IO selector " + selector);
  if (selector == "shutdown!" || selector == "close_read!" || selector == "close_write!" || selector == "flush!") {
    native_host_io_keywords(kwargs, selector == "flush!" ? std::initializer_list<const char *>{"timeout"} : std::initializer_list<const char *>{});
    if (args.size() > (selector == "shutdown!" ? 1U : 0U)) native_host_io_error("ArgumentError", "invalid stream control arguments");
    auto side = RuntimeShutdownSide::Both;
    if (!args.empty()) {
      const auto name = args[0].tag == NativeValue::Tag::Symbol ? native_symbol_text(args[0].scalar_value)
        : native_value_is_string(args[0]) ? native_string_text(args[0]) : "";
      const auto parsed = runtime_shutdown_side_from_name(name);
      if (!parsed) native_host_io_error("ArgumentError", "invalid shutdown side");
      side = *parsed;
    }
    auto timeout = native_host_io_timeout(kwargs); RuntimeIoStatus status;
    { NativeCycleSuspension suspension;
      status = selector == "shutdown!" ? stream->shutdown(side) : selector == "close_read!" ? stream->close_read()
        : selector == "close_write!" ? stream->close_write() : stream->flush(timeout); }
    native_host_io_check(status); return NativeValue::nullv();
  }
  if (selector == "write_all!" || selector == "write!") {
    native_host_io_keywords(kwargs, {"timeout"});
    if (args.size() != 1 || args[0].tag != NativeValue::Tag::Bytes) native_host_io_error("TypeError", "write expects Bytes");
    native_require_suspendable(selector.c_str());
    auto timeout = native_host_io_timeout(kwargs); RuntimeIoStatus status;
    { NativeCycleSuspension suspension;
      status = selector == "write_all!" ? stream->write_all(as_bytes(args[0]).bytes, timeout)
        : stream->write(as_bytes(args[0]).bytes, timeout); }
    native_host_io_check(status); return NativeValue::integer(status.count);
  }
  if (selector == "read_line!" || selector == "read_exact!" || selector == "read_all!") {
    const bool line = selector == "read_line!", exact = selector == "read_exact!";
    native_host_io_keywords(kwargs, {"limit", "timeout"});
    if (args.size() != (exact ? 1U : 0U)) native_host_io_error("ArgumentError", "invalid read arguments");
    native_require_suspendable(selector.c_str());
    auto limit = native_host_io_limit(kwargs);
    std::size_t count = 0;
    if (exact) {
      if (args[0].tag != NativeValue::Tag::Integer || args[0].scalar_value < 0) native_host_io_error("ArgumentError", "invalid read size");
      count = args[0].scalar_value;
    }
    const auto timeout = native_host_io_timeout(kwargs);
    const auto deadline = timeout == std::chrono::milliseconds::max() ? std::chrono::steady_clock::time_point::max() : std::chrono::steady_clock::now() + timeout;
    auto &pending = as_native_runtime_handle(receiver)->io_pending;
    std::string result;
    for (;;) {
      auto take = pending.size();
      auto newline = line ? pending.find('\n') : std::string::npos;
      if (newline != std::string::npos) take = newline;
      if (exact) take = std::min(take, count - result.size());
      result.append(pending, 0, take); pending.erase(0, take);
      if (limit && result.size() > *limit) native_host_io_error("ArgumentError", "read limit exceeded");
      if (line && newline != std::string::npos) { pending.erase(0, 1); return NativeValue::heap_string(std::move(result)); }
      if (exact && result.size() == count) return NativeValue::bytes(std::move(result));
      auto remaining = deadline == std::chrono::steady_clock::time_point::max() ? std::chrono::milliseconds::max()
        : std::max(std::chrono::milliseconds(0), std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
      RuntimeByteBuffer buffer(65536); RuntimeIoStatus status;
      { NativeCycleSuspension suspension; status = stream->read(buffer, remaining); }
      native_host_io_check(status);
      if (status.eof) {
        if (exact) native_host_io_error("UnexpectedEofError", "connection closed before read completed");
        if (line) return result.empty() ? NativeValue::nullv() : NativeValue::heap_string(std::move(result));
        return NativeValue::bytes(std::move(result));
      }
      pending = buffer.bytes();
    }
  }
  native_host_io_error("NoMethodError", "unsupported Unix stream selector: " + selector);
  return NativeValue::nullv();
}
