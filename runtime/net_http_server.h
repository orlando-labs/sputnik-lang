#pragma once

// HTTP server runtime state shared by the bytecode VM and host-native
// executables.  These types deliberately contain no Frame or Vm state; the
// transport/serve implementation can therefore move behind a standalone
// native runtime ABI without changing the public resource representation.

#include "runtime/concurrency.h"
#include "runtime/http_codec.h"
#include "runtime/io.h"
#include "runtime/value.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace amber::runtime {

struct RuntimeHttpServerStats {
  std::uint64_t accepted = 0;
  std::uint64_t completed = 0;
  std::uint64_t failed = 0;
  std::uint64_t rejected = 0;
  std::uint64_t active = 0;
  std::uint64_t requests = 0;
  std::uint64_t active_requests = 0;
  std::uint64_t keepalive_requests = 0;
  std::uint64_t forced_shutdowns = 0;
  std::uint64_t capacity = 0;
};

struct RuntimeHttpServerOptions {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  std::size_t workers = 1;
  std::size_t max_concurrent_per_worker = 64;
  int backlog = 128;
  bool reuse_addr = true;
  std::size_t max_header_bytes = 65536;
  std::size_t max_body_bytes = 1024 * 1024;
  std::chrono::milliseconds read_timeout = std::chrono::milliseconds(30000);
  std::chrono::milliseconds write_timeout = std::chrono::milliseconds(30000);
  std::chrono::milliseconds idle_timeout = std::chrono::milliseconds(15000);
  std::size_t max_requests_per_connection = 100;
};

class RuntimeHttpServer final : public RuntimeIoResource {
  struct ActiveConnection {
    std::shared_ptr<RuntimeTcpStream> stream;
    RuntimeTaskHandle handle;
    bool has_handle = false;
  };

public:
  RuntimeHttpServer()
      : RuntimeIoResource(RuntimeIsolationMode::Unchecked) {}
  const char *type_name() const override { return "net.http.Server"; }
  bool shareable() const override { return true; }

  RuntimeIoStatus close() override {
    const bool do_close = stop_accepting();
    if (do_close) {
      return RuntimeIoResource::close();
    }
    RuntimeIoStatus ok;
    ok.ok = true;
    return ok;
  }

  bool stop_accepting() {
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      changed = accepting_;
      accepting_ = false;
    }
    cv_.notify_all();
    if (listener != nullptr) {
      (void)listener->close();
    }
    return changed;
  }

  bool accepting() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return accepting_;
  }

  bool closed_server() const { return !accepting(); }

  std::optional<std::uint64_t>
  try_acquire_connection(const std::shared_ptr<RuntimeTcpStream> &stream) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_ || active_connections_.size() >= capacity()) {
      return std::nullopt;
    }
    const std::uint64_t id = next_connection_id_++;
    ActiveConnection active;
    active.stream = stream;
    active_connections_.emplace(id, std::move(active));
    ++stats_.accepted;
    stats_.active = static_cast<std::uint64_t>(active_connections_.size());
    return id;
  }

  void attach_task(std::uint64_t id, RuntimeTaskHandle handle) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto found = active_connections_.find(id);
      if (found != active_connections_.end()) {
        found->second.handle = std::move(handle);
        found->second.has_handle = true;
      }
    }
    cv_.notify_all();
  }

  void begin_request(bool keepalive) {
    request_count_.fetch_add(1, std::memory_order_relaxed);
    active_requests_.fetch_add(1, std::memory_order_relaxed);
    if (keepalive) {
      keepalive_request_count_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void end_request() {
    std::size_t active = active_requests_.load(std::memory_order_relaxed);
    while (active != 0 &&
           !active_requests_.compare_exchange_weak(
               active, active - 1U, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
  }

  void release_connection(std::uint64_t id, bool failed) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto found = active_connections_.find(id);
      if (found == active_connections_.end()) {
        return;
      }
      active_connections_.erase(found);
      ++stats_.completed;
      if (failed) {
        ++stats_.failed;
      }
      stats_.active = static_cast<std::uint64_t>(active_connections_.size());
    }
    cv_.notify_all();
  }

  void reject_request() {
    rejected_request_count_.fetch_add(1, std::memory_order_relaxed);
  }

  void wait_until_quiescent() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] { return active_connections_.empty(); });
  }

  bool shutdown(std::chrono::milliseconds timeout) {
    stop_accepting();
    std::vector<std::shared_ptr<RuntimeTcpStream>> remaining_streams;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      const bool drained = cv_.wait_for(
          lock, timeout, [&] { return active_connections_.empty(); });
      if (drained) {
        return true;
      }
      ++stats_.forced_shutdowns;
      remaining_streams.reserve(active_connections_.size());
      for (const auto &entry : active_connections_) {
        remaining_streams.push_back(entry.second.stream);
      }
    }
    for (const auto &stream : remaining_streams) {
      if (stream != nullptr) {
        (void)stream->close();
      }
    }

    // A forced shutdown can race the interval between registering an accepted
    // connection and publishing its task handle. Keep the record until that
    // publication (or normal task completion).
    std::vector<std::pair<std::uint64_t, RuntimeTaskHandle>> remaining_tasks;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [&] {
        for (const auto &entry : active_connections_) {
          if (!entry.second.has_handle) {
            return false;
          }
        }
        return true;
      });
      remaining_tasks.reserve(active_connections_.size());
      for (const auto &entry : active_connections_) {
        remaining_tasks.push_back({entry.first, entry.second.handle});
      }
    }
    for (auto &entry : remaining_tasks) {
      (void)entry.second.cancel();
    }
    for (auto &entry : remaining_tasks) {
      (void)entry.second.wait();
      release_connection(entry.first, true);
    }
    return false;
  }

  std::size_t capacity() const { return workers * max_concurrent_per_worker; }

  RuntimeHttpServerStats stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeHttpServerStats out = stats_;
    out.active = static_cast<std::uint64_t>(active_connections_.size());
    out.requests = request_count_.load(std::memory_order_relaxed);
    out.active_requests = static_cast<std::uint64_t>(
        active_requests_.load(std::memory_order_relaxed));
    out.keepalive_requests =
        keepalive_request_count_.load(std::memory_order_relaxed);
    out.rejected = rejected_request_count_.load(std::memory_order_relaxed);
    out.capacity = static_cast<std::uint64_t>(capacity());
    return out;
  }

  std::shared_ptr<RuntimeTcpListener> listener;
  std::shared_ptr<RuntimeTaskModule> task;
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  std::size_t workers = 1;
  std::size_t max_concurrent_per_worker = 64;
  std::size_t max_header_bytes = 65536;
  std::size_t max_body_bytes = 1024 * 1024;
  std::chrono::milliseconds read_timeout = std::chrono::milliseconds(30000);
  std::chrono::milliseconds write_timeout = std::chrono::milliseconds(30000);
  std::chrono::milliseconds idle_timeout = std::chrono::milliseconds(15000);
  std::size_t max_requests_per_connection = 100;

private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool accepting_ = true;
  std::uint64_t next_connection_id_ = 1;
  std::atomic<std::uint64_t> request_count_{0};
  std::atomic<std::size_t> active_requests_{0};
  std::atomic<std::uint64_t> keepalive_request_count_{0};
  std::atomic<std::uint64_t> rejected_request_count_{0};
  std::unordered_map<std::uint64_t, ActiveConnection> active_connections_;
  RuntimeHttpServerStats stats_;
};

struct RuntimeHttpServerOpenResult : RuntimeIoStatus {
  std::shared_ptr<RuntimeHttpServer> server;
};

// Open the transport and scheduler for an HTTP server after the caller has
// performed its language/profile capability check. This layer contains no
// Value, Frame, RuntimeWorld, or Vm dispatch.
RuntimeHttpServerOpenResult
runtime_http_server_open(RuntimeHttpServerOptions options);

const char *runtime_http_server_reason_phrase(int status);

// Ordered, case-insensitive header handle shared by client and server HTTP
// values. Read-only request views are safe to retain across the typed native
// request adapter.
class RuntimeHttpHeaders final : public RuntimeIoValue {
public:
  const char *type_name() const override { return "net.http.Headers"; }
  bool shareable() const override { return read_only; }
  http::HttpHeaders headers;
  bool read_only = false;
};

class RuntimeHttpServerRequestBody;

class RuntimeHttpServerRequest final : public RuntimeIoValue {
public:
  const char *type_name() const override { return "net.http.ServerRequest"; }
  bool shareable() const override { return false; }

  std::string method;
  std::string target;
  std::string path;
  std::string query;
  http::HttpHeaders headers;
  std::shared_ptr<RuntimeHttpServerRequestBody> body_stream;
  RuntimeEndpoint local_endpoint;
  RuntimeEndpoint remote_endpoint;
  int minor_version = 1;
  bool keep_alive = true;
};

enum class RuntimeHttpServerRequestFraming {
  Empty,
  ContentLength,
  Chunked,
};

class RuntimeHttpServerRequestChunk final : public RuntimeIoValue {
public:
  const char *type_name() const override {
    return "net.http.ServerRequestChunk";
  }
  bool shareable() const override { return false; }

  std::string data;
  std::vector<http::HttpChunkExtension> extensions;
};

class RuntimeHttpServerRequestBody final : public RuntimeIoValue {
public:
  const char *type_name() const override {
    return "net.http.ServerRequestBody";
  }
  bool shareable() const override { return false; }

  enum class ChunkState { Size, Data, Trailers, Complete };
  enum class Consumption { Unset, Chunks, Whole };

  std::shared_ptr<RuntimeTcpStream> stream;
  std::shared_ptr<RuntimeTaskModule> task;
  RuntimeHttpServerRequestFraming framing =
      RuntimeHttpServerRequestFraming::Empty;
  std::string buffered;
  std::uint64_t content_remaining = 0;
  std::uint64_t chunk_remaining = 0;
  std::uint64_t decoded_bytes = 0;
  std::size_t trailer_bytes = 0;
  std::size_t max_body_bytes = 0;
  std::size_t max_header_bytes = 65536;
  std::chrono::milliseconds read_timeout =
      std::chrono::milliseconds::max();
  std::optional<std::chrono::steady_clock::time_point> read_deadline;
  ChunkState chunk_state = ChunkState::Size;
  Consumption consumption = Consumption::Unset;
  std::vector<http::HttpChunkExtension> current_extensions;
  http::HttpHeaders trailers;
  std::string whole_body;
  bool whole_complete = false;
  bool closed = false;
  bool expect_continue = false;
  bool continue_sent = false;
  std::shared_ptr<std::atomic<bool>> final_response_started =
      std::make_shared<std::atomic<bool>>(false);
};

class RuntimeHttpServerResponseWriter final : public RuntimeIoValue {
public:
  const char *type_name() const override {
    return "net.http.ServerResponseWriter";
  }
  bool shareable() const override { return false; }

  enum class Pending { None, Head, Chunk, Close, Trailer, Finish };

  std::shared_ptr<RuntimeTcpStream> stream;
  std::shared_ptr<RuntimeTaskModule> task;
  std::chrono::milliseconds write_timeout =
      std::chrono::milliseconds::max();
  std::optional<std::chrono::steady_clock::time_point> write_deadline;
  std::vector<std::string> declared_trailers;
  std::shared_ptr<std::atomic<bool>> final_response_started;
  std::string pending_bytes;
  std::size_t pending_offset = 0;
  Pending pending = Pending::None;
  bool head_sent = false;
  bool body_closed = false;
  bool finished = false;
};

class RuntimeHttpServerResponse final : public RuntimeIoValue {
public:
  const char *type_name() const override { return "net.http.ServerResponse"; }
  bool shareable() const override { return true; }

  int status = 200;
  std::string reason;
  http::HttpHeaders headers;
  std::string body;
  std::vector<std::string> trailer_names;
  std::function<std::function<Value()>(std::vector<Value>)> producer_factory;

  bool streaming() const { return static_cast<bool>(producer_factory); }
};

struct RuntimeHttpServerBodyReadResult : RuntimeIoStatus {
  bool parked = false;
  std::string body;
};

RuntimeHttpServerBodyReadResult runtime_http_server_read_body_all(
    const std::shared_ptr<RuntimeHttpServerRequestBody> &body);

struct RuntimeHttpServerBodyChunkReadResult
    : RuntimeHttpServerBodyReadResult {
  std::shared_ptr<RuntimeHttpServerRequestChunk> chunk;
};

RuntimeHttpServerBodyChunkReadResult runtime_http_server_read_body_chunk(
    const std::shared_ptr<RuntimeHttpServerRequestBody> &body,
    std::size_t max_bytes);

struct RuntimeHttpServerWriterResult : RuntimeIoStatus {
  bool parked = false;
};

RuntimeHttpServerWriterResult runtime_http_server_writer_write(
    const std::shared_ptr<RuntimeHttpServerResponseWriter> &writer,
    std::string data, std::vector<http::HttpChunkExtension> extensions = {});
RuntimeHttpServerWriterResult runtime_http_server_writer_close(
    const std::shared_ptr<RuntimeHttpServerResponseWriter> &writer);
RuntimeHttpServerWriterResult runtime_http_server_writer_trailer(
    const std::shared_ptr<RuntimeHttpServerResponseWriter> &writer,
    std::string name, std::string value);
RuntimeHttpServerWriterResult runtime_http_server_writer_finish(
    const std::shared_ptr<RuntimeHttpServerResponseWriter> &writer);

struct RuntimeNativeHttpServerRequestSnapshot {
  std::string method;
  std::string target;
  std::string path;
  std::optional<std::string> query;
  std::vector<std::pair<std::string, std::string>> header_pairs;
  Value headers = Value::null();
  Value body_stream = Value::null();
  Value local_endpoint = Value::null();
  Value remote_endpoint = Value::null();
};

std::optional<RuntimeNativeHttpServerRequestSnapshot>
runtime_native_http_server_request_snapshot(const Value &value);

struct RuntimeHttpServerServeResult : RuntimeIoStatus {
  RuntimeHttpServerStats stats;
};

// Runs the accept/request/response loop against a generated native block.
// The block's typed HTTP entry owns Amber value conversion; this transport
// layer remains independent from Vm, Frame, RuntimeWorld, and bytecode.
RuntimeHttpServerServeResult runtime_http_server_serve_native(
    const std::shared_ptr<RuntimeHttpServer> &server,
    const std::shared_ptr<RuntimeNativeBlock> &handler,
    std::optional<std::size_t> max_requests = std::nullopt);

} // namespace amber::runtime
