#pragma once

#include "runtime/io.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace sputnik::runtime {

// The process engine has no dependency on Vm, bytecode or either Sputnik value
// representation. Adapters preserve arbitrary callback results as opaque
// values.
struct SystemValue {
  enum class Kind {
    Null,
    Bool,
    Int,
    Float,
    String,
    Bytes,
    List,
    Map,
    Object,
    Callback,
    Opaque,
    Module
  };
  Kind kind = Kind::Null;
  bool boolean = false;
  std::int64_t integer = 0;
  double floating = 0;
  std::string text;
  std::vector<SystemValue> items;
  std::map<std::string, SystemValue> entries;
  std::shared_ptr<RuntimeIoValue> object;
  std::function<SystemValue(const std::vector<SystemValue> &)> callback;
  std::shared_ptr<void> opaque;

  static SystemValue str(std::string text, bool bytes = false);
  static SystemValue number(std::int64_t value);
  static SystemValue boolean_value(bool value);
  static SystemValue resource(std::shared_ptr<RuntimeIoValue> value);
  static SystemValue list(std::vector<SystemValue> values);
};

class SystemError : public std::runtime_error {
public:
  SystemError(std::string name, std::string message)
      : std::runtime_error(std::move(message)), name(std::move(name)) {}
  std::string name;
};

struct SystemCall {
  SystemValue receiver;
  std::string selector;
  std::vector<SystemValue> args;
  std::map<std::string, SystemValue> kwargs;
  SystemValue block;
  // Called before a spawn or an explicit signal. VM supplies capability and
  // replay enforcement; native code enforces embedded grants and commits an
  // effect barrier.
  std::function<void(const std::string &, const std::string &)> authorize;
  std::function<bool()> cancelled;
};

// Named handlers share the calling strand. Serialize their Sputnik instructions,
// releasing the gate while IO waits, so captured mutable values remain safe.
class SystemCallbackScope {
public:
  explicit SystemCallbackScope(std::shared_ptr<std::recursive_mutex> gate,
                               const std::atomic<bool> *cancel = nullptr);
  ~SystemCallbackScope();

private:
  std::shared_ptr<std::recursive_mutex> previous_, gate_;
  unsigned previous_depth_ = 0;
  const std::atomic<bool> *previous_cancel_ = nullptr;
};
class SystemCallbackSuspension {
public:
  explicit SystemCallbackSuspension(bool enabled = true);
  ~SystemCallbackSuspension();

private:
  std::shared_ptr<std::recursive_mutex> gate_;
  unsigned depth_ = 0;
};

bool system_resource(const std::shared_ptr<RuntimeIoValue> &value);
const std::atomic<bool> *system_callback_cancel_flag();
bool system_blocking_selector(const std::string &selector);
SystemValue system_dispatch(const SystemCall &call);

} // namespace sputnik::runtime
