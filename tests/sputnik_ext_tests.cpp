// Unit tests for the native-extension ABI (runtime/sputnik_ext.{h,cpp}) and its
// in-tree driver surface (runtime/sputnik_ext_runtime.h), 5c-ii. An SputnikCtx is
// exercised over a recording StdlibHost -- the marshalling logic (arena, handle
// encoding, predicates, readers, builders, foreign-handle construction +
// tombstone) is what is under test, so a full VM frame is not required; the
// frame-dependent paths (fault recording) are routed to the recording host.

#include "runtime/sputnik_ext.h"
#include "runtime/sputnik_ext_runtime.h"
#include "runtime/concurrency.h"
#include "runtime/io.h"
#include "runtime/native_call_buffer.h"
#include "runtime/stdlib_registry.h"
#include "runtime/world.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using sputnik::runtime::NativeTagRegistry;
using sputnik::runtime::NativeTypeDescriptor;
using sputnik::runtime::NativeExtErrorDescriptor;
using sputnik::runtime::NativeExtRegistry;
using sputnik::runtime::RuntimeBytes;
using sputnik::runtime::RuntimeDispatchRegistry;
using sputnik::runtime::RuntimeErrorRegistry;
using sputnik::runtime::RuntimeForeignHandle;
using sputnik::runtime::RuntimeNativePackageDescriptor;
using sputnik::runtime::RuntimeTypeRegistry;
using sputnik::runtime::RuntimeWorld;
using sputnik::runtime::SendStatus;
using sputnik::runtime::StdlibHost;
using sputnik::runtime::Value;

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "sputnik_ext test failed: " << message << "\n";
    std::exit(1);
  }
}

// Records faults and answers the few host calls the ABI shims actually make
// (string interning, immutable bytes + zero-copy view). Everything else is a
// stub: the ABI round-trip never reaches it.
struct RecordingHost : StdlibHost {
  bool faulted = false;
  std::string fault_class;
  std::string fault_message;
  std::vector<std::string> strings;

  void stdlib_set_fault(const void * /*frame*/, const std::string &error_class,
                        const std::string &message) override {
    faulted = true;
    fault_class = error_class;
    fault_message = message;
  }
  void stdlib_raise_exception(const void * /*frame*/,
                              Value /*exception*/) override {}
  void stdlib_raise_runtime_error(const void * /*frame*/,
                                  const std::string &error_class,
                                  const std::string &message) override {
    // The ABI's sputnik_fault / sputnik_handle_ptr now raise rescuable errors; the
    // recording host treats them like a recorded fault for assertions.
    faulted = true;
    fault_class = error_class;
    fault_message = message;
  }
  bool stdlib_write_output(const void * /*frame*/, bool /*stderr_stream*/,
                           const std::string & /*text*/) override {
    return true;
  }
  std::optional<Value> stdlib_keyword_arg_value(
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      const std::string & /*name*/) override {
    return std::nullopt;
  }
  bool stdlib_reject_unknown_keywords(
      const void * /*frame*/,
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      std::initializer_list<const char *> /*allowed*/) override {
    return true;
  }
  Value stdlib_string_value_from_text(std::string text) override {
    const std::uint32_t id = static_cast<std::uint32_t>(strings.size());
    strings.push_back(std::move(text));
    return Value::string(id);
  }
  Value stdlib_symbol_value_from_text(std::string /*text*/) override {
    return Value::null();
  }
  std::optional<std::string> stdlib_text_of(const Value &value) override {
    if (value.is_string() && value.as_string().string_id < strings.size()) {
      return strings[value.as_string().string_id];
    }
    return std::nullopt;
  }
  std::string stdlib_display_string(const void * /*frame*/,
                                    const Value &value) override {
    if (const std::optional<std::string> text = stdlib_text_of(value)) {
      return *text;
    }
    return "";
  }
  std::optional<std::string> stdlib_bytes_of(const void * /*frame*/,
                                             const Value & /*value*/) override {
    return std::nullopt;
  }
  Value stdlib_bytes_value_from_bytes(std::string bytes) override {
    return Value::io_value(std::make_shared<RuntimeBytes>(std::move(bytes)));
  }
  bool stdlib_bytes_view(const void * /*frame*/, const Value &value,
                         const std::uint8_t **ptr, std::size_t *len,
                         Value *keepalive) override {
    if (!value.is_io_value()) {
      return false;
    }
    const auto bytes =
        std::dynamic_pointer_cast<RuntimeBytes>(value.as_io_value());
    if (bytes == nullptr) {
      return false;
    }
    const std::string &storage = bytes->string();
    *ptr = reinterpret_cast<const std::uint8_t *>(storage.data());
    *len = storage.size();
    *keepalive = value;
    return true;
  }
  Value stdlib_make_list(std::vector<Value> /*items*/) override {
    return Value::null();
  }
  Value stdlib_make_tuple(std::vector<Value> /*items*/) override {
    return Value::null();
  }
  Value
  stdlib_make_object(std::vector<std::pair<std::string, Value>> /*entries*/,
                     bool /*strict*/) override {
    return Value::null();
  }
  bool stdlib_string_keyed_entries(
      const void * /*frame*/, const Value & /*value*/,
      std::vector<std::pair<std::string, Value>> * /*out*/) override {
    return false;
  }
  bool stdlib_lookup_string_key(const void * /*frame*/, const Value & /*value*/,
                                const std::string & /*key*/, Value * /*out*/,
                                bool * /*found*/) override {
    return false;
  }
  bool stdlib_map_values(const void * /*frame*/, const Value & /*value*/,
                         std::vector<Value> * /*out*/) override {
    return false;
  }
  bool stdlib_list_items(const void * /*frame*/, const Value & /*value*/,
                         std::vector<Value> * /*out*/) override {
    return false;
  }
  bool stdlib_sequence_items(const void * /*frame*/, const Value & /*value*/,
                             std::vector<Value> * /*out*/) override {
    return false;
  }
  sputnik::runtime::StdlibBlockResult
  stdlib_call_stream_block(const void * /*frame*/, const Value & /*block*/,
                           Value /*value*/) override {
    return {};
  }
  sputnik::runtime::StdlibBlockResult
  stdlib_call_path_block(const void * /*frame*/, const Value & /*block*/,
                         Value /*value*/, Value /*accumulator*/) override {
    return {};
  }
  sputnik::runtime::StdlibBlockResult
  stdlib_call_block(const void * /*frame*/, const Value & /*block*/,
                    std::vector<Value> /*args*/) override {
    return {};
  }
  bool stdlib_block_suspension_in_property_arm(
      const void * /*frame*/, const std::string & /*context*/) override {
    return false;
  }
  void stdlib_throw_json_stop(const void * /*frame*/,
                              std::optional<Value> /*value*/) override {}
  bool
  stdlib_integer_range(const void * /*frame*/, const Value & /*value*/,
                       sputnik::runtime::StdlibIntegerRange * /*out*/) override {
    return false;
  }
  bool stdlib_fs_exists(const void * /*frame*/, const std::string & /*path*/,
                        bool * /*out*/) override {
    return false;
  }
  bool stdlib_fs_file(const void * /*frame*/, const std::string & /*path*/,
                      bool * /*out*/) override {
    return false;
  }
  bool stdlib_fs_dir(const void * /*frame*/, const std::string & /*path*/,
                     bool * /*out*/) override {
    return false;
  }
  bool stdlib_fs_metadata(const void * /*frame*/, const std::string & /*path*/,
                          Value * /*out*/) override {
    return false;
  }
  bool stdlib_fs_read_bytes_limited(const void * /*frame*/,
                                    const std::string & /*path*/,
                                    std::optional<std::size_t> /*limit*/,
                                    std::string * /*out*/) override {
    return false;
  }
  bool stdlib_fs_read_text_limited(const void * /*frame*/,
                                   const std::string & /*path*/,
                                   std::optional<std::size_t> /*limit*/,
                                   std::string * /*out*/) override {
    return false;
  }
  bool stdlib_fs_read_text(const void * /*frame*/, const std::string & /*path*/,
                           std::string * /*out*/) override {
    return false;
  }
  bool stdlib_fs_write_text(const void * /*frame*/,
                            const std::string & /*path*/,
                            const std::string & /*text*/) override {
    return false;
  }
  bool stdlib_fs_write_bytes_value(const void * /*frame*/,
                                   const std::string & /*path*/,
                                   const std::string & /*bytes*/,
                                   bool /*create*/,
                                   bool /*truncate*/) override {
    return false;
  }
  bool stdlib_fs_write_text_value(const void * /*frame*/,
                                  const std::string & /*path*/,
                                  const std::string & /*text*/, bool /*create*/,
                                  bool /*truncate*/) override {
    return false;
  }
  bool stdlib_fs_mkdir(const void * /*frame*/,
                       const std::string & /*path*/) override {
    return false;
  }
  bool stdlib_fs_mkdir_p(const void * /*frame*/,
                         const std::string & /*path*/) override {
    return false;
  }
  bool stdlib_fs_remove(const void * /*frame*/,
                        const std::string & /*path*/) override {
    return false;
  }
  bool stdlib_fs_rename(const void * /*frame*/, const std::string & /*from*/,
                        const std::string & /*to*/) override {
    return false;
  }
  bool stdlib_fs_copy(const void * /*frame*/, const std::string & /*from*/,
                      const std::string & /*to*/,
                      std::size_t * /*count*/) override {
    return false;
  }
  bool stdlib_fs_open_file(const void * /*frame*/, const std::string & /*path*/,
                           sputnik::runtime::RuntimeFileMode /*mode*/,
                           sputnik::runtime::RuntimeFileOpenOptions /*options*/,
                           sputnik::runtime::RuntimeIsolationMode /*isolation*/,
                           Value * /*out*/) override {
    return false;
  }
  bool stdlib_fs_close_file(const void * /*frame*/, const Value & /*file*/,
                            bool /*report_fault*/) override {
    return false;
  }
  bool stdlib_net_udp_bind(const void * /*frame*/,
                           const sputnik::runtime::RuntimeEndpoint & /*endpoint*/,
                           sputnik::runtime::RuntimeIsolationMode /*isolation*/,
                           Value * /*out*/) override {
    return false;
  }
  bool stdlib_net_udp_open(const void * /*frame*/,
                           const std::string & /*family*/,
                           sputnik::runtime::RuntimeIsolationMode /*isolation*/,
                           Value * /*out*/) override {
    return false;
  }
  bool
  stdlib_net_tcp_connect(const void * /*frame*/,
                         const sputnik::runtime::RuntimeEndpoint & /*endpoint*/,
                         std::chrono::milliseconds /*timeout*/,
                         sputnik::runtime::RuntimeIsolationMode /*isolation*/,
                         Value * /*out*/) override {
    return false;
  }
  bool
  stdlib_net_tcp_listen(const void * /*frame*/,
                        const sputnik::runtime::RuntimeEndpoint & /*endpoint*/,
                        int /*backlog*/, bool /*reuse_addr*/,
                        sputnik::runtime::RuntimeIsolationMode /*isolation*/,
                        Value * /*out*/) override {
    return false;
  }
  bool stdlib_net_tcp_close(const void * /*frame*/, const Value & /*resource*/,
                            bool /*report_fault*/) override {
    return false;
  }
  SendStatus stdlib_vm_io_value_intrinsic_send(
      const void * /*frame*/, const Value & /*receiver*/,
      const std::string & /*selector*/, const std::vector<Value> & /*args*/,
      const Value & /*block*/,
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      Value * /*out*/) override {
    return SendStatus::NotHandled;
  }
  SendStatus stdlib_net_http_construct_client(
      const void * /*frame*/, const std::vector<Value> & /*args*/,
      const Value & /*block*/,
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      Value * /*out*/) override {
    return SendStatus::NotHandled;
  }
  SendStatus stdlib_net_http_construct_request(
      const void * /*frame*/, const std::vector<Value> & /*args*/,
      const Value & /*block*/,
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      Value * /*out*/) override {
    return SendStatus::NotHandled;
  }
  SendStatus stdlib_net_http_construct_headers(
      const void * /*frame*/, const std::vector<Value> & /*args*/,
      const Value & /*block*/,
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      Value * /*out*/) override {
    return SendStatus::NotHandled;
  }
  SendStatus stdlib_net_http_construct_server(
      const void * /*frame*/, const std::vector<Value> & /*args*/,
      const Value & /*block*/,
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      Value * /*out*/) override {
    return SendStatus::NotHandled;
  }
  SendStatus stdlib_net_http_construct_server_response(
      const void * /*frame*/, const std::vector<Value> & /*args*/,
      const Value & /*block*/,
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      Value * /*out*/) override {
    return SendStatus::NotHandled;
  }
  SendStatus stdlib_net_http_construct_form_body(
      const void * /*frame*/, const std::vector<Value> & /*args*/,
      const Value & /*block*/,
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      Value * /*out*/) override {
    return SendStatus::NotHandled;
  }
  SendStatus stdlib_net_http_json_get(
      const void * /*frame*/, const std::vector<Value> & /*args*/,
      const Value & /*block*/,
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      Value * /*out*/) override {
    return SendStatus::NotHandled;
  }
  SendStatus stdlib_net_http_json_post(
      const void * /*frame*/, const std::vector<Value> & /*args*/,
      const Value & /*block*/,
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      Value * /*out*/) override {
    return SendStatus::NotHandled;
  }
  SendStatus stdlib_net_http_request_body_type_send(
      const void * /*frame*/, const std::string & /*selector*/,
      const std::vector<Value> & /*args*/, const Value & /*block*/,
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      Value * /*out*/) override {
    return SendStatus::NotHandled;
  }
  SendStatus stdlib_net_http_server_response_type_send(
      const void * /*frame*/, const std::string & /*selector*/,
      const std::vector<Value> & /*args*/, const Value & /*block*/,
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      Value * /*out*/) override {
    return SendStatus::NotHandled;
  }
  SendStatus stdlib_vm_task_intrinsic_send(
      const void * /*frame*/, const Value & /*receiver*/,
      const std::string & /*selector*/, const std::vector<Value> & /*args*/,
      const Value & /*block*/,
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      Value * /*out*/) override {
    return SendStatus::NotHandled;
  }
  bool stdlib_secure_random_bytes(const void * /*frame*/, std::size_t /*count*/,
                                  std::string * /*out*/) override {
    return false;
  }
  std::optional<sputnik::runtime::RuntimeTimeValue>
  stdlib_wall_time_now(const void * /*frame*/) override {
    return sputnik::runtime::RuntimeTimeValue{};
  }
  std::optional<sputnik::runtime::RuntimeTimePeriodValue>
  stdlib_monotonic_time(const void * /*frame*/) override {
    return sputnik::runtime::RuntimeTimePeriodValue{};
  }
};

// File-scope sinks so the descriptor callbacks can be plain function pointers
// (matching the C ABI signatures, which cannot capture).
int g_owned_destroyed = 0;
void *g_owned_ctx = nullptr;
void *g_owned_ptr = nullptr;
void owned_destructor(void *ctx, void *handle) {
  ++g_owned_destroyed;
  g_owned_ctx = ctx;
  g_owned_ptr = handle;
}

void test_sputnik_ext_scalar_round_trip() {
  RecordingHost host;
  NativeTagRegistry tags;
  SputnikCtx *cx = sputnik::runtime::sputnik_ext_ctx_open(host, nullptr, tags);

  const SputnikValue null_v = sputnik_make_null(cx);
  expect(sputnik_is_null(cx, null_v) == 1, "null builds and reads as null");

  const SputnikValue bool_v = sputnik_make_bool(cx, 1);
  int bool_out = 0;
  expect(sputnik_is_bool(cx, bool_v) == 1, "bool predicate");
  expect(sputnik_as_bool(cx, bool_v, &bool_out) == 1 && bool_out == 1,
         "bool round-trips");

  const SputnikValue int_v = sputnik_make_int(cx, -42);
  std::int64_t int_out = 0;
  expect(sputnik_is_int(cx, int_v) == 1, "int predicate");
  expect(sputnik_as_int(cx, int_v, &int_out) == 1 && int_out == -42,
         "int round-trips");
  int wrong = 0;
  expect(sputnik_as_bool(cx, int_v, &wrong) == 0,
         "reading an int as bool reports wrong kind without a fault");
  expect(!host.faulted, "wrong-kind reader sets no fault");

  const SputnikValue float_v = sputnik_make_float(cx, 2.5);
  double float_out = 0.0;
  expect(sputnik_is_float(cx, float_v) == 1, "float predicate");
  expect(sputnik_as_float(cx, float_v, &float_out) == 1 && float_out == 2.5,
         "float round-trips");

  sputnik::runtime::sputnik_ext_ctx_close(cx);
}

void test_call_arena_value_lifetime() {
  std::weak_ptr<RuntimeBytes> weak;
  Value retained;
  {
    sputnik::runtime::NativeCallValueArena<Value> arena;
    auto bytes = std::make_shared<RuntimeBytes>("retained through overflow");
    weak = bytes;
    arena.push_back(Value::io_value(std::move(bytes)));
    const Value *first = &arena[0];
    for (std::int64_t i = 1; i < 128; ++i) {
      arena.push_back(Value::integer(i));
    }
    expect(&arena[0] == first && !weak.expired(),
           "inline values remain alive and stable through overflow growth");
    for (std::size_t i = 1; i < arena.size(); ++i) {
      expect(arena[i].as_integer() == static_cast<std::int64_t>(i),
             "arena handles address both inline and overflow values");
    }
    retained = arena[0];
  }
  expect(!weak.expired(), "a returned value outlives its call arena");
  retained = Value::null();
  expect(weak.expired(), "call arena releases its resource ownership");
}

void test_sputnik_ext_str_and_bytes_round_trip() {
  RecordingHost host;
  NativeTagRegistry tags;
  SputnikCtx *cx = sputnik::runtime::sputnik_ext_ctx_open(host, nullptr, tags);

  const std::string text = "blake3";
  const SputnikValue str_v = sputnik_make_str(cx, text.data(), text.size());
  expect(sputnik_is_str(cx, str_v) == 1, "str predicate");
  const char *str_ptr = nullptr;
  std::size_t str_len = 0;
  expect(sputnik_str_view(cx, str_v, &str_ptr, &str_len) == 1,
         "str view succeeds");
  expect(str_len == text.size() &&
             std::memcmp(str_ptr, text.data(), text.size()) == 0,
         "str view round-trips the bytes");

  for (std::size_t i = 0; i < 256; ++i) {
    const std::string extra = "borrowed-" + std::to_string(i);
    const SputnikValue value = sputnik_make_str(cx, extra.data(), extra.size());
    const char *next_ptr = nullptr;
    std::size_t next_len = 0;
    expect(sputnik_str_view(cx, value, &next_ptr, &next_len) == 1 &&
               next_len == extra.size() &&
               std::memcmp(next_ptr, extra.data(), extra.size()) == 0,
           "subsequent string views remain valid");
  }
  expect(std::memcmp(str_ptr, text.data(), text.size()) == 0,
         "borrowed string storage survives subsequent arena and view growth");

  const std::string blob = std::string("\x00\x01\xfe\xff", 4);
  const SputnikValue bytes_v = sputnik_make_bytes(
      cx, reinterpret_cast<const std::uint8_t *>(blob.data()), blob.size());
  expect(sputnik_is_bytes(cx, bytes_v) == 1, "bytes predicate");
  expect(sputnik_is_str(cx, bytes_v) == 0, "bytes is not a str");
  const std::uint8_t *bytes_ptr = nullptr;
  std::size_t bytes_len = 0;
  expect(sputnik_bytes_view(cx, bytes_v, &bytes_ptr, &bytes_len) == 1,
         "bytes view succeeds");
  expect(bytes_len == blob.size() &&
             std::memcmp(bytes_ptr, blob.data(), blob.size()) == 0,
         "bytes view is zero-copy and exact");
  expect(sputnik_bytes_view(cx, str_v, &bytes_ptr, &bytes_len) == 0,
         "bytes view rejects a non-bytes value");
  expect(!host.faulted, "a wrong-kind bytes view sets no fault");

  sputnik::runtime::sputnik_ext_ctx_close(cx);
}

void test_sputnik_ext_handle_lifecycle() {
  g_owned_destroyed = 0;
  g_owned_ctx = nullptr;
  g_owned_ptr = nullptr;

  RecordingHost host;
  NativeTagRegistry tags;
  NativeTypeDescriptor owned;
  owned.tag = "blake3.Hasher";
  owned.ownership = RuntimeForeignHandle::Ownership::Owned;
  owned.owned_destructor = &owned_destructor;
  tags.register_type(owned);

  SputnikCtx *cx = sputnik::runtime::sputnik_ext_ctx_open(host, nullptr, tags);

  int resource = 99;
  const SputnikValue handle_v = sputnik_make_handle(cx, "blake3.Hasher", &resource);
  expect(!host.faulted, "make_handle with a known tag does not fault");
  expect(sputnik_is_handle(cx, handle_v) == 1, "handle predicate");

  void *recovered = nullptr;
  expect(sputnik_handle_ptr(cx, handle_v, "blake3.Hasher", &recovered) == 1 &&
             recovered == &resource,
         "handle_ptr recovers the foreign pointer for a live, matching tag");

  void *mismatch = nullptr;
  expect(sputnik_handle_ptr(cx, handle_v, "other.Tag", &mismatch) == 0,
         "handle_ptr rejects a tag mismatch");
  expect(host.faulted && host.fault_class == "TypeError",
         "tag mismatch records a TypeError");
  host.faulted = false;

  // Deterministic destroy! threads the destroy-time ctx into the destructor.
  Value handle_value = sputnik::runtime::sputnik_ext_ctx_export(cx, handle_v);
  expect(handle_value.is_foreign_handle(),
         "exported value is a foreign handle");
  int destroy_ctx_marker = 0;
  expect(handle_value.as_foreign_handle()->destroy(&destroy_ctx_marker),
         "owned destroy! reports it ran");
  expect(g_owned_destroyed == 1 && g_owned_ctx == &destroy_ctx_marker &&
             g_owned_ptr == &resource,
         "owned destructor runs once with the destroy-time ctx and resource");

  // Use-after-destroy! is a tombstoned LifetimeError.
  void *dead = nullptr;
  expect(sputnik_handle_ptr(cx, handle_v, "blake3.Hasher", &dead) == 0,
         "handle_ptr fails after destroy!");
  expect(host.faulted && host.fault_class == "LifetimeError",
         "use-after-destroy! records a LifetimeError");

  sputnik::runtime::sputnik_ext_ctx_close(cx);
}

void test_sputnik_ext_make_handle_unknown_tag() {
  RecordingHost host;
  NativeTagRegistry tags; // empty
  SputnikCtx *cx = sputnik::runtime::sputnik_ext_ctx_open(host, nullptr, tags);

  int resource = 0;
  const SputnikValue handle_v = sputnik_make_handle(cx, "missing.Tag", &resource);
  expect(host.faulted && host.fault_class == "TypeError",
         "an unknown tag records a TypeError");
  expect(sputnik_is_null(cx, handle_v) == 1,
         "an unknown tag yields a null handle value");

  sputnik::runtime::sputnik_ext_ctx_close(cx);
}

void test_sputnik_ext_fault_and_version() {
  RecordingHost host;
  NativeTagRegistry tags;
  SputnikCtx *cx = sputnik::runtime::sputnik_ext_ctx_open(host, nullptr, tags);

  const SputnikStatus status = sputnik_fault(cx, "ValueError", "bad input");
  expect(status == SPUTNIK_ERR, "sputnik_fault returns SPUTNIK_ERR");
  expect(host.faulted && host.fault_class == "ValueError" &&
             host.fault_message == "bad input",
         "sputnik_fault records the named error class and message");

  expect(sputnik_ext_abi_version() == SPUTNIK_EXT_ABI_VERSION,
         "ABI version handshake matches");

  sputnik::runtime::sputnik_ext_ctx_close(cx);
}

void test_native_extension_type_import() {
  using Ownership = RuntimeForeignHandle::Ownership;

  NativeExtRegistry extension_registry;
  NativeTypeDescriptor owned;
  owned.tag = "pkg.Handle";
  owned.ownership = Ownership::Owned;
  owned.owned_destructor = &owned_destructor;
  extension_registry.register_type(owned);

  RuntimeTypeRegistry types;
  extension_registry.register_types(types);

  const NativeTypeDescriptor *lookup =
      types.native_package_tags().lookup("pkg.Handle");
  expect(lookup != nullptr && lookup->ownership == Ownership::Owned &&
             lookup->owned_destructor == &owned_destructor,
         "native extension types import into RuntimeTypeRegistry");
  expect(types.native_package_tags().lookup("missing.Handle") == nullptr,
         "imported runtime type tags preserve unknown-tag misses");
}

void test_native_extension_thunk_import() {
  NativeExtRegistry extension_registry;
  int marker = 0;
  extension_registry.register_thunk("pkg.fn", &marker);

  RuntimeDispatchRegistry dispatch;
  extension_registry.register_thunks(dispatch);

  expect(dispatch.native_package_thunk("pkg.fn") == &marker,
         "native extension thunks import into RuntimeDispatchRegistry");
  expect(dispatch.native_package_thunk("pkg.missing") == nullptr,
         "imported native extension thunk map preserves misses");
  const auto resolved = extension_registry.resolve_thunk("pkg.fn");
  expect(resolved.has_value() && resolved->fn == &marker,
         "fully-native hosts can resolve a registered thunk directly");
  expect(!extension_registry.resolve_thunk("pkg.missing").has_value(),
         "direct thunk lookup preserves misses");
}

void test_native_extension_runtime_contributions() {
  using Ownership = RuntimeForeignHandle::Ownership;

  NativeExtRegistry extension_registry;
  RuntimeNativePackageDescriptor package;
  int marker = 0;
  package.thunks.push_back({"pkg.fn", &marker, true});
  package.code_bindings.push_back({7, false, "pkg.fn"});
  package.method_bindings.push_back({"pkg.Handle", "bump!", "pkg.fn"});

  NativeTypeDescriptor owned;
  owned.tag = "pkg.Handle";
  owned.ownership = Ownership::Owned;
  owned.owned_destructor = &owned_destructor;
  package.types.push_back(owned);

  NativeExtErrorDescriptor error;
  error.name = "Pkg.NativeLeafError";
  error.default_message = "package failed";
  error.default_exit_code = 23;
  package.errors.push_back(error);
  extension_registry.register_package(std::move(package));

  RuntimeDispatchRegistry dispatch;
  RuntimeTypeRegistry types;
  RuntimeErrorRegistry errors;
  extension_registry.register_runtime_contributions(dispatch, types, errors);

  expect(dispatch.native_package_thunk("pkg.fn") == &marker,
         "native extension contributor imports thunks");
  expect(dispatch.native_package_thunk_is_blocking("pkg.fn"),
         "native extension contributor imports blocking thunk metadata");
  const auto *code_binding = dispatch.native_package_code_binding(7);
  expect(code_binding != nullptr && !code_binding->method &&
             code_binding->logical == "pkg.fn",
         "native extension contributor imports code bindings");
  const std::string *method_binding =
      dispatch.native_package_method_binding("pkg.Handle", "bump!");
  expect(method_binding != nullptr && *method_binding == "pkg.fn",
         "native extension contributor imports method bindings");

  const NativeTypeDescriptor *lookup =
      types.native_package_tags().lookup("pkg.Handle");
  expect(lookup != nullptr && lookup->ownership == Ownership::Owned &&
             lookup->owned_destructor == &owned_destructor,
         "native extension contributor imports types");

  const auto native_error = errors.error_id("NativeError");
  const auto package_error = errors.error_id("Pkg.NativeLeafError");
  expect(native_error.has_value() && package_error.has_value() &&
             errors.error_is_a(*package_error, *native_error),
         "native extension contributor imports default-parent errors");
  expect(errors.error_default_exit_code(*package_error).has_value() &&
             *errors.error_default_exit_code(*package_error) == 23,
         "native extension contributor imports error exit codes");
  expect(std::string(errors.error_default_message(*package_error)) ==
             "package failed",
         "native extension contributor imports error messages");
}

SputnikStatus direct_increment(SputnikCtx *cx, const SputnikValue *args,
                             std::size_t argc, SputnikValue *out) {
  std::int64_t value = 0;
  if (argc != 1U || !sputnik_as_int(cx, args[0], &value)) {
    return sputnik_fault(cx, "TypeError", "increment expects one Int");
  }
  *out = sputnik_make_int(cx, value + 1);
  return SPUTNIK_OK;
}

struct DirectIntArena {
  std::vector<std::int64_t> values;
  std::string error_name;
  std::string error_message;

  SputnikValue push(std::int64_t value) {
    values.push_back(value);
    return reinterpret_cast<SputnikValue>(
        static_cast<std::uintptr_t>(values.size()));
  }
  bool resolve(SputnikValue handle, std::int64_t *out) const {
    const std::uintptr_t raw = reinterpret_cast<std::uintptr_t>(handle);
    if (raw == 0U || raw > values.size()) {
      return false;
    }
    *out = values[raw - 1U];
    return true;
  }
};

int direct_int_as_int(void *opaque, SputnikValue value, std::int64_t *out) {
  return static_cast<DirectIntArena *>(opaque)->resolve(value, out) ? 1 : 0;
}
SputnikValue direct_int_make_int(void *opaque, std::int64_t value) {
  return static_cast<DirectIntArena *>(opaque)->push(value);
}
SputnikStatus direct_int_fault(void *opaque, const char *name,
                             const char *message) {
  auto *arena = static_cast<DirectIntArena *>(opaque);
  arena->error_name = name == nullptr ? "RuntimeError" : name;
  arena->error_message = message == nullptr ? "" : message;
  return SPUTNIK_ERR;
}

void test_direct_sputnik_ctx_dispatch() {
  sputnik::runtime::SputnikExtDirectOps ops;
  ops.as_int = &direct_int_as_int;
  ops.make_int = &direct_int_make_int;
  ops.fault = &direct_int_fault;

  DirectIntArena arena;
  const SputnikValue input = arena.push(41);
  const sputnik::runtime::SputnikExtDirectCallOutcome result =
      sputnik::runtime::sputnik_ext_invoke_direct_free(
          ops, &arena, &direct_increment, &input, 1U);
  std::int64_t output = 0;
  expect(result.status == SPUTNIK_OK && arena.resolve(result.value, &output) &&
             output == 42,
         "direct SputnikCtx dispatch keeps values in the owner arena");

  const sputnik::runtime::SputnikExtDirectCallOutcome faulted =
      sputnik::runtime::sputnik_ext_invoke_direct_free(
          ops, &arena, &direct_increment, nullptr, 0U);
  expect(faulted.status == SPUTNIK_ERR && arena.error_name == "TypeError" &&
             arena.error_message == "increment expects one Int",
         "direct SputnikCtx dispatch records thunk faults in its backend");

  RuntimeForeignHandle owned;
  owned.ownership = RuntimeForeignHandle::Ownership::Owned;
  int destroy_calls = 0;
  owned.ptr = &destroy_calls;
  SputnikValue destructor_result = nullptr;
  owned.teardown = [&](void *ctx, void *resource) {
    expect(resource == &destroy_calls, "destructor receives the resource pointer");
    ++destroy_calls;
    destructor_result = sputnik_make_int(static_cast<SputnikCtx *>(ctx), 73);
  };
  expect(sputnik::runtime::sputnik_ext_destroy_direct(ops, &arena, owned) &&
             !owned.live && destroy_calls == 1 &&
             arena.resolve(destructor_result, &output) && output == 73,
         "direct teardown supplies a live ABI context and tombstones the handle");
  expect(!sputnik::runtime::sputnik_ext_destroy_direct(ops, &arena, owned) &&
             destroy_calls == 1,
         "direct teardown is idempotent");
}

SputnikStatus blocking_thread_probe(SputnikCtx *cx, const SputnikValue * /*args*/,
                                  std::size_t argc, SputnikValue *out) {
  if (argc != 0U) {
    return sputnik_fault(cx, "TypeError", "blocking probe expects no arguments");
  }
  *out = sputnik_make_bool(
      cx, sputnik::runtime::current_runtime_is_blocking_ffi_thread() ? 1 : 0);
  return SPUTNIK_OK;
}

struct BlockingReloadGate {
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool released = false;
};

BlockingReloadGate blocking_reload_gate;

SputnikStatus blocking_reload_probe(SputnikCtx *cx,
                                  const SputnikValue * /*args*/,
                                  std::size_t argc, SputnikValue *out) {
  if (argc != 0U) {
    return sputnik_fault(cx, "TypeError", "reload probe expects no arguments");
  }
  std::unique_lock<std::mutex> lock(blocking_reload_gate.mutex);
  blocking_reload_gate.entered = true;
  blocking_reload_gate.condition.notify_all();
  blocking_reload_gate.condition.wait(
      lock, [] { return blocking_reload_gate.released; });
  *out = sputnik_make_bool(cx, 1);
  return SPUTNIK_OK;
}

sputnik::bytecode::BcCode notebook_probe_code(std::uint32_t code_id) {
  sputnik::bytecode::BcCode code;
  code.code_id = code_id;
  code.kind = sputnik::bytecode::CodeKind::NotebookCell;
  code.reg_count = 1;
  code.instructions = {
      {sputnik::bytecode::Opcode::LoadNull, {{0, false}}},
      {sputnik::bytecode::Opcode::Return, {{0, false}}},
  };
  return code;
}

void test_blocking_native_call_excludes_image_replacement() {
  constexpr std::uint32_t kNativeCodeId = 19U;
  sputnik::bytecode::BcModule module;
  module.strings = {"sputnik.native.bind:19",
                    "F:test.blocking_reload_probe"};
  module.attrs.push_back({0, 1});
  module.code_objects.push_back(notebook_probe_code(kNativeCodeId));

  RuntimeNativePackageDescriptor package;
  package.thunks.push_back(
      {"test.blocking_reload_probe",
       reinterpret_cast<void *>(&blocking_reload_probe), true});
  NativeExtRegistry::global().register_package(std::move(package));

  RuntimeWorld world(module);
  auto replacement = std::make_shared<sputnik::bytecode::BcModule>(module);
  replacement->attrs.clear();
  replacement->code_objects.clear();
  replacement->code_objects.push_back(notebook_probe_code(20U));

  {
    std::lock_guard<std::mutex> lock(blocking_reload_gate.mutex);
    blocking_reload_gate.entered = false;
    blocking_reload_gate.released = false;
  }
  sputnik::runtime::ExecutionResult worker_result;
  std::thread worker([&] {
    worker_result = world.invoke_native_extension(kNativeCodeId);
  });
  bool entered = false;
  {
    std::unique_lock<std::mutex> lock(blocking_reload_gate.mutex);
    entered = blocking_reload_gate.condition.wait_for(
        lock, std::chrono::seconds(5),
        [] { return blocking_reload_gate.entered; });
    if (!entered) {
      blocking_reload_gate.released = true;
    }
  }
  if (!entered) {
    blocking_reload_gate.condition.notify_all();
    worker.join();
    expect(false, "blocking native reload probe did not enter its thunk");
  }

  const sputnik::runtime::RuntimeNotebookImageInstallResult rejected =
      world.install_notebook_image(replacement);
  expect(!rejected.ok && !rejected.swapped &&
             !rejected.diagnostics.empty() &&
             rejected.diagnostics.front().error_name ==
                 "NativeBridgeBusyError",
         "image replacement must reject an active blocking native VM");

  {
    std::lock_guard<std::mutex> lock(blocking_reload_gate.mutex);
    blocking_reload_gate.released = true;
  }
  blocking_reload_gate.condition.notify_all();
  worker.join();
  expect(worker_result.ok() && worker_result.value.is_bool() &&
             worker_result.value.as_bool(),
         "rejected replacement must let the active native call finish");

  const sputnik::runtime::RuntimeNotebookImageInstallResult installed =
      world.install_notebook_image(replacement);
  expect(installed.ok && installed.swapped,
         "image replacement should succeed after the blocking call exits");
  const sputnik::runtime::ExecutionResult removed_binding =
      world.invoke_native_extension(kNativeCodeId);
  expect(!removed_binding.ok() && removed_binding.fault.has_value() &&
             removed_binding.fault->error_name == "VMError" &&
             removed_binding.fault->message.find("unknown native-extension") !=
                 std::string::npos,
         "replacement must drop old native code and registry bindings");
}

void test_runtime_world_direct_native_extension_call() {
  sputnik::bytecode::BcModule module;
  module.strings = {"sputnik.native.bind:7", "F:test.direct_increment",
                    "sputnik.native.bind:9", "F:test.blocking_thread_probe"};
  module.attrs.push_back({0, 1});
  module.attrs.push_back({2, 3});
  sputnik::bytecode::BcCode code;
  code.code_id = 7;
  code.reg_count = 1;
  module.code_objects.push_back(std::move(code));
  sputnik::bytecode::BcCode unbound_code;
  unbound_code.code_id = 8;
  module.code_objects.push_back(std::move(unbound_code));
  sputnik::bytecode::BcCode blocking_code;
  blocking_code.code_id = 9;
  module.code_objects.push_back(std::move(blocking_code));

  RuntimeNativePackageDescriptor package;
  package.thunks.push_back(
      {"test.direct_increment", reinterpret_cast<void *>(&direct_increment)});
  package.thunks.push_back({"test.blocking_thread_probe",
                            reinterpret_cast<void *>(&blocking_thread_probe),
                            true});
  NativeExtRegistry::global().register_package(std::move(package));

  RuntimeWorld world(module);
  const sputnik::runtime::ExecutionResult faulted =
      world.invoke_native_extension(7);
  expect(!faulted.ok() && faulted.fault.has_value() &&
             faulted.fault->error_name == "TypeError",
         "persistent native extension session reports a thunk fault");

  const sputnik::runtime::ExecutionResult recovered =
      world.invoke_native_extension(7, {Value::integer(41)});
  expect(recovered.ok() && recovered.value.is_integer() &&
             recovered.value.as_integer() == 42,
         "persistent native extension session clears fault state");

  const Value bridge_text = world.string_value("persistent");
  const sputnik::runtime::ExecutionResult bridge_size =
      world.invoke_native_stdlib_send(bridge_text, "size");
  expect(bridge_size.ok() && bridge_size.value.is_integer() &&
             bridge_size.value.as_integer() == 10,
         "native extension and stdlib calls share a synchronized bridge "
         "session");

  const sputnik::runtime::ExecutionResult missing =
      world.invoke_native_extension(8);
  expect(!missing.ok() && missing.fault.has_value() &&
             missing.fault->error_name == "NativeRequiredError",
         "direct native extension invocation rejects an unbound code id");

  const sputnik::runtime::ExecutionResult after_missing =
      world.invoke_native_extension(7, {Value::integer(1)});
  expect(after_missing.ok() && after_missing.value.is_integer() &&
             after_missing.value.as_integer() == 2,
         "persistent native extension session remains reusable after an "
         "unbound code id");

  const sputnik::runtime::ExecutionResult blocking_probe =
      world.invoke_native_extension(9);
  expect(blocking_probe.ok() && blocking_probe.value.is_bool() &&
             blocking_probe.value.as_bool(),
         "blocking native extension executes on the blocking FFI executor");
  const sputnik::runtime::ExecutionResult reused_blocking_probe =
      world.invoke_native_extension(9);
  expect(reused_blocking_probe.ok() && reused_blocking_probe.value.is_bool() &&
             reused_blocking_probe.value.as_bool(),
         "blocking native bridge session remains reusable");
}

} // namespace

int main() {
  test_call_arena_value_lifetime();
  test_sputnik_ext_scalar_round_trip();
  test_sputnik_ext_str_and_bytes_round_trip();
  test_sputnik_ext_handle_lifecycle();
  test_sputnik_ext_make_handle_unknown_tag();
  test_sputnik_ext_fault_and_version();
  test_native_extension_type_import();
  test_native_extension_thunk_import();
  test_native_extension_runtime_contributions();
  test_direct_sputnik_ctx_dispatch();
  test_blocking_native_call_excludes_image_replacement();
  test_runtime_world_direct_native_extension_call();
  std::cout << "sputnik_ext_tests: ok\n";
  return 0;
}
