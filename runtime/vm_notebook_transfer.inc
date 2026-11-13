  // The wire contains names and graph references, never VM indices or handles.
  static constexpr std::size_t notebook_transfer_limit = 7U * 1024U * 1024U;
  static constexpr std::uint32_t notebook_transfer_nodes = 250000;

  struct NotebookTransferWriter {
    std::string bytes = "SPV1";
    void number(std::uint64_t value) {
      for (int shift = 56; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<char>((value >> shift) & 255));
    }
    void string(const std::string &value) {
      if (value.size() > notebook_transfer_limit - std::min(bytes.size(), notebook_transfer_limit))
        throw std::runtime_error("value exceeds the transfer byte limit");
      number(value.size());
      bytes += value;
    }
  };

  struct NotebookTransferReader {
    std::string_view bytes;
    unsigned char tag() {
      if (bytes.empty()) throw std::runtime_error("truncated value");
      const auto value = static_cast<unsigned char>(bytes.front());
      bytes.remove_prefix(1);
      return value;
    }
    std::uint64_t number() {
      std::uint64_t value = 0;
      for (unsigned i = 0; i < 8; ++i) value = (value << 8) | tag();
      return value;
    }
    std::int64_t integer() {
      const auto bits = number();
      std::int64_t value = 0;
      std::memcpy(&value, &bits, sizeof(value));
      return value;
    }
    std::string string() {
      const auto length = number();
      if (length > bytes.size()) throw std::runtime_error("truncated string");
      std::string result(bytes.substr(0, length));
      bytes.remove_prefix(length);
      return result;
    }
    std::size_t count() {
      const auto size = number();
      if (size > notebook_transfer_nodes || size > bytes.size())
        throw std::runtime_error("invalid container length");
      return static_cast<std::size_t>(size);
    }
  };

  std::string encode_notebook_value(Frame &frame, const Value &root) {
    NotebookTransferWriter wire;
    std::unordered_map<const void *, std::uint64_t> seen;
    std::size_t nodes = 0;
    std::function<void(const Value &, unsigned, const std::string &)> encode;
    encode = [&](const Value &value, unsigned depth, const std::string &path) {
      if (depth > 128 || ++nodes > notebook_transfer_nodes ||
          wire.bytes.size() > notebook_transfer_limit)
        throw std::runtime_error("transfer limit exceeded at " + path);
      const auto *header = heap_header_from_value(value);
      const void *identity = header;
      if (value.is_result()) identity = value.as_result().get();
      if (value.is_error_instance()) identity = value.as_error_instance().get();
      if (identity && (value.is_list() || value.is_tuple() || value.is_set() ||
                     value.is_map() || value.is_instance_object() || value.is_result() || value.is_error_instance())) {
        if (header && lifecycle_access_error_name(*header))
          throw std::runtime_error("object is unavailable at " + path);
        const auto [it, inserted] = seen.emplace(identity, seen.size());
        if (!inserted) {
          wire.bytes += 'r';
          wire.number(it->second);
          return;
        }
      }
      if (value.is_null()) wire.bytes += 'n';
      else if (value.is_bool()) wire.bytes += value.as_bool() ? 't' : 'f';
      else if (value.is_integer()) {
        wire.bytes += 'i';
        wire.number(static_cast<std::uint64_t>(value.as_integer()));
      } else if (value.is_float()) {
        wire.bytes += 'd';
        const double number = value.as_float();
        std::uint64_t bits = 0;
        std::memcpy(&bits, &number, sizeof(bits));
        wire.number(bits);
      } else if (value.is_big_int()) {
        wire.bytes += 'b';
        wire.string(big_int_to_decimal_string(*value.as_big_int()));
      } else if (value.is_time()) {
        wire.bytes += 'T';
        const auto time = value.as_time();
        wire.number(time->epoch_seconds);
        wire.number(time->nanosecond);
        wire.number(time->zone_offset_seconds);
        wire.string(time->zone_name);
        wire.number(time->zone_fixed_offset);
      } else if (value.is_time_zone()) {
        wire.bytes += 'Z';
        const auto zone = value.as_time_zone();
        wire.number(zone->offset_seconds);
        wire.string(zone->name);
        wire.number(zone->fixed_offset);
      } else if (value.is_time_period()) {
        wire.bytes += 'D';
        const auto period = value.as_time_period();
        wire.number(period->months);
        wire.number(period->days);
        wire.number(period->nanoseconds);
      } else if (value.is_uuid()) {
        wire.bytes += 'U';
        const auto uuid = value.as_uuid();
        wire.string(std::string(uuid->bytes.begin(), uuid->bytes.end()));
      } else if (value.is_regexp_pattern()) {
        wire.bytes += 'P';
        const auto pattern = value.as_regexp_pattern();
        wire.string(pattern->source);
        wire.number(pattern->flags);
        wire.number(pattern->match_limit);
      } else if (value.is_result()) {
        wire.bytes += value.as_result()->is_ok ? 'O' : 'F';
        encode(value.as_result()->payload, depth + 1, path + ".value");
      } else if (value.is_error_instance()) {
        wire.bytes += '!';
        const auto error = value.as_error_instance();
        wire.string(runtime_error_name(error->error_id));
        wire.string(error->message);
        wire.number(error->fields.size());
        for (const auto &[name, field] : error->fields) {
          wire.string(name);
          encode(field, depth + 1, path + "." + name);
        }
      } else if (value.is_string() || value.is_symbol()) {
        wire.bytes += value.is_symbol() ? 'y' : 's';
        const auto text = selector_text_from_value(value);
        if (!text) throw std::runtime_error("invalid string or symbol at " + path);
        wire.string(*text);
      } else if (value.is_list() || value.is_tuple() || value.is_set()) {
        wire.bytes += value.is_list() ? (value.as_list()->frozen ? 'L' : 'l') :
                      value.is_tuple() ? 'u' : (value.as_set()->frozen ? 'E' : 'e');
        const auto &items = value.is_list() ? value.as_list()->items :
                            value.is_tuple() ? value.as_tuple()->items : value.as_set()->items;
        wire.number(items.size());
        for (std::size_t i = 0; i < items.size(); ++i)
          encode(items[i], depth + 1, path + "[" + std::to_string(i) + "]");
      } else if (value.is_map()) {
        const auto map = value.as_map();
        wire.bytes += map->strict ? (map->frozen ? 'Q' : 'q') : (map->frozen ? 'M' : 'm');
        wire.number(map->entries.size());
        for (std::size_t i = 0; i < map->entries.size(); ++i) {
          const auto index = "[" + std::to_string(i) + "]";
          encode(map->entries[i].key, depth + 1, path + index + ".key");
          encode(map->entries[i].value, depth + 1, path + index + ".value");
        }
      } else if (value.is_instance_object()) {
        const auto instance = value.as_instance_object();
        if (instance_is_native_range(instance)) {
          wire.bytes += 'R';
          for (const auto *name : {"start", "finish", "inclusive_end", "step"}) {
            const auto field = instance->ivars.find(name);
            encode(field == instance->ivars.end() ? Value::null() : field->second,
                   depth + 1, path + "." + name);
          }
          return;
        }
        const auto name = class_name_for_index(instance->class_index);
        if (!name || !instance->header.shape)
          throw std::runtime_error("unsupported instance at " + path);
        if (const auto *dump = find_method_for_dispatch(
                frame, instance->class_index, "sputnik_transfer_dump", kMethodFlagInstance)) {
          if (!find_method_for_dispatch(frame, instance->class_index, "sputnik_transfer_load", kMethodFlagClass))
            throw std::runtime_error("missing transfer loader for " + *name + " at " + path);
          const auto encoded = execute_method_to_value(frame, *dump, {}, {}, value, Value::null());
          const auto payload = encoded && encoded->is_io_value() ?
              std::dynamic_pointer_cast<RuntimeBytes>(encoded->as_io_value()) : nullptr;
          if (!payload) throw std::runtime_error("transfer adapter for " + *name + " must return Bytes at " + path);
          wire.bytes += 'p';
          wire.string(*name);
          wire.string(payload->string());
          return;
        }
        wire.bytes += 'o';
        wire.string(*name);
        const auto shape = state_->root_shape_for_class(instance->class_index);
        wire.number(shape->slot_names.size());
        for (const auto &field : shape->slot_names) wire.string(field);
        const auto &fields = instance->header.shape->slot_names;
        wire.number(fields.size());
        for (std::size_t i = 0; i < fields.size(); ++i) {
          wire.string(fields[i]);
          encode(instance->ivar_storage.at(i), depth + 1, path + "." + fields[i]);
        }
      } else if (value.is_io_value() && std::dynamic_pointer_cast<RuntimeBytes>(value.as_io_value())) {
        wire.bytes += 'x';
        wire.string(std::static_pointer_cast<RuntimeBytes>(value.as_io_value())->string());
      } else {
        const std::string type = value.is_io_value() && value.as_io_value() ?
            value.as_io_value()->type_name() : value.is_foreign_handle() ? "native handle" :
            value.is_closure() ? "closure" : value.is_task_handle() ? "task" : "runtime value";
        throw std::runtime_error("unsupported " + type + " at " + path);
      }
    };
    encode(root, 0, "$");
    if (wire.bytes.size() > notebook_transfer_limit)
      throw std::runtime_error("value exceeds the transfer byte limit");
    return std::move(wire.bytes);
  }

  Value decode_notebook_value(Frame &frame, const std::string &bytes) {
    if (bytes.size() > notebook_transfer_limit || bytes.compare(0, 4, "SPV1") != 0)
      throw std::runtime_error("unsupported value format or size");
    NotebookTransferReader wire{std::string_view(bytes).substr(4)};
    std::vector<Value> objects;
    notebook_transfer_roots_.push_back(&objects);
    struct PopRoots {
      std::vector<const std::vector<Value> *> &roots;
      ~PopRoots() { roots.pop_back(); }
    } pop_roots{notebook_transfer_roots_};
    std::size_t nodes = 0;
    std::function<Value(unsigned)> decode = [&](unsigned depth) -> Value {
      if (depth > 128 || ++nodes > notebook_transfer_nodes)
        throw std::runtime_error("transfer graph limit exceeded");
      const auto tag = wire.tag();
      switch (tag) {
      case 'n': return Value::null();
      case 't': return Value::boolean(true);
      case 'f': return Value::boolean(false);
      case 'i': {
        return Value::integer(wire.integer());
      }
      case 'd': {
        const auto bits = wire.number();
        double value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        return Value::floating(value);
      }
      case 'b': {
        auto value = big_from_decimal_text(wire.string());
        if (!value) throw std::runtime_error("invalid BigInt");
        return Value::big_int(std::make_shared<BigIntValue>(std::move(*value)));
      }
      case 'T': {
        auto value = std::make_shared<RuntimeTimeValue>();
        value->epoch_seconds = wire.integer();
        const auto nanosecond = wire.number();
        const auto offset = wire.integer();
        if (nanosecond >= 1000000000 || offset < -86400 || offset > 86400)
          throw std::runtime_error("invalid Time");
        value->nanosecond = static_cast<std::uint32_t>(nanosecond);
        value->zone_offset_seconds = static_cast<std::int32_t>(offset);
        value->zone_name = wire.string();
        value->zone_fixed_offset = wire.number() != 0;
        return Value::time(std::move(value));
      }
      case 'Z': {
        auto value = std::make_shared<RuntimeTimeZoneValue>();
        const auto offset = wire.integer();
        if (offset < -86400 || offset > 86400) throw std::runtime_error("invalid TimeZone");
        value->offset_seconds = static_cast<std::int32_t>(offset);
        value->name = wire.string();
        value->fixed_offset = wire.number() != 0;
        return Value::time_zone(std::move(value));
      }
      case 'D': {
        auto value = std::make_shared<RuntimeTimePeriodValue>();
        value->months = wire.integer();
        value->days = wire.integer();
        value->nanoseconds = wire.integer();
        return Value::time_period(std::move(value));
      }
      case 'U': {
        const auto bytes = wire.string();
        if (bytes.size() != 16) throw std::runtime_error("invalid Uuid");
        auto value = std::make_shared<RuntimeUuidValue>();
        std::copy(bytes.begin(), bytes.end(), value->bytes.begin());
        return Value::uuid(std::move(value));
      }
      case 'P': {
        const auto source = wire.string();
        const auto flags = wire.number();
        const auto limit = wire.number();
        if (flags > UINT32_MAX || limit > UINT32_MAX) throw std::runtime_error("invalid Regexp");
        auto value = std::make_shared<RuntimeRegexpPatternValue>();
        static_cast<regexp::Pattern &>(*value) = regexp::compile(
            source, static_cast<std::uint32_t>(flags), static_cast<std::uint32_t>(limit));
        return Value::regexp_pattern(std::move(value));
      }
      case 'O': case 'F': {
        auto result = std::make_shared<ResultValue>();
        auto value = Value::result(result);
        objects.push_back(value);
        result->is_ok = tag == 'O';
        result->payload = decode(depth + 1);
        return value;
      }
      case '!': {
        const auto name = wire.string();
        const auto id = runtime_error_id(name);
        if (!id) throw std::runtime_error("unknown error type: " + name);
        auto error = std::make_shared<ErrorInstanceValue>();
        error->error_id = *id;
        error->message = wire.string();
        const auto count = wire.count();
        const auto value = Value::error_instance(error);
        objects.push_back(value);
        for (std::size_t i = 0; i < count; ++i) {
          const auto field = wire.string();
          error->fields.emplace_back(field, decode(depth + 1));
        }
        return value;
      }
      case 'R': {
        auto instance = make_instance_value(kNativeSyntheticClassIndex);
        auto value = Value::instance(instance);
        objects.push_back(value);
        instance->ivars[kNativeRangeMarker] = Value::boolean(true);
        for (const auto *name : {"start", "finish", "inclusive_end", "step"})
          instance->ivars[name] = decode(depth + 1);
        if (!extract_range_bounds(frame, value)) throw std::runtime_error("invalid Range");
        return value;
      }
      case 's': return runtime_string_value(wire.string());
      case 'y': return Value::symbol(intern_runtime_symbol(wire.string()));
      case 'x': return Value::io_value(std::make_shared<RuntimeBytes>(wire.string()));
      case 'r': {
        const auto index = wire.number();
        if (index >= objects.size()) throw std::runtime_error("invalid graph reference");
        return objects[index];
      }
      case 'l': case 'L': case 'u': case 'e': case 'E': {
        const auto count = wire.count();
        auto value = tag == 'l' || tag == 'L' ? make_list_value({}, tag == 'L') :
                     tag == 'u' ? make_tuple_value({}) : make_set_value({}, tag == 'E');
        objects.push_back(value);
        auto &items = value.is_list() ? value.as_list()->items :
                      value.is_tuple() ? value.as_tuple()->items : value.as_set()->items;
        items.reserve(count);
        for (std::size_t i = 0; i < count; ++i) items.push_back(decode(depth + 1));
        return value;
      }
      case 'm': case 'M': case 'q': case 'Q': {
        const auto count = wire.count();
        const bool strict = tag == 'q' || tag == 'Q';
        auto value = make_symbol_map_value({}, tag == 'M' || tag == 'Q', strict);
        objects.push_back(value);
        for (std::size_t i = 0; i < count; ++i) {
          const auto key = decode(depth + 1);
          auto item = decode(depth + 1);
          CollectionKeyError error;
          const auto normalized = normalize_map_key(key, &error);
          if (!normalized) throw std::runtime_error("invalid map key");
          map_value_upsert_entry(value.as_map().get(),
              make_canonical_map_entry(*normalized, std::move(item), strict));
        }
        return value;
      }
      case 'o': case 'p': {
        const auto name = wire.string();
        std::uint32_t index = 0;
        while (index < module_.classes.size() && class_name_for_index(index) != name) ++index;
        if (index == module_.classes.size()) throw std::runtime_error("class unavailable: " + name);
        if (tag == 'p') {
          const auto *load = find_method_for_dispatch(frame, index, "sputnik_transfer_load", kMethodFlagClass);
          if (!load) throw std::runtime_error("transfer loader unavailable: " + name);
          const auto payload = Value::io_value(std::make_shared<RuntimeBytes>(wire.string()));
          const auto restored = execute_method_to_value(
              frame, *load, {payload}, {}, Value::class_object(index), Value::null());
          if (!restored || !restored->is_instance_object() || restored->as_instance_object()->class_index != index)
            throw std::runtime_error("transfer adapter returned an incompatible value for " + name);
          objects.push_back(*restored);
          return *restored;
        }
        const auto shape = state_->root_shape_for_class(index);
        const auto layout_size = wire.count();
        if (layout_size != shape->slot_names.size()) throw std::runtime_error("class layout changed: " + name);
        for (const auto &field : shape->slot_names)
          if (field != wire.string()) throw std::runtime_error("class layout changed: " + name);
        const auto count = wire.count();
        const auto instance = make_instance_value(index);
        const auto value = Value::instance(instance);
        objects.push_back(value);
        if (!ensure_instance_layout(frame, instance)) throw std::runtime_error("invalid class layout: " + name);
        for (std::size_t i = 0; i < count; ++i) {
          const auto field = wire.string();
          if (!store_instance_ivar_slow(frame, instance, field, decode(depth + 1)))
            throw std::runtime_error("cannot restore " + name + "." + field);
        }
        return value;
      }
      default: throw std::runtime_error("invalid value tag");
      }
    };
    auto result = decode(0);
    if (!wire.bytes.empty()) throw std::runtime_error("trailing bytes in value snapshot");
    return result;
  }
