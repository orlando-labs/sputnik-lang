#include "runtime/net_http_server.h"

#include "runtime/context.h"
#include "runtime/reactor.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace amber::runtime {

namespace {

struct NativeHttpIoParkGuard {
  bool previous = false;
  NativeHttpIoParkGuard() : previous(tls_runtime_io_park_enabled) {
    tls_runtime_io_park_enabled = true;
    tls_runtime_io_park_requested = false;
  }
  ~NativeHttpIoParkGuard() { tls_runtime_io_park_enabled = previous; }
};

struct NativeHttpIoBlockingGuard {
  bool previous = false;
  NativeHttpIoBlockingGuard() : previous(tls_runtime_io_park_enabled) {
    // Generated native closures do not yet persist a resumable program
    // counter. A cooperative park here would restart the handler and replay
    // effects before body_text/read_chunk. Keep each body read blocking and
    // bounded by the request deadline until native closures gain continuations.
    tls_runtime_io_park_enabled = false;
    tls_runtime_io_park_requested = false;
  }
  ~NativeHttpIoBlockingGuard() {
    tls_runtime_io_park_enabled = previous;
  }
};

bool park_native_http_task(
    const std::shared_ptr<RuntimeTaskModule> &task) {
  if (task == nullptr || !tls_runtime_io_park_requested) {
    return false;
  }
  const RuntimeIoParkRequest request = tls_runtime_io_park_request;
  tls_runtime_io_park_requested = false;
  const std::uint64_t self_id = current_runtime_strand_id();
  if (!task->scheduler().park_current(std::nullopt)) {
    return false;
  }
  runtime_mark_task_parked();
  RuntimeReactor::instance().wait_async(
      request.fd,
      request.want_write ? ReactorInterest::Write : ReactorInterest::Read,
      request.deadline, runtime_wait_cancel_flag(),
      [task, self_id, cancel_owner = current_runtime_task_cancel_owner(),
       run_owner = current_runtime_run_cancellation()](ReactorOutcome) {
        (void)cancel_owner; (void)run_owner;
        task->scheduler().wake_strand(self_id);
      });
  return true;
}

const char *http_server_reason_phrase(int status) {
  switch (status) {
  case 200:
    return "OK";
  case 201:
    return "Created";
  case 202:
    return "Accepted";
  case 204:
    return "No Content";
  case 205:
    return "Reset Content";
  case 301:
    return "Moved Permanently";
  case 302:
    return "Found";
  case 303:
    return "See Other";
  case 304:
    return "Not Modified";
  case 307:
    return "Temporary Redirect";
  case 308:
    return "Permanent Redirect";
  case 400:
    return "Bad Request";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 413:
    return "Payload Too Large";
  case 417:
    return "Expectation Failed";
  case 431:
    return "Request Header Fields Too Large";
  case 500:
    return "Internal Server Error";
  case 501:
    return "Not Implemented";
  case 503:
    return "Service Unavailable";
  default:
    return "";
  }
}

bool http_parse_u64_decimal(const std::string &text, std::uint64_t *out) {
  if (text.empty() || out == nullptr) {
    return false;
  }
  std::uint64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
      return false;
    }
    value = value * 10U + digit;
  }
  *out = value;
  return true;
}

std::optional<std::uint64_t>
http_server_content_length(const http::HttpHeaders &headers,
                           std::string *message) {
  const std::vector<std::string> values = headers.all("content-length");
  if (values.empty()) {
    return 0;
  }
  std::optional<std::uint64_t> expected;
  for (const std::string &value : values) {
    std::uint64_t parsed = 0;
    if (!http_parse_u64_decimal(value, &parsed)) {
      if (message != nullptr) {
        *message = "invalid Content-Length";
      }
      return std::nullopt;
    }
    if (expected.has_value() && *expected != parsed) {
      if (message != nullptr) {
        *message = "conflicting Content-Length values";
      }
      return std::nullopt;
    }
    expected = parsed;
  }
  return expected.value_or(0);
}

bool http_server_header_has_token(const http::HttpHeaders &headers,
                                  const std::string &name,
                                  const std::string &token) {
  for (std::string value : headers.all(name)) {
    value = http::ascii_lower_copy(std::move(value));
    std::size_t start = 0;
    while (start <= value.size()) {
      const std::size_t comma = value.find(',', start);
      std::size_t end = comma == std::string::npos ? value.size() : comma;
      while (start < end && (value[start] == ' ' || value[start] == '\t')) {
        ++start;
      }
      while (end > start &&
             (value[end - 1U] == ' ' || value[end - 1U] == '\t')) {
        --end;
      }
      if (value.substr(start, end - start) == token) {
        return true;
      }
      if (comma == std::string::npos) {
        break;
      }
      start = comma + 1U;
    }
  }
  return false;
}

void http_server_apply_connection_headers(RuntimeHttpServerResponse *response,
                                          int request_minor_version,
                                          bool close_connection) {
  std::string error;
  if (close_connection) {
    (void)response->headers.set("connection", "close", &error);
  } else if (request_minor_version == 0) {
    (void)response->headers.set("connection", "keep-alive", &error);
  }
}

bool http_server_response_requests_close(
    const RuntimeHttpServerResponse &response) {
  return http_server_header_has_token(response.headers, "connection", "close");
}

std::string http_server_response_wire(RuntimeHttpServerResponse response,
                                      const std::string &request_method,
                                      int request_minor_version,
                                      bool close_connection) {
  if (response.status >= 100 && response.status < 200) {
    throw RuntimeTaskFailure(
        "ArgumentError",
        "informational response cannot be a final server response");
  }
  const bool head = request_method == "HEAD";
  const bool contentless = response.status == 204 || response.status == 205 ||
                           response.status == 304;
  const std::size_t representation_size = response.body.size();
  if (contentless && representation_size != 0U) {
    throw RuntimeTaskFailure("ArgumentError",
                             "response status does not permit content");
  }
  if (response.headers.contains("transfer-encoding") ||
      response.headers.contains("trailer")) {
    throw RuntimeTaskFailure(
        "InvalidHeaderError",
        "buffered response owns Transfer-Encoding and Trailer");
  }

  std::string header_error;
  if (response.status == 204) {
    response.headers.remove("content-length");
  } else if (response.status == 205) {
    const std::optional<std::uint64_t> length =
        http_server_content_length(response.headers, &header_error);
    if (!length.has_value() || *length != 0U) {
      throw RuntimeTaskFailure("InvalidHeaderError",
                               "205 Content-Length must be zero");
    }
    if (!response.headers.contains("content-length")) {
      (void)response.headers.set("content-length", "0", &header_error);
    }
  } else if (response.status != 304) {
    const std::optional<std::uint64_t> length =
        http_server_content_length(response.headers, &header_error);
    if (!length.has_value() ||
        (!head && response.headers.contains("content-length") &&
         *length != representation_size)) {
      throw RuntimeTaskFailure(
          "InvalidHeaderError",
          "Content-Length does not match buffered response body");
    }
    if (!response.headers.contains("content-length")) {
      (void)response.headers.set("content-length",
                                 std::to_string(representation_size),
                                 &header_error);
    }
  }
  http_server_apply_connection_headers(&response, request_minor_version,
                                       close_connection);
  const std::string body =
      (head || contentless) ? std::string{} : response.body;
  const std::string reason = response.reason.empty()
                                 ? http_server_reason_phrase(response.status)
                                 : response.reason;
  std::string wire =
      std::string(request_minor_version == 0 ? "HTTP/1.0 " : "HTTP/1.1 ") +
      std::to_string(response.status);
  if (!reason.empty()) {
    wire += " ";
    wire += reason;
  }
  wire += "\r\n";
  for (const auto &entry : response.headers.pairs()) {
    wire += entry.first;
    wire += ": ";
    wire += entry.second;
    wire += "\r\n";
  }
  wire += "\r\n";
  wire += body;
  return wire;
}

bool write_http_server_response(
    const std::shared_ptr<RuntimeTcpStream> &stream,
    RuntimeHttpServerResponse response, std::chrono::milliseconds timeout,
    const std::string &request_method = "GET",
    int request_minor_version = 1, bool close_connection = true) {
  if (stream == nullptr) {
    return false;
  }
  const std::string wire =
      http_server_response_wire(std::move(response), request_method,
                                request_minor_version, close_connection);
  return stream->write_all(wire, timeout).ok;
}

RuntimeHttpServerResponse http_server_error_response(int status,
                                                     std::string message) {
  RuntimeHttpServerResponse response;
  response.status = status;
  response.reason = http_server_reason_phrase(status);
  response.body = std::move(message);
  std::string error;
  (void)response.headers.set("content-type", "text/plain; charset=utf-8",
                             &error);
  return response;
}

struct RuntimeHttpServerReadResult {
  bool ok = false;
  bool eof = false;
  bool timed_out = false;
  bool parked = false;
  int error_status = 400;
  std::string message;
  std::shared_ptr<RuntimeHttpServerRequest> request;
};

RuntimeHttpServerReadResult read_http_server_request(
    const std::shared_ptr<RuntimeHttpServer> &server,
    const std::shared_ptr<RuntimeTcpStream> &stream, std::string &bytes,
    std::optional<std::chrono::steady_clock::time_point> deadline) {
  RuntimeHttpServerReadResult result;
  if (server == nullptr || stream == nullptr) {
    result.message = "invalid server stream";
    return result;
  }

  std::size_t header_end = std::string::npos;
  while ((header_end = bytes.find("\r\n\r\n")) == std::string::npos) {
    if (bytes.size() > server->max_header_bytes) {
      result.error_status = 431;
      result.message = "request headers exceed limit";
      return result;
    }
    RuntimeByteBuffer buffer(4096);
    const auto now = std::chrono::steady_clock::now();
    const std::chrono::milliseconds remaining =
        !deadline.has_value()
            ? std::chrono::milliseconds::max()
            : now >= *deadline
                  ? std::chrono::milliseconds(0)
                  : std::chrono::duration_cast<std::chrono::milliseconds>(
                        *deadline - now);
    RuntimeIoStatus read;
    {
      NativeHttpIoParkGuard park_guard;
      read = stream->read(buffer, remaining);
    }
    if (read.park) {
      result.parked = true;
      return result;
    }
    if (read.eof) {
      result.eof = bytes.empty();
      result.message = "connection closed before request headers";
      return result;
    }
    if (!read.ok) {
      result.timed_out = read.timed_out;
      result.message =
          read.message.empty() ? "failed to read request" : read.message;
      return result;
    }
    bytes += buffer.bytes();
  }
  if (header_end > server->max_header_bytes) {
    result.error_status = 431;
    result.message = "request headers exceed limit";
    return result;
  }

  const std::string head = bytes.substr(0, header_end);
  const std::size_t first_line_end = head.find("\r\n");
  const std::string request_line = first_line_end == std::string::npos
                                       ? head
                                       : head.substr(0, first_line_end);
  const std::size_t first_space = request_line.find(' ');
  const std::size_t second_space =
      first_space == std::string::npos
          ? std::string::npos
          : request_line.find(' ', first_space + 1U);
  if (first_space == std::string::npos || second_space == std::string::npos ||
      second_space + 1U >= request_line.size()) {
    result.message = "invalid HTTP request line";
    return result;
  }

  std::string method;
  if (!http::http_normalize_method(request_line.substr(0, first_space),
                                   &method)) {
    result.message = "invalid HTTP method";
    return result;
  }
  const std::string target =
      request_line.substr(first_space + 1U, second_space - first_space - 1U);
  const std::string version = request_line.substr(second_space + 1U);
  if (target.empty() || (version != "HTTP/1.0" && version != "HTTP/1.1")) {
    result.message = "invalid HTTP request line";
    return result;
  }

  http::HttpHeaders headers;
  std::size_t line_start =
      first_line_end == std::string::npos ? head.size() : first_line_end + 2U;
  while (line_start < head.size()) {
    const std::size_t line_end = head.find("\r\n", line_start);
    const std::string line =
        line_end == std::string::npos
            ? head.substr(line_start)
            : head.substr(line_start, line_end - line_start);
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) {
      result.message = "malformed HTTP header";
      return result;
    }
    std::string error;
    if (!headers.add(line.substr(0, colon), line.substr(colon + 1U), &error)) {
      result.message = error;
      return result;
    }
    if (line_end == std::string::npos) {
      break;
    }
    line_start = line_end + 2U;
  }

  const std::vector<std::string> hosts = headers.all("host");
  if (hosts.size() > 1U || (version == "HTTP/1.1" && hosts.size() != 1U) ||
      (!hosts.empty() && hosts[0].empty())) {
    result.message = "HTTP/1.1 requires exactly one non-empty Host";
    return result;
  }
  const std::optional<std::string> query_content_type =
      headers.first("content-type");
  if (method == "QUERY" &&
      (!query_content_type.has_value() || query_content_type->empty())) {
    result.message = "QUERY request requires Content-Type";
    return result;
  }

  const bool has_transfer_encoding = headers.contains("transfer-encoding");
  const bool has_content_length = headers.contains("content-length");
  if (has_transfer_encoding && has_content_length) {
    result.message =
        "Transfer-Encoding and Content-Length cannot be combined";
    return result;
  }

  RuntimeHttpServerRequestFraming framing =
      RuntimeHttpServerRequestFraming::Empty;
  std::uint64_t content_length_value = 0;
  if (has_transfer_encoding) {
    std::vector<std::string> codings;
    for (const std::string &line : headers.all("transfer-encoding")) {
      std::size_t start = 0;
      while (start <= line.size()) {
        const std::size_t comma = line.find(',', start);
        std::size_t end = comma == std::string::npos ? line.size() : comma;
        while (start < end && (line[start] == ' ' || line[start] == '\t')) {
          ++start;
        }
        while (end > start &&
               (line[end - 1U] == ' ' || line[end - 1U] == '\t')) {
          --end;
        }
        if (start == end) {
          result.message = "invalid Transfer-Encoding";
          return result;
        }
        codings.push_back(
            http::ascii_lower_copy(line.substr(start, end - start)));
        if (comma == std::string::npos) {
          break;
        }
        start = comma + 1U;
      }
    }
    if (codings.size() != 1U || codings[0] != "chunked") {
      result.error_status = 501;
      result.message =
          "only request Transfer-Encoding: chunked is supported";
      return result;
    }
    framing = RuntimeHttpServerRequestFraming::Chunked;
  } else if (has_content_length) {
    std::string length_error;
    const std::optional<std::uint64_t> content_length =
        http_server_content_length(headers, &length_error);
    if (!content_length.has_value()) {
      result.message = length_error;
      return result;
    }
    if (*content_length > server->max_body_bytes) {
      result.error_status = 413;
      result.message = "request body exceeds limit";
      return result;
    }
    content_length_value = *content_length;
    if (content_length_value > 0) {
      framing = RuntimeHttpServerRequestFraming::ContentLength;
    }
  }

  auto body = std::make_shared<RuntimeHttpServerRequestBody>();
  body->stream = stream;
  body->task = server->task;
  body->framing = framing;
  body->buffered = bytes.substr(header_end + 4U);
  bytes.clear();
  body->content_remaining = content_length_value;
  body->max_body_bytes = server->max_body_bytes;
  body->max_header_bytes = server->max_header_bytes;
  body->read_timeout = server->read_timeout;
  body->closed = framing == RuntimeHttpServerRequestFraming::Empty;
  if (headers.contains("expect")) {
    const std::vector<std::string> expectations = headers.all("expect");
    if (expectations.size() != 1U ||
        http::ascii_lower_copy(expectations[0]) != "100-continue") {
      result.error_status = 417;
      result.message = "unsupported HTTP expectation";
      return result;
    }
    body->expect_continue = !body->closed;
  }
  if (body->closed) {
    body->chunk_state = RuntimeHttpServerRequestBody::ChunkState::Complete;
  }

  auto request = std::make_shared<RuntimeHttpServerRequest>();
  request->method = std::move(method);
  request->target = target;
  const std::size_t query_pos = target.find('?');
  request->path =
      query_pos == std::string::npos ? target : target.substr(0, query_pos);
  request->query = query_pos == std::string::npos
                       ? std::string{}
                       : target.substr(query_pos + 1U);
  request->headers = std::move(headers);
  request->body_stream = std::move(body);
  request->local_endpoint = stream->local_endpoint();
  request->remote_endpoint = stream->remote_endpoint();
  request->minor_version = version == "HTTP/1.0" ? 0 : 1;
  request->keep_alive =
      !http_server_header_has_token(request->headers, "connection", "close") &&
      (request->minor_version == 1 ||
       http_server_header_has_token(request->headers, "connection",
                                    "keep-alive"));
  result.ok = true;
  result.request = std::move(request);
  return result;
}

std::string http_server_streaming_response_head(
    RuntimeHttpServerResponse response, int request_minor_version,
    bool close_connection) {
  if ((response.status >= 100 && response.status < 200) ||
      response.status == 204 || response.status == 205 ||
      response.status == 304) {
    throw RuntimeTaskFailure(
        "ArgumentError", "streaming response status must permit content");
  }
  std::string error;
  if (!response.headers.set("transfer-encoding", "chunked", &error)) {
    throw RuntimeTaskFailure("InvalidHeaderError", error);
  }
  if (!response.trailer_names.empty()) {
    std::string names;
    for (const std::string &name : response.trailer_names) {
      if (!names.empty()) {
        names += ", ";
      }
      names += name;
    }
    if (!response.headers.set("trailer", names, &error)) {
      throw RuntimeTaskFailure("InvalidHeaderError", error);
    }
  }
  http_server_apply_connection_headers(&response, request_minor_version,
                                       close_connection);
  const std::string reason = response.reason.empty()
                                 ? http_server_reason_phrase(response.status)
                                 : response.reason;
  std::string wire =
      std::string(request_minor_version == 0 ? "HTTP/1.0 " : "HTTP/1.1 ") +
      std::to_string(response.status);
  if (!reason.empty()) {
    wire += " ";
    wire += reason;
  }
  wire += "\r\n";
  for (const auto &entry : response.headers.pairs()) {
    wire += entry.first;
    wire += ": ";
    wire += entry.second;
    wire += "\r\n";
  }
  wire += "\r\n";
  return wire;
}

bool http_server_finish_streaming_response(
    const std::shared_ptr<RuntimeHttpServerResponseWriter> &writer) {
  if (writer == nullptr || writer->stream == nullptr || writer->finished) {
    return writer != nullptr && writer->finished;
  }
  if (writer->pending == RuntimeHttpServerResponseWriter::Pending::Head) {
    const RuntimeIoStatus head = writer->stream->write_all(
        writer->pending_bytes.substr(writer->pending_offset),
        writer->write_timeout);
    if (!head.ok) {
      return false;
    }
    writer->pending = RuntimeHttpServerResponseWriter::Pending::None;
    writer->pending_bytes.clear();
    writer->pending_offset = 0;
    writer->head_sent = true;
    if (writer->final_response_started != nullptr) {
      writer->final_response_started->store(true, std::memory_order_release);
    }
  } else if (writer->pending !=
             RuntimeHttpServerResponseWriter::Pending::None) {
    return false;
  }
  if (!writer->body_closed) {
    const RuntimeIoStatus closed =
        writer->stream->write_all("0\r\n", writer->write_timeout);
    if (!closed.ok) {
      return false;
    }
    writer->body_closed = true;
  }
  const RuntimeIoStatus finished =
      writer->stream->write_all("\r\n", writer->write_timeout);
  writer->finished = finished.ok;
  return finished.ok;
}

RuntimeHttpServerWriterResult flush_http_server_writer(
    const std::shared_ptr<RuntimeHttpServerResponseWriter> &writer) {
  RuntimeHttpServerWriterResult result;
  if (writer == nullptr || writer->stream == nullptr) {
    result.error_name = "ClosedResourceError";
    result.message = "streaming response writer is closed";
    return result;
  }
  while (writer->pending_offset < writer->pending_bytes.size()) {
    if (!writer->write_deadline.has_value() &&
        writer->write_timeout != std::chrono::milliseconds::max()) {
      writer->write_deadline =
          std::chrono::steady_clock::now() + writer->write_timeout;
    }
    std::chrono::milliseconds remaining = std::chrono::milliseconds::max();
    if (writer->write_deadline.has_value()) {
      const auto now = std::chrono::steady_clock::now();
      remaining =
          now >= *writer->write_deadline
              ? std::chrono::milliseconds(0)
              : std::chrono::duration_cast<std::chrono::milliseconds>(
                    *writer->write_deadline - now);
    }
    const RuntimeIoStatus written = writer->stream->write_all(
        writer->pending_bytes.substr(writer->pending_offset), remaining);
    writer->write_deadline.reset();
    if (!written.ok) {
      result.error_name =
          written.error_name.empty() ? "IOError" : written.error_name;
      result.message = written.message.empty()
                           ? "streaming response write failed"
                           : written.message;
      return result;
    }
    writer->pending_offset = writer->pending_bytes.size();
  }

  const RuntimeHttpServerResponseWriter::Pending completed = writer->pending;
  writer->pending = RuntimeHttpServerResponseWriter::Pending::None;
  writer->pending_bytes.clear();
  writer->pending_offset = 0;
  if (completed == RuntimeHttpServerResponseWriter::Pending::Head) {
    writer->head_sent = true;
    if (writer->final_response_started != nullptr) {
      writer->final_response_started->store(true, std::memory_order_release);
    }
  } else if (completed ==
             RuntimeHttpServerResponseWriter::Pending::Close) {
    writer->body_closed = true;
  } else if (completed ==
             RuntimeHttpServerResponseWriter::Pending::Finish) {
    writer->finished = true;
  }
  result.ok = true;
  return result;
}

enum class NativeHttpBodyReadStatus { Chunk, Eof, Parked, Faulted };

void set_body_read_error(RuntimeHttpServerBodyReadResult *result,
                         std::string name, std::string message) {
  result->error_name = std::move(name);
  result->message = std::move(message);
}

NativeHttpBodyReadStatus read_http_server_body_more(
    const std::shared_ptr<RuntimeHttpServerRequestBody> &body,
    RuntimeHttpServerBodyReadResult *result) {
  if (body == nullptr || body->stream == nullptr) {
    set_body_read_error(result, "ClosedResourceError",
                        "request body stream is closed");
    return NativeHttpBodyReadStatus::Faulted;
  }
  if (!body->read_deadline.has_value() &&
      body->read_timeout != std::chrono::milliseconds::max()) {
    body->read_deadline =
        std::chrono::steady_clock::now() + body->read_timeout;
  }
  std::chrono::milliseconds remaining = std::chrono::milliseconds::max();
  if (body->read_deadline.has_value()) {
    const auto now = std::chrono::steady_clock::now();
    remaining = now >= *body->read_deadline
                    ? std::chrono::milliseconds(0)
                    : std::chrono::duration_cast<std::chrono::milliseconds>(
                          *body->read_deadline - now);
  }
  RuntimeByteBuffer buffer(4096);
  RuntimeIoStatus read;
  {
    NativeHttpIoBlockingGuard blocking_guard;
    read = body->stream->read(buffer, remaining);
  }
  if (read.park) {
    set_body_read_error(result, "RuntimeError",
                        "native HTTP body read unexpectedly requested park");
    return NativeHttpBodyReadStatus::Faulted;
  }
  body->read_deadline.reset();
  if (read.eof) {
    set_body_read_error(result, "UnexpectedEofError",
                        "connection closed before request body completed");
    return NativeHttpBodyReadStatus::Faulted;
  }
  if (!read.ok) {
    set_body_read_error(result,
                        read.error_name.empty() ? "IOError"
                                                : read.error_name,
                        read.message.empty() ? "request body read failed"
                                             : read.message);
    return NativeHttpBodyReadStatus::Faulted;
  }
  if (read.count == 0) {
    set_body_read_error(result, "IOError",
                        "request body reader made no progress");
    return NativeHttpBodyReadStatus::Faulted;
  }
  body->buffered += buffer.bytes();
  return NativeHttpBodyReadStatus::Chunk;
}

bool take_http_server_crlf_line(
    const std::shared_ptr<RuntimeHttpServerRequestBody> &body,
    std::string *line, bool *need_more,
    RuntimeHttpServerBodyReadResult *result) {
  *need_more = false;
  const std::size_t lf = body->buffered.find('\n');
  if (lf == std::string::npos) {
    if (body->buffered.size() > body->max_header_bytes) {
      set_body_read_error(result, "HeaderLimitError",
                          "request chunk line exceeds limit");
      return false;
    }
    *need_more = true;
    return true;
  }
  if (lf == 0 || body->buffered[lf - 1U] != '\r' ||
      body->buffered.find('\r') != lf - 1U) {
    set_body_read_error(result, "ChunkError",
                        "request chunk uses malformed CRLF");
    return false;
  }
  *line = body->buffered.substr(0, lf - 1U);
  body->buffered.erase(0, lf + 1U);
  return true;
}

bool forbidden_http_trailer_name(const std::string &name) {
  const std::string lower = http::ascii_lower_copy(name);
  return lower == "content-length" || lower == "transfer-encoding" ||
         lower == "trailer" || lower == "host" ||
         lower == "connection" || lower == "keep-alive" ||
         lower == "upgrade" || lower == "authorization" ||
         lower == "proxy-authorization" || lower == "content-type" ||
         lower == "content-encoding" || lower == "content-range";
}

NativeHttpBodyReadStatus read_http_server_chunked_body(
    const std::shared_ptr<RuntimeHttpServerRequestBody> &body,
    std::size_t max_part, RuntimeHttpServerBodyReadResult *result,
    std::shared_ptr<RuntimeHttpServerRequestChunk> *out) {
  while (true) {
    if (body->chunk_state ==
        RuntimeHttpServerRequestBody::ChunkState::Size) {
      std::string line;
      bool need_more = false;
      if (!take_http_server_crlf_line(body, &line, &need_more, result)) {
        return NativeHttpBodyReadStatus::Faulted;
      }
      if (need_more) {
        const NativeHttpBodyReadStatus status =
            read_http_server_body_more(body, result);
        if (status != NativeHttpBodyReadStatus::Chunk) {
          return status;
        }
        continue;
      }
      const std::size_t semi = line.find(';');
      const std::string size_text = line.substr(0, semi);
      if (size_text.empty()) {
        set_body_read_error(result, "ChunkError",
                            "request chunk size is missing");
        return NativeHttpBodyReadStatus::Faulted;
      }
      std::uint64_t size = 0;
      for (const char c : size_text) {
        int digit = -1;
        if (c >= '0' && c <= '9') {
          digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
          digit = 10 + c - 'a';
        } else if (c >= 'A' && c <= 'F') {
          digit = 10 + c - 'A';
        }
        if (digit < 0 ||
            size > (std::numeric_limits<std::uint64_t>::max() >> 4U)) {
          set_body_read_error(result, "ChunkError",
                              "invalid request chunk size");
          return NativeHttpBodyReadStatus::Faulted;
        }
        size = (size << 4U) | static_cast<std::uint64_t>(digit);
      }
      std::string extension_error;
      if (!http::http_parse_chunk_extensions(
              semi == std::string::npos ? std::string{} : line.substr(semi),
              &body->current_extensions, &extension_error,
              /*max_count=*/128, body->max_header_bytes)) {
        set_body_read_error(result, "ChunkError", extension_error);
        return NativeHttpBodyReadStatus::Faulted;
      }
      if (size == 0) {
        body->chunk_state =
            RuntimeHttpServerRequestBody::ChunkState::Trailers;
        continue;
      }
      const std::uint64_t remaining_limit =
          body->decoded_bytes >= body->max_body_bytes
              ? 0
              : static_cast<std::uint64_t>(body->max_body_bytes) -
                    body->decoded_bytes;
      if (size > remaining_limit ||
          (out != nullptr && size > max_part)) {
        set_body_read_error(result, "BodyLimitError",
                            "request body exceeds limit");
        return NativeHttpBodyReadStatus::Faulted;
      }
      body->chunk_remaining = size;
      body->chunk_state = RuntimeHttpServerRequestBody::ChunkState::Data;
    }

    if (body->chunk_state ==
        RuntimeHttpServerRequestBody::ChunkState::Data) {
      const std::uint64_t needed = body->chunk_remaining + 2U;
      while (body->buffered.size() < needed) {
        const NativeHttpBodyReadStatus status =
            read_http_server_body_more(body, result);
        if (status != NativeHttpBodyReadStatus::Chunk) {
          return status;
        }
      }
      const std::size_t count =
          static_cast<std::size_t>(body->chunk_remaining);
      if (body->buffered[count] != '\r' ||
          body->buffered[count + 1U] != '\n') {
        set_body_read_error(
            result, "ChunkError",
            "request chunk data is not followed by CRLF");
        return NativeHttpBodyReadStatus::Faulted;
      }
      if (out == nullptr) {
        body->whole_body += body->buffered.substr(0, count);
      } else {
        auto chunk = std::make_shared<RuntimeHttpServerRequestChunk>();
        chunk->data = body->buffered.substr(0, count);
        chunk->extensions = std::move(body->current_extensions);
        *out = std::move(chunk);
      }
      body->buffered.erase(0, count + 2U);
      body->decoded_bytes += count;
      body->chunk_remaining = 0;
      body->current_extensions.clear();
      body->chunk_state = RuntimeHttpServerRequestBody::ChunkState::Size;
      return NativeHttpBodyReadStatus::Chunk;
    }

    if (body->chunk_state ==
        RuntimeHttpServerRequestBody::ChunkState::Trailers) {
      std::string line;
      bool need_more = false;
      if (!take_http_server_crlf_line(body, &line, &need_more, result)) {
        return NativeHttpBodyReadStatus::Faulted;
      }
      if (need_more) {
        const NativeHttpBodyReadStatus status =
            read_http_server_body_more(body, result);
        if (status != NativeHttpBodyReadStatus::Chunk) {
          return status;
        }
        continue;
      }
      body->trailer_bytes += line.size() + 2U;
      if (body->trailer_bytes > body->max_header_bytes) {
        set_body_read_error(result, "HeaderLimitError",
                            "request trailers exceed limit");
        return NativeHttpBodyReadStatus::Faulted;
      }
      if (line.empty()) {
        body->closed = true;
        body->chunk_state =
            RuntimeHttpServerRequestBody::ChunkState::Complete;
        return NativeHttpBodyReadStatus::Eof;
      }
      const std::size_t colon = line.find(':');
      if (colon == std::string::npos) {
        set_body_read_error(result, "ChunkError",
                            "malformed request trailer");
        return NativeHttpBodyReadStatus::Faulted;
      }
      const std::string trailer_name = line.substr(0, colon);
      if (forbidden_http_trailer_name(trailer_name)) {
        set_body_read_error(
            result, "ChunkError",
            "field is forbidden in request trailers: " +
                http::ascii_lower_copy(trailer_name));
        return NativeHttpBodyReadStatus::Faulted;
      }
      std::string error;
      if (!body->trailers.add(trailer_name, line.substr(colon + 1U),
                              &error)) {
        set_body_read_error(result, "ChunkError", error);
        return NativeHttpBodyReadStatus::Faulted;
      }
      continue;
    }
    return NativeHttpBodyReadStatus::Eof;
  }
}

} // namespace

const char *runtime_http_server_reason_phrase(int status) {
  return http_server_reason_phrase(status);
}

RuntimeHttpServerOpenResult
runtime_http_server_open(RuntimeHttpServerOptions options) {
  RuntimeHttpServerOpenResult result;
  if (options.workers == 0 || options.max_concurrent_per_worker == 0 ||
      options.max_header_bytes == 0 ||
      options.max_requests_per_connection == 0 || options.backlog <= 0) {
    result.error_name = "ArgumentError";
    result.message = "HTTP server limits must be positive";
    return result;
  }

  std::shared_ptr<RuntimeTlsContext> tls_context;
  if (!options.tls.cert_file.empty() || !options.tls.key_file.empty()) {
    RuntimeIoStatus status;
    tls_context = RuntimeTlsContext::create(options.tls, true, &status);
    if (!status.ok) {
      static_cast<RuntimeIoStatus &>(result) = std::move(status);
      return result;
    }
  }
  auto server = std::make_shared<RuntimeHttpServer>();
  server->host = std::move(options.host);
  server->port = options.port;
  server->workers = options.workers;
  server->max_concurrent_per_worker = options.max_concurrent_per_worker;
  server->max_header_bytes = options.max_header_bytes;
  server->max_body_bytes = options.max_body_bytes;
  server->read_timeout = options.read_timeout;
  server->write_timeout = options.write_timeout;
  server->idle_timeout = options.idle_timeout;
  server->max_requests_per_connection =
      options.max_requests_per_connection;

  RuntimeTcpListenResult listening = RuntimeTcpListener::listen(
      RuntimeEndpoint{server->host, server->port}, options.backlog,
      options.reuse_addr, RuntimeIsolationMode::Unchecked);
  if (!listening.ok) {
    static_cast<RuntimeIoStatus &>(result) =
        static_cast<RuntimeIoStatus &&>(std::move(listening));
    return result;
  }

  server->listener = std::move(listening.listener);
  server->listener->set_tls_context(std::move(tls_context));
  server->port = server->listener->local_endpoint().port;
  // Cooperative server IO multiplexes admitted connections; the scheduler's
  // OS-thread count is independent from the hard connection capacity.
  server->task = std::make_shared<RuntimeTaskModule>(
      RuntimeSchedulerConfig{server->workers, 1});
  result.ok = true;
  result.server = std::move(server);
  return result;
}

std::optional<RuntimeNativeHttpServerRequestSnapshot>
runtime_native_http_server_request_snapshot(const Value &value) {
  if (!value.is_io_value()) {
    return std::nullopt;
  }
  const auto request =
      std::dynamic_pointer_cast<RuntimeHttpServerRequest>(value.as_io_value());
  if (request == nullptr) {
    return std::nullopt;
  }

  RuntimeNativeHttpServerRequestSnapshot snapshot;
  snapshot.method = request->method;
  snapshot.target = request->target;
  snapshot.path = request->path;
  if (!request->query.empty()) {
    snapshot.query = request->query;
  }
  snapshot.header_pairs = request->headers.pairs();

  auto headers = std::make_shared<RuntimeHttpHeaders>();
  headers->headers = request->headers;
  headers->read_only = true;
  snapshot.headers = Value::io_value(std::move(headers));
  snapshot.body_stream = Value::io_value(request->body_stream);
  snapshot.local_endpoint = Value::io_value(
      std::make_shared<RuntimeEndpoint>(request->local_endpoint));
  snapshot.remote_endpoint = Value::io_value(
      std::make_shared<RuntimeEndpoint>(request->remote_endpoint));
  return snapshot;
}

RuntimeHttpServerBodyReadResult runtime_http_server_read_body_all(
    const std::shared_ptr<RuntimeHttpServerRequestBody> &body) {
  RuntimeHttpServerBodyReadResult result;
  if (body == nullptr) {
    result.error_name = "ClosedResourceError";
    result.message = "request body stream is closed";
    return result;
  }
  if (body->consumption ==
      RuntimeHttpServerRequestBody::Consumption::Chunks) {
    result.error_name = "BodyConsumedError";
    result.message =
        "request body is already being consumed chunk by chunk";
    return result;
  }
  body->consumption = RuntimeHttpServerRequestBody::Consumption::Whole;
  if (body->whole_complete) {
    result.ok = true;
    result.body = body->whole_body;
    return result;
  }
  if (body->expect_continue && !body->continue_sent &&
      !body->final_response_started->load(std::memory_order_acquire)) {
    const RuntimeIoStatus continued = body->stream->write_all(
        "HTTP/1.1 100 Continue\r\n\r\n", body->read_timeout);
    if (!continued.ok) {
      result.error_name = continued.error_name.empty()
                              ? "IOError"
                              : continued.error_name;
      result.message = continued.message.empty()
                           ? "failed to write 100 Continue"
                           : continued.message;
      return result;
    }
    body->continue_sent = true;
  }

  while (!body->closed) {
    if (body->framing == RuntimeHttpServerRequestFraming::Empty) {
      body->closed = true;
      break;
    }
    if (body->framing ==
        RuntimeHttpServerRequestFraming::ContentLength) {
      while (body->buffered.empty()) {
        const NativeHttpBodyReadStatus status =
            read_http_server_body_more(body, &result);
        if (status == NativeHttpBodyReadStatus::Parked ||
            status == NativeHttpBodyReadStatus::Faulted) {
          return result;
        }
      }
      const std::size_t take =
          static_cast<std::size_t>(std::min<std::uint64_t>(
              body->content_remaining, body->buffered.size()));
      body->whole_body += body->buffered.substr(0, take);
      body->buffered.erase(0, take);
      body->content_remaining -= take;
      body->decoded_bytes += take;
      if (body->content_remaining == 0) {
        body->closed = true;
        body->chunk_state =
            RuntimeHttpServerRequestBody::ChunkState::Complete;
      }
      continue;
    }

    const NativeHttpBodyReadStatus status =
        read_http_server_chunked_body(
            body, std::numeric_limits<std::size_t>::max(), &result, nullptr);
    if (status == NativeHttpBodyReadStatus::Parked ||
        status == NativeHttpBodyReadStatus::Faulted) {
      return result;
    }
    if (status == NativeHttpBodyReadStatus::Eof) {
      break;
    }
  }

  body->whole_complete = true;
  result.ok = true;
  result.body = body->whole_body;
  return result;
}

RuntimeHttpServerBodyChunkReadResult runtime_http_server_read_body_chunk(
    const std::shared_ptr<RuntimeHttpServerRequestBody> &body,
    std::size_t max_bytes) {
  RuntimeHttpServerBodyChunkReadResult result;
  if (body == nullptr) {
    result.error_name = "ClosedResourceError";
    result.message = "request body stream is closed";
    return result;
  }
  if (max_bytes == 0) {
    result.error_name = "ArgumentError";
    result.message = "max_bytes must be positive Int";
    return result;
  }
  if (body->consumption ==
      RuntimeHttpServerRequestBody::Consumption::Whole) {
    result.error_name = "BodyConsumedError";
    result.message =
        "request body is already being consumed as a whole";
    return result;
  }
  body->consumption = RuntimeHttpServerRequestBody::Consumption::Chunks;
  if (body->closed) {
    result.ok = true;
    return result;
  }
  if (body->expect_continue && !body->continue_sent &&
      !body->final_response_started->load(std::memory_order_acquire)) {
    const RuntimeIoStatus continued = body->stream->write_all(
        "HTTP/1.1 100 Continue\r\n\r\n", body->read_timeout);
    if (!continued.ok) {
      result.error_name =
          continued.error_name.empty() ? "IOError" : continued.error_name;
      result.message = continued.message.empty()
                           ? "failed to write 100 Continue"
                           : continued.message;
      return result;
    }
    body->continue_sent = true;
  }
  if (body->framing == RuntimeHttpServerRequestFraming::Empty) {
    body->closed = true;
    result.ok = true;
    return result;
  }

  NativeHttpBodyReadStatus status = NativeHttpBodyReadStatus::Faulted;
  if (body->framing ==
      RuntimeHttpServerRequestFraming::ContentLength) {
    while (body->buffered.empty()) {
      status = read_http_server_body_more(body, &result);
      if (status != NativeHttpBodyReadStatus::Chunk) {
        return result;
      }
    }
    const std::size_t take =
        static_cast<std::size_t>(std::min<std::uint64_t>(
            body->content_remaining,
            std::min<std::uint64_t>(body->buffered.size(), max_bytes)));
    auto chunk = std::make_shared<RuntimeHttpServerRequestChunk>();
    chunk->data = body->buffered.substr(0, take);
    body->buffered.erase(0, take);
    body->content_remaining -= take;
    body->decoded_bytes += take;
    if (body->content_remaining == 0) {
      body->closed = true;
      body->chunk_state =
          RuntimeHttpServerRequestBody::ChunkState::Complete;
    }
    result.chunk = std::move(chunk);
    result.ok = true;
    return result;
  }

  status = read_http_server_chunked_body(
      body, max_bytes, &result, &result.chunk);
  if (status == NativeHttpBodyReadStatus::Parked ||
      status == NativeHttpBodyReadStatus::Faulted) {
    return result;
  }
  result.ok = true;
  return result;
}

RuntimeHttpServerWriterResult runtime_http_server_writer_write(
    const std::shared_ptr<RuntimeHttpServerResponseWriter> &writer,
    std::string data, std::vector<http::HttpChunkExtension> extensions) {
  const RuntimeHttpServerResponseWriter::Pending pending =
      writer == nullptr
          ? RuntimeHttpServerResponseWriter::Pending::None
          : writer->pending;
  RuntimeHttpServerWriterResult result =
      flush_http_server_writer(writer);
  if (!result.ok || pending !=
                        RuntimeHttpServerResponseWriter::Pending::None) {
    if (result.ok &&
        pending == RuntimeHttpServerResponseWriter::Pending::Head) {
      // The original write had not yet been encoded; continue after the
      // deferred response head is committed.
    } else {
      return result;
    }
  }
  if (writer->body_closed || writer->finished) {
    result.ok = false;
    result.error_name = "RequestStateError";
    result.message = "cannot write after streaming body is closed";
    return result;
  }
  std::string error;
  if (!http::http_encode_chunk(data, extensions, &writer->pending_bytes,
                               &error)) {
    result.ok = false;
    result.error_name = "ChunkError";
    result.message = std::move(error);
    return result;
  }
  writer->pending = RuntimeHttpServerResponseWriter::Pending::Chunk;
  return flush_http_server_writer(writer);
}

RuntimeHttpServerWriterResult runtime_http_server_writer_close(
    const std::shared_ptr<RuntimeHttpServerResponseWriter> &writer) {
  const RuntimeHttpServerResponseWriter::Pending pending =
      writer == nullptr
          ? RuntimeHttpServerResponseWriter::Pending::None
          : writer->pending;
  RuntimeHttpServerWriterResult result =
      flush_http_server_writer(writer);
  if (!result.ok || (pending !=
                         RuntimeHttpServerResponseWriter::Pending::None &&
                     pending !=
                         RuntimeHttpServerResponseWriter::Pending::Head)) {
    return result;
  }
  if (writer->body_closed) {
    return result;
  }
  writer->pending_bytes = "0\r\n";
  writer->pending = RuntimeHttpServerResponseWriter::Pending::Close;
  return flush_http_server_writer(writer);
}

RuntimeHttpServerWriterResult runtime_http_server_writer_trailer(
    const std::shared_ptr<RuntimeHttpServerResponseWriter> &writer,
    std::string name, std::string value) {
  const RuntimeHttpServerResponseWriter::Pending pending =
      writer == nullptr
          ? RuntimeHttpServerResponseWriter::Pending::None
          : writer->pending;
  RuntimeHttpServerWriterResult result =
      flush_http_server_writer(writer);
  if (!result.ok || pending !=
                        RuntimeHttpServerResponseWriter::Pending::None) {
    return result;
  }
  if (!writer->body_closed || writer->finished) {
    result.ok = false;
    result.error_name = "RequestStateError";
    result.message =
        "trailers are written after close and before finish";
    return result;
  }
  if (!http::http_valid_field_name(name) ||
      !http::http_valid_field_value(value)) {
    result.ok = false;
    result.error_name = "InvalidHeaderError";
    result.message = "invalid response trailer";
    return result;
  }
  name = http::ascii_lower_copy(std::move(name));
  if (std::find(writer->declared_trailers.begin(),
                writer->declared_trailers.end(), name) ==
      writer->declared_trailers.end()) {
    result.ok = false;
    result.error_name = "InvalidHeaderError";
    result.message = "response trailer was not declared: " + name;
    return result;
  }
  writer->pending_bytes = name + ": " + value + "\r\n";
  writer->pending = RuntimeHttpServerResponseWriter::Pending::Trailer;
  return flush_http_server_writer(writer);
}

RuntimeHttpServerWriterResult runtime_http_server_writer_finish(
    const std::shared_ptr<RuntimeHttpServerResponseWriter> &writer) {
  const RuntimeHttpServerResponseWriter::Pending pending =
      writer == nullptr
          ? RuntimeHttpServerResponseWriter::Pending::None
          : writer->pending;
  RuntimeHttpServerWriterResult result =
      flush_http_server_writer(writer);
  if (!result.ok || pending !=
                        RuntimeHttpServerResponseWriter::Pending::None) {
    return result;
  }
  if (!writer->body_closed) {
    result.ok = false;
    result.error_name = "RequestStateError";
    result.message =
        "close streaming body before finishing trailers";
    return result;
  }
  if (writer->finished) {
    return result;
  }
  writer->pending_bytes = "\r\n";
  writer->pending = RuntimeHttpServerResponseWriter::Pending::Finish;
  return flush_http_server_writer(writer);
}

RuntimeHttpServerServeResult runtime_http_server_serve_native(
    const std::shared_ptr<RuntimeHttpServer> &server,
    const std::shared_ptr<RuntimeNativeBlock> &handler,
    std::optional<std::size_t> max_requests) {
  RuntimeHttpServerServeResult result;
  if (server == nullptr || server->listener == nullptr) {
    result.error_name = "ClosedResourceError";
    result.message = "net.http.Server is closed";
    return result;
  }
  if (handler == nullptr) {
    result.error_name = "TypeError";
    result.message = "net.http.Server#serve requires block";
    return result;
  }

  struct RequestTaskState {
    std::uint64_t connection_id = 0;
    std::shared_ptr<RuntimeTcpStream> stream;
    std::shared_ptr<RuntimeHttpServerRequest> request;
    std::function<Value()> producer;
    std::shared_ptr<RuntimeHttpServerResponseWriter> writer;
    std::shared_ptr<RuntimeHttpServerRequestBody> request_body;
    std::string buffered;
    std::string request_method;
    int request_minor_version = 1;
    bool request_keep_alive = false;
    bool close_after_response = true;
    bool request_in_flight = false;
    std::size_t request_count = 0;
    std::optional<std::chrono::steady_clock::time_point> header_deadline;
    bool request_started = false;
    bool headers_sent = false;
    bool released = false;
  };

  auto finish_task = [server](const std::shared_ptr<RequestTaskState> &state,
                              bool failed) {
    if (state == nullptr || state->released) {
      return;
    }
    state->released = true;
    if (state->request_in_flight) {
      state->request_in_flight = false;
      server->end_request();
    }
    state->request.reset();
    state->producer = {};
    state->writer.reset();
    state->request_body.reset();
    if (state->stream != nullptr) {
      (void)state->stream->close();
    }
    state->stream.reset();
    state->buffered.clear();
    server->release_connection(state->connection_id, failed);
  };

  std::size_t accepted_count = 0;
  while (!server->closed_server() &&
         (!max_requests.has_value() || accepted_count < *max_requests)) {
    RuntimeTcpAcceptResult accepted =
        server->listener->accept(std::chrono::milliseconds(100));
    if (accepted.timed_out) {
      continue;
    }
    if (!accepted.ok) {
      if (server->closed_server() ||
          accepted.error_name == "AlreadyClosedError") {
        break;
      }
      result.error_name =
          accepted.error_name.empty() ? "IOError" : accepted.error_name;
      result.message = accepted.message.empty()
                           ? "HTTP server accept failed"
                           : accepted.message;
      return result;
    }
    const std::optional<std::uint64_t> connection_id =
        server->try_acquire_connection(accepted.stream);
    if (!connection_id.has_value()) {
      if (accepted.stream != nullptr && server->accepting()) {
        RuntimeHttpServerResponse overloaded =
            http_server_error_response(503, "service unavailable");
        (void)write_http_server_response(
            accepted.stream, std::move(overloaded), server->write_timeout);
        server->reject_request();
        (void)accepted.stream->close();
        continue;
      }
      if (accepted.stream != nullptr) {
        (void)accepted.stream->close();
      }
      break;
    }
    ++accepted_count;

    auto state = std::make_shared<RequestTaskState>();
    state->connection_id = *connection_id;
    state->stream = std::move(accepted.stream);
    RuntimeTaskHandle request_task = server->task->spawn_resumable(
        [server, state, handler, finish_task]() mutable {
          RuntimeTaskAnnotationScope annotation("net.http.server.request");
          state->stream->adopt_to_current_owner();
          while (true) {
            if (!state->request_started) {
              const bool keepalive_read = state->request_count > 0;
              if (!state->header_deadline.has_value()) {
                const std::chrono::milliseconds timeout =
                    keepalive_read ? server->idle_timeout
                                   : server->read_timeout;
                if (timeout != std::chrono::milliseconds::max()) {
                  state->header_deadline =
                      std::chrono::steady_clock::now() + timeout;
                }
              }
              RuntimeHttpServerReadResult read = read_http_server_request(
                  server, state->stream, state->buffered,
                  state->header_deadline);
              if (read.parked) {
                if (!park_native_http_task(server->task)) {
                  finish_task(state, true);
                  throw RuntimeTaskFailure(
                      "RuntimeError",
                      "HTTP request task could not park for IO");
                }
                return Value::null();
              }
              state->header_deadline.reset();
              if (!read.ok) {
                if (!read.eof && !(keepalive_read && read.timed_out)) {
                  RuntimeHttpServerResponse response =
                      http_server_error_response(read.error_status,
                                                 read.message);
                  (void)write_http_server_response(
                      state->stream, std::move(response),
                      server->write_timeout);
                  server->reject_request();
                }
                finish_task(state, !read.eof && !read.timed_out);
                return Value::null();
              }
              state->request = std::move(read.request);
              state->request_body = state->request->body_stream;
              state->request_method = state->request->method;
              state->request_minor_version = state->request->minor_version;
              state->request_keep_alive = state->request->keep_alive;
              ++state->request_count;
              state->close_after_response =
                  !state->request_keep_alive || !server->accepting() ||
                  state->request_count >=
                      server->max_requests_per_connection;
              state->request_started = true;
              state->request_in_flight = true;
              server->begin_request(state->request_count > 1);
            }

            auto complete_request = [&]() -> bool {
              if (state->request_in_flight) {
                state->request_in_flight = false;
                server->end_request();
              }
              const bool reusable =
                  !state->close_after_response && server->accepting() &&
                  state->request_body != nullptr &&
                  state->request_body->closed;
              if (!reusable) {
                finish_task(state, false);
                return false;
              }
              state->buffered = std::move(state->request_body->buffered);
              state->request.reset();
              state->producer = {};
              state->writer.reset();
              state->request_body.reset();
              state->request_started = false;
              state->headers_sent = false;
              return true;
            };

            try {
              if (state->producer) {
                (void)state->producer();
                if (runtime_task_is_parked()) {
                  return Value::null();
                }
                if (!http_server_finish_streaming_response(state->writer)) {
                  throw RuntimeTaskFailure(
                      "ConnectionError",
                      "failed to finish streaming HTTP response");
                }
                if (!complete_request()) {
                  return Value::null();
                }
                continue;
              }

              RuntimeHttpServerResponse response;
              if (!handler->invoke_http_server(state->request, &response)) {
                throw RuntimeTaskFailure(
                    "TypeError",
                    "native HTTP handler cannot convert its response");
              }
              if (runtime_task_is_parked()) {
                return Value::null();
              }

              if (response.streaming() &&
                  state->request_method == "HEAD") {
                response.producer_factory = {};
                response.trailer_names.clear();
                response.headers.remove("transfer-encoding");
                response.headers.remove("trailer");
              }
              state->close_after_response =
                  state->close_after_response ||
                  http_server_response_requests_close(response) ||
                  state->request_body == nullptr ||
                  !state->request_body->closed;
              if (response.streaming()) {
                state->headers_sent = true;
                state->writer =
                    std::make_shared<RuntimeHttpServerResponseWriter>();
                state->writer->stream = state->stream;
                state->writer->task = server->task;
                state->writer->write_timeout = server->write_timeout;
                state->writer->declared_trailers = response.trailer_names;
                state->writer->final_response_started =
                    state->request_body->final_response_started;
                state->writer->pending_bytes =
                    http_server_streaming_response_head(
                        response, state->request_minor_version,
                        state->close_after_response);
                state->writer->pending =
                    RuntimeHttpServerResponseWriter::Pending::Head;
                state->producer = response.producer_factory(
                    {Value::io_value(state->writer)});

                (void)state->producer();
                if (runtime_task_is_parked()) {
                  return Value::null();
                }
                if (!http_server_finish_streaming_response(state->writer)) {
                  throw RuntimeTaskFailure(
                      "ConnectionError",
                      "failed to finish streaming HTTP response");
                }
                if (!complete_request()) {
                  return Value::null();
                }
                continue;
              }

              state->request_body->final_response_started->store(
                  true, std::memory_order_release);
              if (!write_http_server_response(
                      state->stream, std::move(response),
                      server->write_timeout, state->request_method,
                      state->request_minor_version,
                      state->close_after_response)) {
                throw RuntimeTaskFailure(
                    "ConnectionError", "failed to write HTTP response");
              }
              if (!complete_request()) {
                return Value::null();
              }
            } catch (const RuntimeTaskFailure &failure) {
              const bool control_flow =
                  failure.error_name() == "CancelledError" ||
                  failure.error_name() == "FlowCancelledError" ||
                  failure.error_name() == "IsolationError" ||
                  failure.error_name() == "LifetimeError" ||
                  failure.error_name() == "CapabilityError" ||
                  failure.error_name() == "EffectViolationError" ||
                  failure.error_name() == "DeterminismError" ||
                  failure.error_name() == "UnsupportedProfileError" ||
                  failure.error_name() == "WorldFrozenError" ||
                  failure.error_name() == "ConnectionResetError" ||
                  failure.error_name() == "BrokenPipeError" ||
                  failure.error_name() == "UnexpectedEofError";
              if (control_flow) {
                finish_task(state, true);
                return Value::null();
              }
              const bool headers_sent =
                  state->headers_sent ||
                  (state->writer != nullptr && state->writer->head_sent);
              if (!headers_sent) {
                if (state->request_body != nullptr) {
                  state->request_body->final_response_started->store(
                      true, std::memory_order_release);
                }
                RuntimeHttpServerResponse error_response =
                    http_server_error_response(
                        500, failure.message().empty()
                                 ? "handler failed"
                                 : failure.message());
                (void)write_http_server_response(
                    state->stream, std::move(error_response),
                    server->write_timeout, state->request_method,
                    state->request_minor_version, true);
              }
              finish_task(state, true);
              return Value::null();
            } catch (const RuntimeTaskCancelled &) {
              finish_task(state, true);
              return Value::null();
            }
          }
        });
    server->attach_task(*connection_id, std::move(request_task));
  }

  if (max_requests.has_value() && accepted_count >= *max_requests) {
    server->stop_accepting();
  }
  server->wait_until_quiescent();
  result.ok = true;
  result.stats = server->stats();
  return result;
}

} // namespace amber::runtime
