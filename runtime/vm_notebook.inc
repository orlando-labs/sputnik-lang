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
    if (selector == "input") {
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
      const auto value = key ? capture->read(*key) : std::nullopt;
      if (!value) {
        set_fault(frame, "NotebookCellError", "unknown or unavailable project input: " + key.value_or("<invalid>"));
        return SendStatus::Faulted;
      }
      if (const auto v = std::get_if<bool>(&*value)) *out = Value::boolean(*v);
      else if (const auto v = std::get_if<std::int64_t>(&*value)) *out = Value::integer(*v);
      else if (const auto v = std::get_if<double>(&*value)) *out = Value::floating(*v);
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
    if (selector != "show") return SendStatus::NotHandled;
    if (args.size() != 1 || !block.is_null() ||
        !stdlib_reject_unknown_keywords(&frame, kw_args,
                                        {"caption", "order", "id", "live", "throttle_ms"})) {
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
        if (live_id.empty()) throw std::invalid_argument("notebook.show id must not be empty");
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
        throw std::invalid_argument("notebook.show currently supports sputnik-plot Figure, Figure3D and PNG Rendered objects");
      const auto object = args[0].as_instance_object();
      const auto name = class_name_for_index(object->class_index).value_or("");
      if (name != "plot.Figure" && name != "plot.Figure3D" && name != "plot.Rendered")
        throw std::invalid_argument("notebook.show object is not an sputnik-plot figure");
      if (!ensure_lifecycle_access(frame, args[0])) return SendStatus::Faulted;
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
        if (!method) throw std::invalid_argument("sputnik-plot figure is missing bytes(format:)");
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
        throw std::invalid_argument("sputnik-plot returned an invalid or mismatched PNG");
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
        sink->append(std::move(display), key);
      }
      *out = Value::null();
      return SendStatus::Matched;
    } catch (const std::exception &error) {
      set_fault(frame, "TypeError", error.what());
      return SendStatus::Faulted;
    }
  }
