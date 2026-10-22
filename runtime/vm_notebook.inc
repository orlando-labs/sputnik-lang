// Included inside Vm: notebook display uses ordinary Sputnik dispatch to render
// plots, then crosses the host boundary with immutable images and inert scenes.
  bool notebook_scene_bounded(const Value &value, unsigned depth,
                              std::size_t &nodes, std::size_t &bytes) {
    if (depth > 16 || nodes++ >= 200000 || bytes > 4U * 1024U * 1024U) return false;
    bytes += 64; // conservative JSON numbers, separators, keys and brackets
    if (value.is_null() || value.is_bool() || value.is_integer()) return true;
    if (value.is_float()) return std::isfinite(value.as_float());
    if (value.is_string() || value.is_symbol()) {
      const auto text = selector_text_from_value(value);
      if (!text || text->size() > 65536) return false;
      bytes += 6 * text->size();
      return bytes <= 4U * 1024U * 1024U;
    }
    if (value.is_list() && value.as_list()) {
      for (const auto &item : value.as_list()->items)
        if (!notebook_scene_bounded(item, depth + 1, nodes, bytes)) return false;
      return true;
    }
    if (value.is_map() && value.as_map()) {
      for (const auto &entry : value.as_map()->entries) {
        if ((!entry.key.is_string() && !entry.key.is_symbol()) ||
            !notebook_scene_bounded(entry.key, depth + 1, nodes, bytes) ||
            !notebook_scene_bounded(entry.value, depth + 1, nodes, bytes)) return false;
      }
      return true;
    }
    return false;
  }

  NotebookDisplayOrder notebook_order(const Value &value, unsigned depth,
                                       unsigned &remaining) {
    if (depth > 16 || remaining-- == 0)
      throw std::invalid_argument("notebook.show order is too deeply nested or cyclic");
    NotebookDisplayOrder key;
    using Kind = NotebookDisplayOrder::Kind;
    if (value.is_integer()) key.int_value = value.as_integer();
    else if (value.is_float()) {
      key.integer = false;
      key.float_value = value.as_float();
      if (!std::isfinite(key.float_value))
        throw std::invalid_argument("notebook.show order must be finite");
    } else if (value.is_bool()) {
      key.kind = Kind::Bool;
      key.int_value = value.as_bool();
    } else if (value.is_string() || value.is_symbol()) {
      key.kind = value.is_string() ? Kind::String : Kind::Symbol;
      const auto text = selector_text_from_value(value);
      if (!text || text->size() > 65536)
        throw std::invalid_argument("notebook.show order text is invalid or too large");
      key.text = *text;
    } else if (is_desc_wrapper(value)) {
      key.kind = Kind::Desc;
      key.items.push_back(notebook_order(value.as_tuple()->items[1], depth + 1, remaining));
    } else if (value.is_tuple() || value.is_list()) {
      key.kind = value.is_tuple() ? Kind::Tuple : Kind::List;
      const auto *items = value.is_tuple() ?
          (value.as_tuple() ? &value.as_tuple()->items : nullptr) :
          (value.as_list() ? &value.as_list()->items : nullptr);
      if (items == nullptr) throw std::invalid_argument("invalid notebook.show order sequence");
      for (const auto &item : *items) key.items.push_back(notebook_order(item, depth + 1, remaining));
    } else throw std::invalid_argument("notebook.show order must be a sortable number, Bool, Str, Symbol, List or Tuple");
    return key;
  }

  SendStatus apply_notebook_show(Frame &frame, const std::string &selector,
      const std::vector<Value> &args, const Value &block,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args, Value *out) {
    if (selector == "images") return apply_notebook_images(frame, args, block, kw_args, out);
    if (selector == "read_image" || selector == "read_images") return apply_notebook_read_images(frame, selector, args, block, kw_args, out);
    if (selector == "chart_begin" || selector == "chart_point" || selector == "chart_end")
      return apply_notebook_chart(frame, selector, args, block, kw_args, out);
    if (selector == "config" || selector == "cell_config" || selector == "parse_config" || selector == "serialize_config") {
      if (args.size() != 1 || !block.is_null() || !reject_unknown_keywords(frame, kw_args, {"format"})) {
        set_fault(frame, "TypeError", "config requires one argument and optional format keyword");
        return SendStatus::Faulted;
      }
      const auto format_value = keyword_arg_value(kw_args, "format");
      const auto format = format_value ? selector_text_from_value(*format_value) : std::optional<std::string>("json");
      if (!format || (*format != "json" && *format != "yaml" && *format != "toml")) {
        set_fault(frame, "TypeError", "config format must be json, yaml or toml"); return SendStatus::Faulted;
      }
      try {
        if (selector == "serialize_config") {
          std::string json;
          if (!http_json_generate_text(frame, args[0], &json)) return SendStatus::Faulted;
          *out = string_value_from_text(config_from_json(json, *format));
          return SendStatus::Matched;
        }
        auto text = string_text_from_value(args[0]);
        if (!text) throw std::runtime_error("config requires a String");
        if (selector == "config" || selector == "cell_config") {
          const auto capture = notebook_cell_context_.input_capture;
          if (!capture || text->find('\0') != std::string::npos) throw std::runtime_error("config requires an active synchronous notebook cell");
          const auto value = capture->read(selector == "cell_config" ? notebook_cell_input_key(notebook_cell_context_.live_cell_id, *text) : *text);
          if (!value || !std::holds_alternative<std::string>(*value)) throw std::runtime_error("unknown or non-text config: " + *text);
          text = std::get<std::string>(*value);
        }
        const auto json = config_to_json(*text, *format);
        return call_json_native(frame, "parse", {string_value_from_text(json)},
            {{intern_runtime_symbol("duplicate_keys"), Value::symbol(intern_runtime_symbol("reject"))}}, out);
      } catch (const std::exception &error) { set_fault(frame, "NotebookCellError", error.what()); return SendStatus::Faulted; }
    }
    if (selector == "input" || selector == "cell_input") {
      if (args.size() != 1 || !args[0].is_string() || !kw_args.empty() || !block.is_null()) {
        set_fault(frame, "TypeError", "notebook.input requires one string key and no keywords or block");
        return SendStatus::Faulted;
      }
      const auto key = string_text_from_value(args[0]);
      const auto capture = notebook_cell_context_.input_capture;
      if (!capture) {
        set_fault(frame, "NotebookCellError", "notebook.input requires an active synchronous notebook cell");
        return SendStatus::Faulted;
      }
      const bool local = selector == "cell_input";
      const auto value = key && key->find('\0') == std::string::npos
          ? capture->read(local ? notebook_cell_input_key(notebook_cell_context_.live_cell_id, *key) : *key)
          : std::nullopt;
      if (!value) {
        set_fault(frame, "NotebookCellError", std::string(local ? "unknown or unavailable cell input: " : "unknown or unavailable project input: ") + key.value_or("<invalid>"));
        return SendStatus::Faulted;
      }
      if (const auto v = std::get_if<bool>(&*value)) *out = Value::boolean(*v);
      else if (const auto v = std::get_if<std::int64_t>(&*value)) *out = Value::integer(*v);
      else if (const auto v = std::get_if<double>(&*value)) *out = Value::floating(*v);
      else if (const auto v = std::get_if<NotebookInputRange>(&*value)) {
        auto number = [](const NotebookInputNumber &n) {
          if (const auto i = std::get_if<std::int64_t>(&n)) return Value::integer(*i);
          return Value::floating(std::get<double>(n));
        };
        auto range = make_instance_value(kNativeSyntheticClassIndex);
        range->ivars[kNativeRangeMarker] = Value::boolean(true);
        range->ivars["start"] = number(v->start);
        range->ivars["finish"] = number(v->finish);
        range->ivars["inclusive_end"] = Value::boolean(v->inclusive_end);
        range->ivars["step"] = number(v->step);
        *out = Value::instance(std::move(range));
        if (!extract_range_bounds(frame, *out)) return SendStatus::Faulted;
      }
      else *out = string_value_from_text(std::get<std::string>(*value));
      return SendStatus::Matched;
    }
    if (selector == "progress") {
      if (args.size() != 5 || !block.is_null() || !kw_args.empty()) {
        set_fault(frame, "TypeError",
                  "notebook.progress accepts id, current, total, desc, done and no keywords or block");
        return SendStatus::Faulted;
      }
      auto live_sink = notebook_cell_context_.live_store;
      if (!live_sink) live_sink = NotebookLiveScope::current();
      if (!live_sink) {
        *out = Value::boolean(false);
        return SendStatus::Matched;
      }
      if (!live_sink->open()) {
        set_fault(frame, "NotebookCellError",
                  "notebook.progress requires an active notebook run");
        return SendStatus::Faulted;
      }
      try {
        if (!args[0].is_string())
          throw std::invalid_argument("notebook.progress id must be Str");
        const auto id = string_text_from_value(args[0]);
        if (!id || id->empty())
          throw std::invalid_argument("notebook.progress id must not be empty");
        auto number = [](const Value &value, const char *name) {
          if (value.is_integer()) return static_cast<double>(value.as_integer());
          if (value.is_float()) return value.as_float();
          throw std::invalid_argument(std::string("notebook.progress ") + name +
                                      " must be numeric");
        };
        const double current = number(args[1], "current");
        const double total = number(args[2], "total");
        if (!args[3].is_string())
          throw std::invalid_argument("notebook.progress desc must be Str");
        if (!args[4].is_bool())
          throw std::invalid_argument("notebook.progress done must be Bool; received " +
              value_to_debug_string(args[4], &module_, &runtime_strings_, &runtime_symbols_));
        const auto description = string_text_from_value(args[3]);
        if (!description)
          throw std::invalid_argument("notebook.progress desc is invalid");
        const std::uint64_t cell_id = notebook_cell_context_.live_cell_id != 0
            ? notebook_cell_context_.live_cell_id
            : NotebookLiveScope::current_cell_id();
        const std::uint64_t generation = notebook_cell_context_.live_generation != 0
            ? notebook_cell_context_.live_generation
            : NotebookLiveScope::cell_generation();
        const auto status = live_sink->publish_progress(
            cell_id, *id, current, total, *description, args[4].as_bool(),
            NotebookLiveStore::monotonic_millis(), generation);
        if (status == NotebookLivePublishResult::Closed) {
          set_fault(frame, "NotebookCellError",
                    "notebook.progress run is closed or stale");
          return SendStatus::Faulted;
        }
        if (status != NotebookLivePublishResult::Accepted) {
          set_fault(frame, "TypeError", "notebook.progress payload exceeds run limits");
          return SendStatus::Faulted;
        }
        *out = Value::boolean(true);
        return SendStatus::Matched;
      } catch (const std::exception &error) {
        set_fault(frame, "TypeError", error.what());
        return SendStatus::Faulted;
      }
    }
    if (selector == "selection" || selector == "annotations" || selector == "audio_annotations") {
      if (args.size() != 1 || !args[0].is_string() || !kw_args.empty() || !block.is_null()) {
        set_fault(frame, "TypeError", "selection/annotations require one channel ID"); return SendStatus::Faulted;
      }
      const auto capture = notebook_cell_context_.input_capture;
      const auto id = string_text_from_value(args[0]).value_or("");
      const auto input = capture ? capture->read(notebook_component_data_key(selector, id)) : std::nullopt;
      if (!input || !std::holds_alternative<std::string>(*input)) {
        set_fault(frame, "NotebookCellError", "unknown or unavailable accepted component data: " + id); return SendStatus::Faulted;
      }
      const auto status = http_json_parse_text(frame, std::get<std::string>(*input), out);
      if (status != SendStatus::Matched || out->is_null() || selector == "selection") return status;
      if (selector == "audio_annotations") {
        auto field = [&](const char *key) { Value value; bool found = false; stdlib_lookup_string_key(&frame, *out, key, &value, &found); return value; };
        const auto channel = string_text_from_value(field("channel")), asset = string_text_from_value(field("asset")), source = string_text_from_value(field("source"));
        bool current = false;
        if (channel && asset && source && *source == "input") {
          (void)capture->read(audio_input_key(*channel));
          const auto inputs = notebook_cell_context_.project_audio_inputs;
          if (inputs) if (const auto found = inputs->find(*channel); found != inputs->end()) current = notebook_asset_id(found->second.audio.bytes) == *asset;
        } else if (channel && asset && source && *source == "cell") {
          auto live = notebook_cell_context_.live_store; if (!live) live = NotebookLiveScope::current();
          const auto owner = field("cell");
          if (live && owner.is_integer()) for (const auto &event : live->snapshot_cell(owner.as_integer()))
            if (event.kind == NotebookLiveEventKind::Figure && event.id == *channel && event.display.mime == kAudioMime)
              current = notebook_asset_id(audio_decode(event.display.bytes).bytes) == *asset;
        }
        if (!current) *out = Value::null();
        return SendStatus::Matched;
      }
      Value channel, asset; bool has_channel = false, has_asset = false;
      if (!stdlib_lookup_string_key(&frame, *out, "channel", &channel, &has_channel) ||
          !stdlib_lookup_string_key(&frame, *out, "asset", &asset, &has_asset)) return SendStatus::Faulted;
      const auto channel_text = string_text_from_value(channel), asset_text = string_text_from_value(asset);
      if (!has_channel || !has_asset || !channel_text || !asset_text) { *out = Value::null(); return SendStatus::Matched; }
      const std::vector<NotebookDisplay> *images = nullptr;
      const auto configured = notebook_cell_context_.project_images;
      if (configured) if (const auto found = configured->find(*channel_text); found != configured->end()) images = &found->second.images;
      auto live = notebook_cell_context_.live_store; if (!live) live = NotebookLiveScope::current();
      const auto published = live ? live->image_channel(*channel_text) : std::nullopt;
      if (published) images = &published->images;
      if (!images || std::none_of(images->begin(), images->end(), [&](const auto &image) { return notebook_asset_id(image.bytes) == *asset_text; })) *out = Value::null();
      return SendStatus::Matched;
    }
    if (selector == "read_audio") {
      try {
        if (args.size() != 1 || !args[0].is_string() || !kw_args.empty() || !block.is_null()) throw std::invalid_argument("read_audio requires one Audio Input channel");
        const auto id = string_text_from_value(args[0]).value_or("");
        const auto inputs = notebook_cell_context_.project_audio_inputs;
        const auto capture = notebook_cell_context_.input_capture;
        if (!inputs || !inputs->count(id) || !capture || !capture->active() || !capture->read(audio_input_key(id)))
          throw std::invalid_argument("unknown or unavailable accepted Audio Input: " + id);
        const auto &input = inputs->at(id);
        if (input.audio.bytes.empty()) throw std::invalid_argument("Audio Input is empty; choose or record audio and Apply");
        *out = make_symbol_map_value({{stdlib_symbol_value_from_text("data"), stdlib_bytes_value_from_bytes(input.audio.bytes)},
          {stdlib_symbol_value_from_text("format"), string_value_from_text(input.audio.format)}});
        return SendStatus::Matched;
      } catch (const std::exception &e) { set_fault(frame, "NotebookCellError", e.what()); return SendStatus::Faulted; }
    }
    if (selector == "component" || selector == "audio_control") {
      try {
        auto live = notebook_cell_context_.live_store;
        if (!live) live = NotebookLiveScope::current();
        if (!live || !live->open() || !block.is_null()) throw std::invalid_argument("component access requires an active notebook run");
        if (selector == "component") {
          if (args.size() != 1 || !args[0].is_string() || !stdlib_reject_unknown_keywords(&frame, kw_args, {"board"}))
            throw std::invalid_argument("notebook.component requires a hard ID and optional board scope");
          const auto id = string_text_from_value(args[0]).value_or("");
          std::string board;
          if (auto v = keyword_arg_value(kw_args, "board")) {
            if (!v->is_string()) throw std::invalid_argument("component board must be Str");
            board = string_text_from_value(*v).value_or("");
          }
          std::optional<NotebookAudioComponent> target;
          if (notebook_cell_context_.project_audio) for (const auto &c : *notebook_cell_context_.project_audio)
            if (c.id == id && (board.empty() || c.board == board)) {
              if (target) throw std::invalid_argument("component ID is ambiguous; specify board");
              target = c;
            }
          if (!target) throw std::invalid_argument("unknown Audio Player component: " + id);
          std::optional<std::uint32_t> wrapper;
          for (std::uint32_t i = 0; i < module_.classes.size(); ++i)
            if (class_name_for_index(i).value_or("") == "notebook.ui.AudioPlayerHandle") { wrapper = i; break; }
          if (!wrapper) throw std::invalid_argument("component player requires importing notebook.ui or audio");
          const auto *init = find_method_for_dispatch(frame, *wrapper, "init", kMethodFlagInstance);
          if (!init || fault_) return SendStatus::Faulted;
          auto instance = make_instance_value(*wrapper);
          if (!ensure_instance_layout(frame, instance)) return SendStatus::Faulted;
          const auto value = Value::instance(instance);
          if (!execute_method_to_value(frame, *init, {string_value_from_text(target->board + ":" + target->id), Value::integer(target->cell), string_value_from_text(target->channel)}, {}, value, Value::null())) return SendStatus::Faulted;
          *out = value;
        } else {
          if (args.size() != 5 || !kw_args.empty() || !args[0].is_string() || !args[1].is_integer() || args[1].as_integer() <= 0 || !args[2].is_string() || !args[3].is_string())
            throw std::invalid_argument("invalid player handle command");
          const auto scope = string_text_from_value(args[0]).value_or("");
          const auto target = static_cast<std::uint64_t>(args[1].as_integer());
          const auto channel = string_text_from_value(args[2]).value_or("");
          const auto action = string_text_from_value(args[3]).value_or("");
          // Revalidate the handle against the current manifest after edits/run.
          bool configured = false;
          if (notebook_cell_context_.project_audio) for (const auto &c : *notebook_cell_context_.project_audio)
            if (c.board + ":" + c.id == scope && c.cell == target && c.channel == channel) configured = true;
          if (!configured) throw std::invalid_argument("player component handle is stale");
          if (action == "append" || action == "finish" || action == "reset") {
            const auto handle = live->bound_audio(target, channel);
            if (action == "append") {
              const auto bytes = args[4].is_string() ? string_text_from_value(args[4]) : stdlib_bytes_of(&frame, args[4]);
              if (!bytes) return SendStatus::Faulted;
              live->append_audio(handle, *bytes);
            } else if (action == "finish") live->finish_audio(handle); else live->reset_audio(handle);
          } else {
            double number = 0;
            if (action == "seek" || action == "volume") {
              if (args[4].is_float()) number = args[4].as_float();
              else if (args[4].is_integer()) number = args[4].as_integer();
              else throw std::invalid_argument("player seek/volume requires a number");
            } else if (!args[4].is_null()) throw std::invalid_argument("player command does not accept a value");
            live->control_audio(notebook_cell_context_.live_cell_id, notebook_cell_context_.live_generation,
                scope, target, channel, action, number);
          }
          *out = Value::null();
        }
        return SendStatus::Matched;
      } catch (const std::exception &error) { set_fault(frame, "NotebookCellError", error.what()); return SendStatus::Faulted; }
    }
    const bool audio_short = selector == "player";
    if (selector != "show" && !audio_short) return SendStatus::NotHandled;
    if (args.size() != 1 || !block.is_null() ||
        !stdlib_reject_unknown_keywords(&frame, kw_args,
                                        audio_short ? std::initializer_list<const char *>{"caption", "order", "id", "live", "throttle_ms", "position", "volume", "looped", "autoplay", "show_waveform"} : std::initializer_list<const char *>{"caption", "order", "id", "live", "throttle_ms"})) {
      if (!fault_) set_fault(frame, "TypeError", "notebook.show accepts one object, caption:, order:, id:, live:, throttle_ms: and no block");
      return SendStatus::Faulted;
    }
    try {
      const auto id_value = keyword_arg_value(kw_args, "id");
      std::string live_id;
      if (id_value && !id_value->is_null()) {
        if (!id_value->is_string())
          throw std::invalid_argument("notebook.show id must be Str or null");
        live_id = string_text_from_value(*id_value).value_or("");
        if (live_id.empty() || live_id.size() > 256 || live_id.find('\0') != std::string::npos)
          throw std::invalid_argument("notebook.show id must contain 1..256 bytes without NUL");
      }
      bool live = !live_id.empty();
      if (const auto live_value = keyword_arg_value(kw_args, "live");
          live_value && !live_value->is_null()) {
        if (!live_value->is_bool()) throw std::invalid_argument("notebook.show live must be Bool");
        live = live_value->as_bool();
      }
      if (live && live_id.empty())
        throw std::invalid_argument("notebook.show live requires id:");
      std::uint64_t throttle_ms = 0;
      if (const auto throttle_value = keyword_arg_value(kw_args, "throttle_ms");
          throttle_value && !throttle_value->is_null()) {
        if (!throttle_value->is_integer() || throttle_value->as_integer() < 0)
          throw std::invalid_argument("notebook.show throttle_ms must be a non-negative Int");
        throttle_ms = static_cast<std::uint64_t>(throttle_value->as_integer());
      }
      auto live_sink = notebook_cell_context_.live_store;
      if (!live_sink) live_sink = NotebookLiveScope::current();
      const std::uint64_t live_cell_id = notebook_cell_context_.live_cell_id != 0
          ? notebook_cell_context_.live_cell_id
          : NotebookLiveScope::current_cell_id();
      const std::uint64_t live_generation = notebook_cell_context_.live_generation != 0
          ? notebook_cell_context_.live_generation
          : NotebookLiveScope::cell_generation();
      if (live) {
        if (!live_sink || !live_sink->open()) {
          set_fault(frame, "NotebookCellError", "notebook.show live requires an active notebook run");
          return SendStatus::Faulted;
        }
        if (!live_sink->figure_due(live_cell_id, live_id, throttle_ms,
                                   NotebookLiveStore::monotonic_millis(),
                                   live_generation)) {
          *out = Value::null();
          return SendStatus::Matched;
        }
      }
      const auto sink = notebook_cell_context_.display_collector;
      if (!live && (!sink || !sink->active())) {
        set_fault(frame, "NotebookCellError", "notebook.show requires an active synchronous notebook cell");
        return SendStatus::Faulted;
      }
      NotebookDisplay display;
      if (const auto caption = keyword_arg_value(kw_args, "caption"); caption && !caption->is_null()) {
        if (!caption->is_string()) throw std::invalid_argument("notebook.show caption must be Str or null");
        display.caption = string_text_from_value(*caption).value_or("");
        if (display.caption.size() > 65536) throw std::invalid_argument("notebook.show caption exceeds 64 KiB");
      }
      const auto order = keyword_arg_value(kw_args, "order");
      unsigned remaining = 256;
      const auto key = notebook_order(order.value_or(Value::integer(0)), 0, remaining);
      if (!args[0].is_instance_object() || !args[0].as_instance_object())
        throw std::invalid_argument("notebook.show currently supports plot Figure, Figure3D, PNG Rendered and notebook.ui.DataTableView objects");
      const auto object = args[0].as_instance_object();
      const auto name = class_name_for_index(object->class_index).value_or("");
      if (!ensure_lifecycle_access(frame, args[0])) return SendStatus::Faulted;
      if (name == "notebook.ui.AudioPlayer" || (audio_short && name == "audio.AudioTrack")) {
        const auto *method = find_method_for_dispatch(frame, object->class_index, "notebook_audio", kMethodFlagInstance);
        if (!method) throw std::invalid_argument("audio object is missing notebook_audio()");
        auto spec = execute_method_to_value(frame, *method, {}, {}, args[0], Value::null(), "notebook audio");
        if (!spec) return SendStatus::Faulted;
        auto field = [&](const char *key, Value fallback) {
          if (audio_short) if (auto value = keyword_arg_value(kw_args, key)) return *value;
          Value value; bool found = false;
          if (!stdlib_lookup_string_key(&frame, *spec, key, &value, &found)) throw std::invalid_argument("invalid audio descriptor");
          return found ? value : fallback;
        };
        AudioDisplay audio;

        auto number = [&](const char *key, double fallback) {
          const auto value = field(key, Value::floating(fallback));
          if (value.is_float()) return value.as_float();
          if (value.is_integer()) return double(value.as_integer());
          throw std::invalid_argument(std::string("audio ") + key + " must be numeric");
        };
        auto flag = [&](const char *key, bool fallback) {
          const auto value = field(key, Value::boolean(fallback));
          if (!value.is_bool()) throw std::invalid_argument(std::string("audio ") + key + " must be Bool");
          return value.as_bool();
        };
        audio.position = number("position", 0); audio.volume = number("volume", 1);
        audio.waveform = flag("show_waveform", true); audio.loop = flag("looped", false); audio.autoplay = flag("autoplay", false);
        const auto stream = field("stream", Value::null());
        if (!stream.is_null()) {
          if (!stream.is_integer() || stream.as_integer() <= 0 || !live_sink || !live_sink->open())
            throw std::invalid_argument("PCM player requires an active stream/run");
          if (live_id.empty()) live_id = "audio_" + std::to_string(stream.as_integer());
          live_sink->bind_audio(stream.as_integer(), live_cell_id, live_generation, live_id, audio, display.caption);
          *out = Value::null(); return SendStatus::Matched;
        }
        const auto data = field("data", Value::null());
        const auto bytes = data.is_string() ? string_text_from_value(data) : stdlib_bytes_of(&frame, data);
        if (!bytes) return SendStatus::Faulted;
        audio.bytes = *bytes;
        audio.format = selector_text_from_value(field("format", Value::null())).value_or("");
        display.mime = kAudioMime; display.bytes = audio_encode(audio);
      } else if (audio_short) {
        throw std::invalid_argument("notebook.player requires an AudioTrack");
      } else if (name == "notebook.ui.DataTableView") {
        const auto *method = find_method_for_dispatch(frame, object->class_index, "notebook_table", kMethodFlagInstance);
        if (fault_) return SendStatus::Faulted;
        if (!method) throw std::invalid_argument("DataTableView is missing notebook_table()");
        auto spec = execute_method_to_value(frame, *method, {}, {}, args[0], Value::null(), "notebook.show table");
        if (!spec) return SendStatus::Faulted;
        Value rows, columns;
        bool found_rows = false, found_columns = false;
        if (!stdlib_lookup_string_key(&frame, *spec, "rows", &rows, &found_rows) ||
            !stdlib_lookup_string_key(&frame, *spec, "columns", &columns, &found_columns)) return SendStatus::Faulted;
        if (!found_rows || !found_columns || !columns.is_list() || !columns.as_list())
          throw std::invalid_argument("DataTableView must provide columns and rows");
        std::vector<std::string> names;
        if (columns.as_list()->items.size() > 32)
          throw std::invalid_argument("DataTableView display supports at most 32 columns");
        for (const auto &column : columns.as_list()->items) {
          const auto text = string_text_from_value(column);
          if (!text) throw std::invalid_argument("DataTableView column names must be Str");
          names.push_back(*text);
        }
        display.mime = "application/vnd.sputnik.table+json";
        display.bytes = notebook_table_snapshot(rows, &module_, &runtime_strings_, &runtime_symbols_, &names);
        if (display.bytes.empty()) throw std::invalid_argument("invalid DataTableView columns or rows");
      } else {
      if (name != "plot.Figure" && name != "plot.Figure3D" && name != "plot.Rendered")
        throw std::invalid_argument("notebook.show object is not an plot figure");
      const auto width = instance_ivar_value_or_null(object, "width");
      const auto height = instance_ivar_value_or_null(object, "height");
      if (!width.is_integer() || !height.is_integer() || width.as_integer() <= 0 || height.as_integer() <= 0 ||
          width.as_integer() > 8192 || height.as_integer() > 8192 || width.as_integer() * height.as_integer() > 16000000)
        throw std::invalid_argument("notebook.show image dimensions must fit 8192 px per side and 16 megapixels");
      display.width = static_cast<std::uint32_t>(width.as_integer());
      display.height = static_cast<std::uint32_t>(height.as_integer());
      Value png;
      if (name == "plot.Rendered") {
        png = instance_ivar_value_or_null(object, "bytes");
        const auto mime = string_text_from_value(instance_ivar_value_or_null(object, "mime"));
        if (!mime || *mime != "image/png") throw std::invalid_argument("notebook.show Rendered must be PNG; use render(format: :png)");
      } else {
        const auto *method = find_method_for_dispatch(frame, object->class_index, "bytes", kMethodFlagInstance);
        if (fault_) return SendStatus::Faulted;
        if (!method) throw std::invalid_argument("plot figure is missing bytes(format:)");
        const std::vector<std::pair<std::uint32_t, Value>> keywords = {
            {intern_runtime_symbol("format"), Value::symbol(intern_runtime_symbol("png"))}};
        auto rendered = execute_method_to_value(frame, *method, {}, keywords, args[0], Value::null(), "notebook.show rendering");
        if (!rendered) return SendStatus::Faulted;
        png = std::move(*rendered);
      }
      const auto bytes = stdlib_bytes_of(&frame, png);
      if (!bytes) return SendStatus::Faulted;
      display.bytes = *bytes;
      const auto &data = display.bytes;
      auto u32 = [&](std::size_t offset) {
        std::uint32_t result = 0;
        for (unsigned i = 0; i < 4; ++i) result = (result << 8U) | static_cast<unsigned char>(data[offset + i]);
        return result;
      };
      if (data.size() < 33 || data.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) != 0 ||
          data.compare(12, 4, "IHDR") != 0 || u32(16) != display.width || u32(20) != display.height)
        throw std::invalid_argument("plot returned an invalid or mismatched PNG");
      if (name == "plot.Figure") {
        const auto *scene_method = find_method_for_dispatch(frame, object->class_index, "notebook_scene", kMethodFlagInstance);
        if (fault_) return SendStatus::Faulted;
        if (scene_method) {
          auto scene = execute_method_to_value(frame, *scene_method, {}, {}, args[0], Value::null(), "notebook.show scene");
          if (!scene) return SendStatus::Faulted;
          std::size_t nodes = 0, estimated_bytes = 0;
          // Large/unknown scenes retain their PNG; interactive viewing is an
          // optional bounded capability, never a handle back into the VM.
          if (notebook_scene_bounded(*scene, 0, nodes, estimated_bytes)) {
            if (!http_json_generate_text(frame, *scene, &display.plot_scene)) return SendStatus::Faulted;
            if (display.plot_scene.size() > 4U * 1024U * 1024U) display.plot_scene.clear();
          }
        }
      }
      }
      display.id = live_id;
      if (live) {
        if (!sink || !sink->active()) {
          set_fault(frame, "NotebookCellError", "live figure requires the active cell's render context");
          return SendStatus::Faulted;
        }
        sink->replace(display, key, live_id);
        const auto status = live_sink->publish_figure(
            live_cell_id, std::move(live_id), std::move(display), throttle_ms,
            NotebookLiveStore::monotonic_millis(), live_generation);
        if (status == NotebookLivePublishResult::Throttled) {
          *out = Value::null();
          return SendStatus::Matched;
        }
        if (status == NotebookLivePublishResult::Closed) {
          set_fault(frame, "NotebookCellError", "notebook.show live run is closed or stale");
          return SendStatus::Faulted;
        }
        if (status != NotebookLivePublishResult::Accepted)
          throw std::invalid_argument("notebook.show live figure exceeds run limits");
      } else {
        sink->replace(std::move(display), key, live_id);
      }
      *out = Value::null();
      return SendStatus::Matched;
    } catch (const std::exception &error) {
      set_fault(frame, "TypeError", error.what());
      return SendStatus::Faulted;
    }
  }
