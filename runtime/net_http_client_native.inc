// Included by generated native C++ after the common HTTP adapters. This path
// shares client leases and the wire codec with the VM without creating a world.
#if AMBER_NATIVE_HTTP_CLIENT_DIRECT
static void native_http_client_error(const std::string &name,
                                     const std::string &message) {
  throw NativeRaised{native_named_error(name, message)};
}

static void native_http_client_error(amber::runtime::http::HttpErrorKind kind,
                                     const std::string &message) {
  native_http_client_error(amber::runtime::http::http_error_class_name(kind),
                           message);
}

static std::string native_http_client_text(const NativeValue &value) {
  if (native_value_is_string(value))
    return native_string_text(value);
  if (value.tag == NativeValue::Tag::Symbol)
    return native_symbol_text(value.scalar_value);
  native_http_client_error("TypeError", "HTTP value must be a Str or Symbol");
  return {};
}

static amber::runtime::http::HttpHeaders
native_http_client_headers(const NativeValue &value) {
  std::vector<std::pair<std::string, std::string>> pairs;
  if (!native_http_header_pairs_from_value(value, &pairs)) {
    native_http_client_error("TypeError",
                             "headers must be Headers, Map or pairs");
  }
  amber::runtime::http::HttpHeaders headers;
  for (const auto &[name, text] : pairs) {
    std::string error;
    if (!headers.add(name, text, &error))
      native_http_client_error("InvalidHeaderError", error);
  }
  return headers;
}

static NativeValue native_http_client_headers_value(
    const amber::runtime::http::HttpHeaders &values) {
  auto headers = std::make_shared<amber::runtime::RuntimeHttpHeaders>();
  headers->headers = values;
  headers->read_only = true;
  return native_http_runtime_handle_with_header_pairs(
      amber::runtime::Value::io_value(headers), values.pairs());
}

static NativeValue native_http_client_response_once(
    const std::shared_ptr<amber::runtime::RuntimeHttpClient> &client,
    const std::string &method, const std::string &url,
    const amber::runtime::http::HttpHeaders &headers, const std::string &body,
    bool has_body, std::chrono::milliseconds timeout,
    std::chrono::milliseconds pool_timeout) {
  using namespace amber::runtime;
  http::HttpRequest request;
  http::HttpErrorKind kind = http::HttpErrorKind::None;
  std::string error;
  if (!http::http_build_request(method, url, headers, body, has_body, true,
                                &request, &kind, &error))
    native_http_client_error(kind, error);
  const std::string endpoint =
      request.host + ":" + std::to_string(request.port);
  if (!amber::capability::capability_set_allows(embedded_capability_grants(),
                                                "net.connect", endpoint)) {
    native_http_client_error("CapabilityError",
                             "capability is not granted: net.connect=" +
                                 endpoint);
  }
  const std::string origin = request.scheme + "://" + endpoint;
  native_commit_effect();
  native_require_suspendable("http.request");
  NativeCycleSuspension suspension;
  RuntimeHttpPoolAcquire lease = client->acquire_slot(origin, pool_timeout);
  if (lease.kind == RuntimeHttpPoolAcquire::Kind::Closed)
    native_http_client_error("ClosedResourceError",
                             "net.http.Client is closed");
  if (lease.kind == RuntimeHttpPoolAcquire::Kind::Timeout)
    native_http_client_error("PoolTimeoutError",
                             "timed out waiting for an HTTP connection slot");
  auto transport = std::move(lease.transport);
  if (transport == nullptr)
    transport =
        http::http_connect(request, timeout, client->tls, &kind, &error);
  if (transport == nullptr) {
    client->release_transport(origin, nullptr, false);
    native_http_client_error(kind, error);
  }
  std::uint64_t written = 0;
  if (!http::http_write_request_head(*transport, request, &kind, &error) ||
      (has_body && !http::http_write_request_body_chunk(
                       *transport, request, body, &written, &kind, &error))) {
    client->release_transport(origin, std::move(transport), false);
    native_http_client_error(kind, error);
  }
  std::weak_ptr<RuntimeHttpClient> weak_client = client;
  auto release = [weak_client,
                  origin](std::unique_ptr<http::HttpTransport> stream,
                          bool reusable) {
    if (auto owner = weak_client.lock())
      owner->release_transport(origin, std::move(stream), reusable);
    else if (stream != nullptr)
      stream->close();
  };
  auto started = http::http_read_response_start(
      std::move(transport), request.method == "HEAD", {}, release);
  if (!started.ok) {
    client->release_transport(origin, nullptr, false);
    native_http_client_error(started.error_kind, started.error_message);
  }
  auto response = std::make_shared<RuntimeHttpResponse>();
  response->status = started.status;
  response->reason = std::move(started.reason);
  response->minor_version = started.minor_version;
  response->headers = std::move(started.headers);
  response->body = std::make_shared<RuntimeHttpResponseBody>();
  response->body->stream = std::move(started.body);
  return NativeValue::runtime_handle(Value::io_value(response));
}

static bool native_http_try_client_send(const NativeValue &receiver,
                                        const std::string &selector,
                                        const NativeArgsView &args,
                                        const NativeKeywordArgsView &kwargs,
                                        const NativeValue &block,
                                        NativeValue *out) {
  using namespace amber::runtime;
  if (receiver.tag != NativeValue::Tag::RuntimeHandle || out == nullptr)
    return false;
  const Value &value = as_native_runtime_handle(receiver)->value;
  if (value.is_native_type()) {
    const auto type = value.as_native_type().kind;
    if (type == RuntimeNativeTypeKind::Net && selector == "http" &&
        args.empty() && kwargs.empty()) {
      *out = NativeValue::runtime_handle(
          Value::native_type(RuntimeNativeTypeKind::NetHttp));
      return true;
    }
    if (type == RuntimeNativeTypeKind::NetHttp &&
        (selector == "Client" || selector == "Server" ||
         selector == "ServerResponse" || selector == "ServerRequest")) {
      const auto target =
          selector == "Client"   ? RuntimeNativeTypeKind::NetHttpClient
          : selector == "Server" ? RuntimeNativeTypeKind::NetHttpServer
          : selector == "ServerResponse"
              ? RuntimeNativeTypeKind::NetHttpServerResponse
              : RuntimeNativeTypeKind::NetHttpServerRequest;
      NativeValue callee =
          NativeValue::runtime_handle(Value::native_type(target));
      if (target == RuntimeNativeTypeKind::NetHttpServerRequest &&
          args.empty() && kwargs.empty() && block.tag == NativeValue::Tag::Null)
        *out = callee;
      else
        *out = native_http_send(callee, "new", args, kwargs, block);
      return true;
    }
    if (type != RuntimeNativeTypeKind::NetHttpClient ||
        (selector != "new" && selector != "__call__"))
      return false;
    if (!args.empty() || block.tag != NativeValue::Tag::Null)
      native_http_client_error("ArgumentError",
                               "net.http.Client takes only keywords");
    auto client = std::make_shared<RuntimeHttpClient>();
    for (const auto &keyword : kwargs) {
      const auto &option = keyword.value;
      std::chrono::milliseconds *duration =
          keyword.name == "timeout"        ? &client->timeout
          : keyword.name == "pool_timeout" ? &client->pool_timeout
          : keyword.name == "idle_timeout" ? &client->idle_timeout
                                           : nullptr;
      std::size_t *size =
          keyword.name == "max_idle_connections" ? &client->max_idle_connections
          : keyword.name == "max_idle_per_origin" ? &client->max_idle_per_origin
          : keyword.name == "max_active_per_origin"
              ? &client->max_active_per_origin
          : keyword.name == "max_redirects" ? &client->max_redirects
                                            : nullptr;
      std::string *path =
          keyword.name == "tls_ca_file"     ? &client->tls.ca_file
          : keyword.name == "tls_ca_path"   ? &client->tls.ca_path
          : keyword.name == "tls_cert_file" ? &client->tls.cert_file
          : keyword.name == "tls_key_file"  ? &client->tls.key_file
                                            : nullptr;
      if (duration != nullptr) {
        if (!native_http_server_duration(option, duration))
          native_http_client_error(
              "ArgumentError",
              keyword.name + " must be a nonnegative duration or null");
      } else if (size != nullptr) {
        if (!native_http_positive_size(
                option, keyword.name != "max_active_per_origin", size))
          native_http_client_error("ArgumentError",
                                   keyword.name + " has an invalid size");
      } else if (path != nullptr) {
        if (option.tag == NativeValue::Tag::Null)
          continue;
        if (!native_value_is_string(option))
          native_http_client_error("TypeError",
                                   keyword.name + " must be a Str or null");
        *path = native_string_text(option);
        if (path->empty() || path->find('\0') != std::string::npos)
          native_http_client_error("ArgumentError",
                                   keyword.name + " must be a nonempty path");
      } else if (keyword.name == "redirects") {
        const auto mode = native_http_client_text(option);
        if (mode == "safe")
          client->redirect_mode = RuntimeHttpRedirectMode::Safe;
        else if (mode == "manual")
          client->redirect_mode = RuntimeHttpRedirectMode::Manual;
        else if (mode != "off")
          native_http_client_error("ArgumentError",
                                   "redirects must be :off, :manual or :safe");
      } else {
        native_http_client_error("ArgumentError",
                                 "unknown Client keyword: " + keyword.name);
      }
    }
    if (client->tls.cert_file.empty() != client->tls.key_file.empty())
      native_http_client_error(
          "ArgumentError",
          "tls_cert_file and tls_key_file must be provided together");
    *out = NativeValue::runtime_handle(Value::io_value(client));
    return true;
  }
  if (!value.is_io_value())
    return false;
  const auto io = value.as_io_value();
  if (auto client = std::dynamic_pointer_cast<RuntimeHttpClient>(io)) {
    if (selector == "closed?" || selector == "close!" ||
        selector == "close_idle!") {
      if (!args.empty() || !kwargs.empty() ||
          block.tag != NativeValue::Tag::Null)
        native_http_client_error("ArgumentError",
                                 selector + " takes no arguments");
      if (selector == "closed?")
        *out = NativeValue::boolean(client->closed());
      else {
        native_commit_effect();
        if (selector == "close!")
          client->close_client();
        else
          client->close_idle();
        *out = NativeValue::nullv();
      }
      return true;
    }
    std::string method = selector;
    if (method != "get" && method != "head" && method != "post" &&
        method != "put" && method != "patch" && method != "delete" &&
        method != "query")
      return false;
    if (args.size() != 1U)
      native_http_client_error("ArgumentError", selector + " has wrong arity");
    std::string url = native_http_client_text(args[0]);
    http::HttpHeaders headers;
    std::string body;
    bool has_body = false;
    auto timeout = client->timeout;
    auto pool_timeout = client->pool_timeout;
    for (const auto &keyword : kwargs) {
      if (keyword.name == "headers")
        headers = native_http_client_headers(keyword.value);
      else if (keyword.name == "body") {
        if (keyword.value.tag != NativeValue::Tag::Null) {
          body = keyword.value.tag == NativeValue::Tag::Bytes
                     ? as_bytes(keyword.value).bytes
                     : native_http_client_text(keyword.value);
          has_body = true;
        }
      } else if (keyword.name == "timeout" || keyword.name == "pool_timeout") {
        if (!native_http_server_duration(
                keyword.value,
                keyword.name == "timeout" ? &timeout : &pool_timeout))
          native_http_client_error(
              "ArgumentError",
              "timeout must be a nonnegative duration or null");
      } else
        native_http_client_error("ArgumentError",
                                 "unknown request keyword: " + keyword.name);
    }
    if (client->closed())
      native_http_client_error("ClosedResourceError",
                               "net.http.Client is closed");
    std::vector<RuntimeHttpRedirectRecordData> redirects;
    for (;;) {
      NativeValue result = native_http_client_response_once(
          client, method, url, headers, body, has_body, timeout, pool_timeout);
      auto response = std::dynamic_pointer_cast<RuntimeHttpResponse>(
          as_native_runtime_handle(result)->value.as_io_value());
      response->redirects = redirects;
      const auto location = response->headers.first("location");
      const int status = response->status;
      const bool redirect = status == 301 || status == 302 || status == 303 ||
                            status == 307 || status == 308;
      http::HttpErrorKind kind = http::HttpErrorKind::None;
      std::string error, next_url;
      if (redirect && location &&
          http::http_resolve_location(url, *location, &next_url, &kind, &error))
        response->redirect_location = next_url;
      std::string normalized;
      http::http_normalize_method(method, &normalized);
      const bool follow =
          client->redirect_mode == RuntimeHttpRedirectMode::Safe && redirect &&
          (status == 303 || normalized == "QUERY" || normalized == "GET" ||
           normalized == "HEAD");
      if (!follow) {
        if (block.tag == NativeValue::Tag::Null)
          *out = result;
        else {
          try {
            *out = amber_native_call_closure(block, {result});
          } catch (...) {
            (void)response->body->close();
            throw;
          }
          (void)response->body->close();
        }
        return true;
      }
      if (!response->redirect_location) {
        (void)response->body->close();
        native_http_client_error(
            kind == http::HttpErrorKind::None ? http::HttpErrorKind::InvalidUrl
                                              : kind,
            error.empty() ? "redirect response has no valid Location" : error);
      }
      if (redirects.size() >= client->max_redirects) {
        (void)response->body->close();
        native_http_client_error("TooManyRedirectsError",
                                 "too many HTTP redirects");
      }
      http::HttpUrl previous, next;
      http::http_parse_url(url, &previous, &kind, &error);
      http::http_parse_url(next_url, &next, &kind, &error);
      const bool cross = previous.scheme != next.scheme ||
                         previous.host != next.host ||
                         previous.port != next.port;
      if (!response->body->stream->discard(std::nullopt, &kind, &error))
        native_http_client_error(kind, error);
      std::string after =
          status == 303 && normalized != "HEAD" ? "GET" : normalized;
      redirects.push_back({status, url, next_url, normalized, after, cross});
      if (cross) {
        headers.remove("authorization");
        headers.remove("cookie");
        headers.remove("proxy-authorization");
      }
      headers.remove("host");
      if (status == 303) {
        body.clear();
        has_body = false;
        headers.remove("content-length");
        headers.remove("content-type");
      }
      method = after;
      url = next_url;
    }
  }
  auto response = std::dynamic_pointer_cast<RuntimeHttpResponse>(io);
  auto body = std::dynamic_pointer_cast<RuntimeHttpResponseBody>(io);
  if (response != nullptr) {
    if (selector == "status" || selector == "reason" || selector == "version" ||
        selector == "headers" || selector == "body" || selector == "success?" ||
        selector == "ok?" || selector == "closed?" ||
        selector == "informational?" || selector == "client_error?" ||
        selector == "server_error?" || selector == "redirect?" ||
        selector == "redirect_location" || selector == "redirects" ||
        selector == "close!") {
      if (!args.empty() || !kwargs.empty() ||
          block.tag != NativeValue::Tag::Null)
        native_http_client_error("ArgumentError",
                                 selector + " takes no arguments");
      if (selector == "status")
        *out = NativeValue::integer(response->status);
      else if (selector == "reason")
        *out = NativeValue::heap_string(response->reason);
      else if (selector == "version")
        *out = NativeValue::heap_string(
            "1." + std::to_string(response->minor_version));
      else if (selector == "headers")
        *out = native_http_client_headers_value(response->headers);
      else if (selector == "body")
        *out = NativeValue::runtime_handle(Value::io_value(response->body));
      else if (selector == "informational?")
        *out = NativeValue::boolean(response->status >= 100 &&
                                    response->status < 200);
      else if (selector == "client_error?")
        *out = NativeValue::boolean(response->status >= 400 &&
                                    response->status < 500);
      else if (selector == "server_error?")
        *out = NativeValue::boolean(response->status >= 500);
      else if (selector == "closed?")
        *out = NativeValue::boolean(response->body->closed() ||
                                    response->body->stream->closed() ||
                                    response->body->stream->consumed());
      else if (selector == "success?" || selector == "ok?")
        *out = NativeValue::boolean(response->status >= 200 &&
                                    response->status < 300);
      else if (selector == "redirect?")
        *out = NativeValue::boolean(response->status >= 300 &&
                                    response->status < 400);
      else if (selector == "redirect_location")
        *out = response->redirect_location
                   ? NativeValue::heap_string(*response->redirect_location)
                   : NativeValue::nullv();
      else if (selector == "redirects") {
        std::vector<NativeValue> records;
        for (const auto &data : response->redirects) {
          auto record = std::make_shared<RuntimeHttpRedirectRecord>();
          record->data = data;
          records.push_back(
              NativeValue::runtime_handle(Value::io_value(record)));
        }
        *out = NativeValue::list(std::move(records));
      } else {
        (void)response->body->close();
        *out = NativeValue::nullv();
      }
      return true;
    }
    if (selector == "expect_status!") {
      if (args.empty() || !kwargs.empty() ||
          block.tag != NativeValue::Tag::Null)
        native_http_client_error("ArgumentError",
                                 "expect_status! expects status Ints");
      bool matched = false;
      std::string expected;
      for (const auto &status : args) {
        if (status.tag != NativeValue::Tag::Integer)
          native_http_client_error("TypeError", "expected status must be Int");
        if (!expected.empty())
          expected += ", ";
        expected += std::to_string(status.scalar_value);
        matched = matched || response->status == status.scalar_value;
      }
      if (!matched)
        native_http_client_error("HttpError",
                                 "expected HTTP status " + expected + ", got " +
                                     std::to_string(response->status));
      *out = receiver;
      return true;
    }
    if (selector != "body_text" && selector != "body_bytes" &&
        selector != "json")
      return false;
    body = response->body;
  }
  if (auto record = std::dynamic_pointer_cast<RuntimeHttpRedirectRecord>(io)) {
    if (!args.empty() || !kwargs.empty() || block.tag != NativeValue::Tag::Null)
      return false;
    if (selector == "status")
      *out = NativeValue::integer(record->data.status);
    else if (selector == "from_url")
      *out = NativeValue::heap_string(record->data.from_url);
    else if (selector == "to_url")
      *out = NativeValue::heap_string(record->data.to_url);
    else if (selector == "method_before")
      *out = NativeValue::heap_string(record->data.method_before);
    else if (selector == "method_after")
      *out = NativeValue::heap_string(record->data.method_after);
    else if (selector == "cross_origin?")
      *out = NativeValue::boolean(record->data.cross_origin);
    else
      return false;
    return true;
  }
  if (body == nullptr)
    return false;
  if (selector == "closed?" || selector == "consumed?" ||
      selector == "close!" || selector == "trailers") {
    if (!args.empty() || !kwargs.empty() || block.tag != NativeValue::Tag::Null)
      native_http_client_error("ArgumentError",
                               selector + " takes no arguments");
    if (selector == "closed?")
      *out = NativeValue::boolean(body->closed() || body->stream->closed());
    else if (selector == "consumed?")
      *out = NativeValue::boolean(body->stream->consumed());
    else if (selector == "trailers")
      *out = body->stream->consumed()
                 ? native_http_client_headers_value(body->stream->trailers())
                 : NativeValue::nullv();
    else {
      (void)body->close();
      *out = NativeValue::nullv();
    }
    return true;
  }
  const bool json = response != nullptr && selector == "json";
  const bool text = selector == "body_text" || selector == "text" || json;
  const bool bytes = selector == "body_bytes" || selector == "bytes";
  const bool discard = selector == "discard!";
  const bool read = selector == "read";
  const bool each = selector == "each_chunk";
  if (!text && !bytes && !discard && !read && !each)
    return false;
  if (!args.empty() || (each ? block.tag != NativeValue::Tag::Closure
                             : block.tag != NativeValue::Tag::Null))
    native_http_client_error("ArgumentError",
                             selector + " has invalid arguments");
  std::optional<std::size_t> limit;
  std::size_t max = 8192;
  for (const auto &keyword : kwargs) {
    if (keyword.name == "limit" && !read && !each) {
      if (keyword.value.tag != NativeValue::Tag::Null) {
        std::size_t size;
        if (!native_http_positive_size(keyword.value, true, &size))
          native_http_client_error("ArgumentError",
                                   "limit must be nonnegative Int");
        limit = size;
      }
    } else if ((keyword.name == "max_bytes" && read) ||
               (keyword.name == "size" && each)) {
      if (!native_http_positive_size(keyword.value, read, &max))
        native_http_client_error("ArgumentError",
                                 "max_bytes must be positive Int");
    } else if (keyword.name == "timeout" && each) {
      // The existing VM accepts this keyword; IO uses the client timeout.
      continue;
    } else if (keyword.name == "encoding" && text && !json) {
      const auto encoding = native_http_client_text(keyword.value);
      if (encoding != "utf8" && encoding != "utf-8")
        native_http_client_error("ArgumentError", "unsupported text encoding");
    } else
      native_http_client_error("ArgumentError",
                               "unknown body keyword: " + keyword.name);
  }
  const auto mode = text      ? RuntimeHttpBodyMode::Text
                    : bytes   ? RuntimeHttpBodyMode::Bytes
                    : discard ? RuntimeHttpBodyMode::Discard
                    : each    ? RuntimeHttpBodyMode::Each
                              : RuntimeHttpBodyMode::Read;
  if (!body->access_status().ok)
    native_http_client_error(body->access_status().error_name,
                             body->access_status().message);
  if (body->mode != RuntimeHttpBodyMode::None && !(read && body->mode == mode))
    native_http_client_error("BodyConsumedError",
                             "response body is already consumed");
  body->mode = mode;
  native_commit_effect();
  native_require_suspendable("http.response_body");
  NativeCycleSuspension suspension;
  http::HttpErrorKind kind = http::HttpErrorKind::None;
  std::string error, payload;
  if (each) {
    try {
      while (true) {
        if (!body->stream->read(max, &payload, &kind, &error))
          native_http_client_error(kind, error);
        if (payload.empty())
          break;
        (void)amber_native_call_closure(block, {NativeValue::bytes(payload)});
      }
    } catch (...) {
      (void)body->close();
      throw;
    }
    *out = NativeValue::nullv();
  } else if (discard) {
    if (!body->stream->discard(limit, &kind, &error))
      native_http_client_error(kind, error);
    *out = NativeValue::nullv();
  } else {
    const bool success =
        read ? body->stream->read(max, &payload, &kind, &error)
             : body->stream->read_all(limit, &payload, &kind, &error);
    if (!success)
      native_http_client_error(kind, error);
    if (text) {
      *out = json ? native_json_parse_value(NativeValue::heap_string(payload))
                  : NativeValue::heap_string(payload);
    } else
      *out = NativeValue::bytes(payload);
  }
  return true;
}
#else
static bool native_http_try_client_send(const NativeValue &,
                                        const std::string &,
                                        const NativeArgsView &,
                                        const NativeKeywordArgsView &,
                                        const NativeValue &, NativeValue *) {
  return false;
}
#endif
