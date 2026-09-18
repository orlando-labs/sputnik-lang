#pragma once

// HTTP client resource state shared by VM dispatch and direct native code.
#include "runtime/io.h"
#include "runtime/net_http.h"
#include "runtime/tls.h"
#include "runtime/value.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace amber::runtime {

struct RuntimeHttpPoolAcquire {
  enum class Kind { Reused, OpenNew, Closed, Timeout };
  Kind kind = Kind::OpenNew;
  std::unique_ptr<http::HttpTransport> transport;
};

enum class RuntimeHttpRedirectMode { Off, Manual, Safe };

struct RuntimeHttpRedirectRecordData {
  int status = 0;
  std::string from_url;
  std::string to_url;
  std::string method_before;
  std::string method_after;
  bool cross_origin = false;
};

// net.http Amber-facing value instances (DESIGN-stdlib-net-http-io §7/§12),
// wrapped as io values via Value::io_value and dispatched in the is_io_value
// SEND chain. Response bodies and request handles own active pool leases until
// EOF/discard, early close, or failure.
class RuntimeHttpClient final : public RuntimeIoValue {
  struct RuntimeHttpIdleConnection {
    std::string origin;
    std::unique_ptr<http::HttpTransport> transport;
    std::chrono::steady_clock::time_point idle_since;
    std::uint64_t sequence = 0;
  };

public:
  const char *type_name() const override { return "net.http.Client"; }
  bool shareable() const override { return trace_hook.is_null(); }

  std::chrono::milliseconds timeout = std::chrono::milliseconds::max();
  std::chrono::milliseconds pool_timeout = std::chrono::milliseconds(5000);
  std::chrono::milliseconds idle_timeout = std::chrono::milliseconds(90000);
  std::size_t max_idle_connections = 64;
  std::size_t max_idle_per_origin = 4;
  std::size_t max_active_per_origin = 16;
  RuntimeHttpRedirectMode redirect_mode = RuntimeHttpRedirectMode::Off;
  std::size_t max_redirects = 5;
  Value trace_hook = Value::null();
  RuntimeTlsOptions tls;

  bool closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

  void close_client() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    close_idle_locked();
    cv_.notify_all();
  }

  void close_idle() {
    std::lock_guard<std::mutex> lock(mutex_);
    close_idle_locked();
  }

  RuntimeHttpPoolAcquire acquire_slot(const std::string &origin,
                                      std::chrono::milliseconds wait_timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto deadline = wait_timeout == std::chrono::milliseconds::max()
                              ? std::chrono::steady_clock::time_point::max()
                              : std::chrono::steady_clock::now() + wait_timeout;

    for (;;) {
      prune_idle_locked(std::chrono::steady_clock::now());
      if (closed_) {
        return {RuntimeHttpPoolAcquire::Kind::Closed, nullptr};
      }

      auto idle = idle_by_origin_.find(origin);
      if (idle != idle_by_origin_.end() && !idle->second.empty()) {
        RuntimeHttpIdleConnection lease = std::move(idle->second.front());
        idle->second.pop_front();
        --idle_count_;
        if (idle->second.empty()) {
          idle_by_origin_.erase(idle);
        }
        ++active_by_origin_[origin];
        RuntimeHttpPoolAcquire result;
        result.kind = RuntimeHttpPoolAcquire::Kind::Reused;
        result.transport = std::move(lease.transport);
        return result;
      }

      if (active_by_origin_[origin] < max_active_per_origin) {
        ++active_by_origin_[origin];
        return {RuntimeHttpPoolAcquire::Kind::OpenNew, nullptr};
      }

      if (wait_timeout == std::chrono::milliseconds(0)) {
        return {RuntimeHttpPoolAcquire::Kind::Timeout, nullptr};
      }
      if (wait_timeout == std::chrono::milliseconds::max()) {
        cv_.wait(lock);
      } else if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
        return {RuntimeHttpPoolAcquire::Kind::Timeout, nullptr};
      }
    }
  }

  void release_transport(const std::string &origin,
                         std::unique_ptr<http::HttpTransport> transport,
                         bool reusable) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto active = active_by_origin_.find(origin);
      if (active != active_by_origin_.end() && active->second > 0U) {
        --active->second;
        if (active->second == 0U) {
          active_by_origin_.erase(active);
        }
      }
      if (transport != nullptr && reusable && !closed_ &&
          max_idle_connections > 0U && max_idle_per_origin > 0U) {
        RuntimeHttpIdleConnection idle;
        idle.origin = origin;
        idle.transport = std::move(transport);
        idle.idle_since = std::chrono::steady_clock::now();
        idle.sequence = next_idle_sequence_++;
        idle_by_origin_[origin].push_back(std::move(idle));
        ++idle_count_;
        evict_idle_locked();
      } else if (transport != nullptr) {
        transport->close();
      }
    }
    cv_.notify_all();
  }

private:
  void close_idle_locked() {
    for (auto &bucket : idle_by_origin_) {
      for (auto &idle : bucket.second) {
        if (idle.transport != nullptr) {
          idle.transport->close();
        }
      }
    }
    idle_by_origin_.clear();
    idle_count_ = 0;
  }

  void prune_idle_locked(std::chrono::steady_clock::time_point now) {
    if (idle_timeout == std::chrono::milliseconds::max()) {
      return;
    }
    for (auto it = idle_by_origin_.begin(); it != idle_by_origin_.end();) {
      auto &bucket = it->second;
      while (!bucket.empty() &&
             now - bucket.front().idle_since >= idle_timeout) {
        if (bucket.front().transport != nullptr) {
          bucket.front().transport->close();
        }
        bucket.pop_front();
        --idle_count_;
      }
      if (bucket.empty()) {
        it = idle_by_origin_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void evict_idle_locked() {
    for (;;) {
      bool evicted = false;
      for (auto it = idle_by_origin_.begin(); it != idle_by_origin_.end();) {
        auto &bucket = it->second;
        while (bucket.size() > max_idle_per_origin) {
          if (bucket.front().transport != nullptr) {
            bucket.front().transport->close();
          }
          bucket.pop_front();
          --idle_count_;
          evicted = true;
        }
        if (bucket.empty()) {
          it = idle_by_origin_.erase(it);
        } else {
          ++it;
        }
      }
      if (idle_count_ <= max_idle_connections) {
        return;
      }

      auto oldest_bucket = idle_by_origin_.end();
      for (auto it = idle_by_origin_.begin(); it != idle_by_origin_.end();
           ++it) {
        if (it->second.empty()) {
          continue;
        }
        if (oldest_bucket == idle_by_origin_.end() ||
            it->second.front().idle_since <
                oldest_bucket->second.front().idle_since ||
            (it->second.front().idle_since ==
                 oldest_bucket->second.front().idle_since &&
             it->second.front().sequence <
                 oldest_bucket->second.front().sequence)) {
          oldest_bucket = it;
        }
      }
      if (oldest_bucket == idle_by_origin_.end()) {
        idle_count_ = 0;
        return;
      }
      if (oldest_bucket->second.front().transport != nullptr) {
        oldest_bucket->second.front().transport->close();
      }
      oldest_bucket->second.pop_front();
      --idle_count_;
      evicted = true;
      if (oldest_bucket->second.empty()) {
        idle_by_origin_.erase(oldest_bucket);
      }
      if (!evicted) {
        return;
      }
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool closed_ = false;
  std::unordered_map<std::string, std::deque<RuntimeHttpIdleConnection>>
      idle_by_origin_;
  std::unordered_map<std::string, std::size_t> active_by_origin_;
  std::size_t idle_count_ = 0;
  std::uint64_t next_idle_sequence_ = 0;
};

enum class RuntimeHttpRequestBodyKind { Static, Producer, Reader };

class RuntimeHttpRequestBody final : public RuntimeIoValue {
public:
  const char *type_name() const override { return "net.http.RequestBody"; }
  bool shareable() const override {
    // Static request bodies are immutable snapshots and replayable. Producer
    // and reader-backed bodies carry executable/resource state and stay strand
    // local.
    return kind == RuntimeHttpRequestBodyKind::Static;
  }

  RuntimeHttpRequestBodyKind kind = RuntimeHttpRequestBodyKind::Static;
  std::string bytes;
  std::optional<std::uint64_t> length;
  std::optional<std::string> content_type;
  Value producer = Value::null();
  Value reader = Value::null();
};

enum class RuntimeHttpBodyMode { None, Read, Bytes, Text, Each, Discard };

class RuntimeHttpResponseBody final : public RuntimeIoResource {
public:
  RuntimeHttpResponseBody()
      : RuntimeIoResource(RuntimeIsolationMode::Checked) {}
  const char *type_name() const override { return "net.http.ResponseBody"; }
  RuntimeIoStatus close() override {
    if (stream != nullptr) {
      stream->close();
    }
    return RuntimeIoResource::close();
  }

  std::unique_ptr<http::HttpResponseBodyStream> stream;
  RuntimeHttpBodyMode mode = RuntimeHttpBodyMode::None;
  Value trace_hook = Value::null();
  std::string trace_origin;
  std::string trace_method;
  std::string trace_url;
  int trace_status = 0;
};

class RuntimeHttpRedirectRecord final : public RuntimeIoValue {
public:
  const char *type_name() const override { return "net.http.RedirectRecord"; }
  bool shareable() const override { return true; }
  RuntimeHttpRedirectRecordData data;
};

// An immutable request snapshot (§8). Shareable across strands: it owns no
// resource and is never mutated after construction. Static bodies remain
// shareable; streaming bodies deliberately do not.
class RuntimeHttpRequest final : public RuntimeIoValue {
public:
  const char *type_name() const override { return "net.http.Request"; }
  bool shareable() const override {
    return body == nullptr || body->shareable();
  }
  std::string method; // normalized uppercase token
  std::string url;
  http::HttpHeaders headers;
  std::shared_ptr<RuntimeHttpRequestBody> body;
};

class RuntimeHttpResponse final : public RuntimeIoValue {
public:
  const char *type_name() const override { return "net.http.Response"; }
  int status = 0;
  std::string reason;
  int minor_version = 1;
  http::HttpHeaders headers;
  std::shared_ptr<RuntimeHttpResponseBody> body;
  std::optional<std::string> redirect_location;
  std::vector<RuntimeHttpRedirectRecordData> redirects;
};

class RuntimeHttpRequestHandle final : public RuntimeIoResource {
public:
  RuntimeHttpRequestHandle()
      : RuntimeIoResource(RuntimeIsolationMode::Checked) {}
  const char *type_name() const override { return "net.http.RequestHandle"; }
  RuntimeIoStatus close() override {
    if (transport != nullptr) {
      if (pool_active) {
        if (const std::shared_ptr<RuntimeHttpClient> owner = client.lock()) {
          owner->release_transport(pool_origin, std::move(transport),
                                   /*reusable=*/false);
        } else {
          transport->close();
          transport.reset();
        }
        pool_active = false;
      } else {
        transport->close();
        transport.reset();
      }
    }
    aborted = true;
    return RuntimeIoResource::close();
  }

  std::weak_ptr<RuntimeHttpClient> client;
  std::string pool_origin;
  std::unique_ptr<http::HttpTransport> transport;
  http::HttpRequest request;
  std::chrono::milliseconds timeout = std::chrono::milliseconds::max();
  Value trace_hook = Value::null();
  std::string trace_url;
  std::uint64_t bytes_written = 0;
  bool pool_active = false;
  bool finished = false;
  bool response_returned = false;
  bool aborted = false;
  // A buffered request can race an early final response (notably 413) while
  // its body is still being written. Preserve the transport long enough to
  // read that response instead of replacing it with the socket write error.
  bool recover_early_response = false;
  bool request_write_failed = false;
  http::HttpErrorKind request_write_error_kind =
      http::HttpErrorKind::Connection;
  std::string request_write_error;
};

} // namespace amber::runtime
